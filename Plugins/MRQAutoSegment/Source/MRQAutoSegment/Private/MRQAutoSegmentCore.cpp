// Copyright Epic Games, Inc. All Rights Reserved.

#include "MRQAutoSegmentCore.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "RHIGlobals.h"

#include "LevelSequence.h"
#include "MoviePipelineBasicConfig.h"
#include "MoviePipelineExecutor.h"
#include "MoviePipelineQueue.h"
#include "Graph/Nodes/MovieGraphFileOutputNode.h"
#include "Graph/MovieGraphNamedResolution.h"

DEFINE_LOG_CATEGORY_STATIC(LogMRQAutoSegment, Log, All);

namespace
{
	/** A limit that does not restrict anything, for constraints that are reported only. */
	constexpr int32 kNoLimit = MAX_int32;
}

FString FMRQAutoSegmentCore::FormatBytes(int64 InBytes)
{
	static const TCHAR* Units[] = { TEXT("B"), TEXT("KiB"), TEXT("MiB"), TEXT("GiB"), TEXT("TiB") };

	if (InBytes <= 0)
	{
		return TEXT("0 B");
	}

	double Value = static_cast<double>(InBytes);
	int32 UnitIndex = 0;
	while (Value >= 1024.0 && UnitIndex < 4)
	{
		Value /= 1024.0;
		++UnitIndex;
	}

	if (UnitIndex == 0)
	{
		return FString::Printf(TEXT("%lld B"), InBytes);
	}
	return FString::Printf(TEXT("%.2f %s"), Value, Units[UnitIndex]);
}

FMRQHardwareBudget FMRQAutoSegmentCore::ProbeHardware(const FString& InOutputDirectory)
{
	FMRQHardwareBudget Budget;

	// --- System RAM -----------------------------------------------------------------------
	const FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();
	Budget.TotalPhysicalRAM = static_cast<int64>(MemStats.TotalPhysical);
	Budget.AvailablePhysicalRAM = static_cast<int64>(MemStats.AvailablePhysical);

	// --- Video memory ---------------------------------------------------------------------
	// The RHI can only report what this process has allocated, so AvailableVRAM is an upper
	// bound: memory held by other applications is invisible from in here. The planner keeps a
	// reserve fraction for exactly that reason.
	if (GDynamicRHI != nullptr)
	{
		FTextureMemoryStats TexStats;
		RHIGetTextureMemoryStats(TexStats);

		Budget.TotalVRAM = TexStats.GetTotalDeviceWorkingMemory();
		Budget.EngineUsedVRAM = static_cast<int64>(TexStats.StreamingMemorySize + TexStats.NonStreamingMemorySize);
		Budget.bVRAMKnown = Budget.TotalVRAM > 0;
		Budget.AvailableVRAM = Budget.bVRAMKnown
			? FMath::Max<int64>(Budget.TotalVRAM - Budget.EngineUsedVRAM, 0)
			: 0;
	}

	Budget.AdapterName = GRHIAdapterName;

	// --- Free disk space ------------------------------------------------------------------
	// GetDiskTotalAndFreeSpace wants a path that exists, so walk up until we find one.
	FString ProbePath = InOutputDirectory;
	if (ProbePath.IsEmpty())
	{
		ProbePath = FPaths::ProjectDir();
	}
	ProbePath = FPaths::ConvertRelativePathToFull(ProbePath);

	while (!ProbePath.IsEmpty() && !IFileManager::Get().DirectoryExists(*ProbePath))
	{
		const FString Parent = FPaths::GetPath(ProbePath);
		if (Parent.IsEmpty() || Parent == ProbePath)
		{
			break;
		}
		ProbePath = Parent;
	}

	uint64 TotalBytes = 0;
	uint64 FreeBytes = 0;
	if (!ProbePath.IsEmpty() && FPlatformMisc::GetDiskTotalAndFreeSpace(ProbePath, TotalBytes, FreeBytes))
	{
		Budget.FreeDiskBytes = static_cast<int64>(FreeBytes);
	}

	UE_LOG(LogMRQAutoSegment, Log,
		TEXT("Hardware: RAM %s free / %s total, VRAM %s free / %s total (engine using %s)%s, disk %s free on %s"),
		*FormatBytes(Budget.AvailablePhysicalRAM), *FormatBytes(Budget.TotalPhysicalRAM),
		*FormatBytes(Budget.AvailableVRAM), *FormatBytes(Budget.TotalVRAM), *FormatBytes(Budget.EngineUsedVRAM),
		Budget.bVRAMKnown ? TEXT("") : TEXT(" [VRAM unknown]"),
		*FormatBytes(Budget.FreeDiskBytes), *ProbePath);

	return Budget;
}

FString FMRQAutoSegmentCore::MakeRangeLabel(int32 StartFrame, int32 EndFrame, int32 PadDigits)
{
	const int32 Pad = FMath::Clamp(PadDigits, 1, 10);
	return FString::Printf(TEXT("%0*d-%0*d"), Pad, StartFrame, Pad, EndFrame);
}

FString FMRQAutoSegmentCore::AppendRangeToPattern(const FString& InPattern, const FString& InRangeLabel)
{
	FString Pattern = InPattern;
	if (Pattern.IsEmpty())
	{
		Pattern = TEXT("{sequence_name}");
	}
	return FString::Printf(TEXT("%s_%s"), *Pattern, *InRangeLabel);
}

FString FMRQAutoSegmentCore::PreviewFileName(const FString& InPattern, const FString& InRangeLabel, const FString& InSequenceName, const FString& InExtension)
{
	FString Name = AppendRangeToPattern(InPattern, InRangeLabel);

	// Resolve only the tokens we can know before a render starts; anything else
	// ({date}, {version}, ...) is left as-is so it is obvious it comes from the engine.
	if (!InSequenceName.IsEmpty())
	{
		Name.ReplaceInline(TEXT("{sequence_name}"), *InSequenceName);
		Name.ReplaceInline(TEXT("{shot_name}"), *InSequenceName);
	}

	return Name + InExtension;
}

FMRQSegmentPlan FMRQAutoSegmentCore::BuildPlan(const FMRQSegmentRequest& InRequest, const FMRQHardwareBudget& InBudget)
{
	FMRQSegmentPlan Plan;

	const int32 TotalFrames = InRequest.RangeEnd - InRequest.RangeStart + 1;
	Plan.TotalFrames = FMath::Max(TotalFrames, 0);
	if (TotalFrames <= 0)
	{
		Plan.Message = TEXT("帧范围为空：结束帧必须大于等于起始帧。");
		return Plan;
	}

	const int32 BytesPerPixel = FMath::Max(InRequest.BytesPerPixel, 1);
	const int64 FrameBufferBytes = FMath::Max<int64>(
		static_cast<int64>(InRequest.Resolution.X) * static_cast<int64>(InRequest.Resolution.Y) * BytesPerPixel, 1);

	// How much one rendered frame is assumed to add to each pool. These are estimates, and the
	// panel shows the resulting arithmetic so the numbers can be argued with rather than trusted.
	const int64 RAMPerFrame = FMath::Max<int64>(
		static_cast<int64>(static_cast<double>(FrameBufferBytes) * FMath::Max(InRequest.RAMGrowthPerFrame, 0.0f)), 1);
	const int64 VRAMPerFrame = FMath::Max<int64>(
		static_cast<int64>(static_cast<double>(FrameBufferBytes) * FMath::Max(InRequest.VRAMGrowthPerFrame, 0.0f)), 1);

	const int64 UsableRAM = FMath::Max<int64>(
		static_cast<int64>(static_cast<double>(InBudget.AvailablePhysicalRAM) * (1.0 - FMath::Clamp(InRequest.RAMReserveFraction, 0.0f, 0.95f))), 0);
	const int64 UsableVRAM = FMath::Max<int64>(
		static_cast<int64>(static_cast<double>(InBudget.AvailableVRAM) * (1.0 - FMath::Clamp(InRequest.VRAMReserveFraction, 0.0f, 0.95f))), 0);

	const int32 FramesByRAM = static_cast<int32>(FMath::Clamp<int64>(UsableRAM / RAMPerFrame, 1, kNoLimit));
	const int32 FramesByVRAM = InBudget.bVRAMKnown
		? static_cast<int32>(FMath::Clamp<int64>(UsableVRAM / VRAMPerFrame, 1, kNoLimit))
		: kNoLimit;
	const int32 FramesByCap = FMath::Max(InRequest.MaxSegmentLength, 1);

	int32 FramesPerSegment = TotalFrames;
	EMRQSegmentBound Binding = EMRQSegmentBound::None;

	switch (InRequest.Mode)
	{
	case EMRQSegmentMode::FixedCount:
	{
		const int32 Count = FMath::Max(InRequest.FixedSegmentCount, 1);
		FramesPerSegment = FMath::DivideAndRoundUp(TotalFrames, Count);
		Binding = EMRQSegmentBound::None;

		FMRQSegmentConstraint Row;
		Row.Name = TEXT("用户指定段数");
		Row.Frames = FramesPerSegment;
		Row.Detail = FString::Printf(TEXT("%d 帧 / %d 段"), TotalFrames, Count);
		Row.bBinding = true;
		Plan.Constraints.Add(Row);
		break;
	}

	case EMRQSegmentMode::FixedLength:
	{
		FramesPerSegment = FMath::Max(InRequest.FixedSegmentLength, 1);
		Binding = EMRQSegmentBound::None;

		FMRQSegmentConstraint Row;
		Row.Name = TEXT("用户指定长度");
		Row.Frames = FramesPerSegment;
		Row.Detail = FString::Printf(TEXT("固定 %d 帧每段"), FramesPerSegment);
		Row.bBinding = true;
		Plan.Constraints.Add(Row);
		break;
	}

	case EMRQSegmentMode::Auto:
	default:
	{
		FramesPerSegment = FMath::Min3(FramesByRAM, FramesByVRAM, FramesByCap);

		if (FramesPerSegment == FramesByVRAM)      { Binding = EMRQSegmentBound::VideoRAM; }
		else if (FramesPerSegment == FramesByRAM)  { Binding = EMRQSegmentBound::SystemRAM; }
		else                                       { Binding = EMRQSegmentBound::UserCap; }

		{
			FMRQSegmentConstraint Row;
			Row.Name = TEXT("系统内存");
			Row.Frames = FramesByRAM;
			Row.Detail = FString::Printf(TEXT("可用 %s（已扣 %d%% 保留）÷ 每帧约 %s"),
				*FormatBytes(UsableRAM), FMath::RoundToInt(FMath::Clamp(InRequest.RAMReserveFraction, 0.0f, 0.95f) * 100.0f),
				*FormatBytes(RAMPerFrame));
			Row.bBinding = (Binding == EMRQSegmentBound::SystemRAM);
			Plan.Constraints.Add(Row);
		}

		{
			FMRQSegmentConstraint Row;
			Row.Name = TEXT("显存");
			Row.Frames = FramesByVRAM;
			if (InBudget.bVRAMKnown)
			{
				Row.Detail = FString::Printf(TEXT("可用 %s（已扣 %d%% 保留）÷ 每帧约 %s"),
					*FormatBytes(UsableVRAM), FMath::RoundToInt(FMath::Clamp(InRequest.VRAMReserveFraction, 0.0f, 0.95f) * 100.0f),
					*FormatBytes(VRAMPerFrame));
			}
			else
			{
				Row.Detail = TEXT("RHI 未报告显存，本项不参与限制");
			}
			Row.bBinding = (Binding == EMRQSegmentBound::VideoRAM);
			Plan.Constraints.Add(Row);
		}

		{
			FMRQSegmentConstraint Row;
			Row.Name = TEXT("长度上限");
			Row.Frames = FramesByCap;
			Row.Detail = FString::Printf(TEXT("用户设置每段最长 %d 帧"), FramesByCap);
			Row.bBinding = (Binding == EMRQSegmentBound::UserCap);
			Plan.Constraints.Add(Row);
		}
		break;
	}
	}

	FramesPerSegment = FMath::Clamp(FramesPerSegment, 1, TotalFrames);
	Plan.FramesPerSegment = FramesPerSegment;
	Plan.SegmentCount = FMath::DivideAndRoundUp(TotalFrames, FramesPerSegment);
	Plan.Binding = Binding;

	// --- Disk: reported, but it does not shorten a segment, it limits the whole render -------
	if (InRequest.bLimitByDisk && InBudget.FreeDiskBytes > 0)
	{
		const int64 DiskBudget = static_cast<int64>(
			static_cast<double>(InBudget.FreeDiskBytes) * FMath::Clamp(InRequest.DiskUseFraction, 0.0f, 0.95f));
		const int64 EstimatedTotal = FrameBufferBytes * static_cast<int64>(TotalFrames);
		const bool bFits = EstimatedTotal <= DiskBudget;

		FMRQSegmentConstraint Row;
		Row.Name = TEXT("磁盘空间");
		Row.Frames = kNoLimit;
		Row.Detail = FString::Printf(TEXT("整段预计 %s / 可用 %s（可用额度 %s）"),
			*FormatBytes(EstimatedTotal), *FormatBytes(InBudget.FreeDiskBytes), *FormatBytes(DiskBudget));
		Row.bBinding = false;
		Plan.Constraints.Add(Row);

		if (!bFits)
		{
			Plan.Message = FString::Printf(
				TEXT("警告：整段输出预计需要 %s，超过磁盘可用额度 %s。建议减小范围、降低分辨率或改输出格式。"),
				*FormatBytes(EstimatedTotal), *FormatBytes(DiskBudget));
		}
	}

	// --- Materialise the segments -----------------------------------------------------------
	const int32 PadDigits = FMath::Clamp(InRequest.LabelPadDigits, 1, 10);
	Plan.Segments.Reserve(Plan.SegmentCount);

	int32 Cursor = InRequest.RangeStart;
	for (int32 Index = 0; Index < Plan.SegmentCount; ++Index)
	{
		FMRQSegmentPlanEntry Entry;
		Entry.Index = Index;
		Entry.StartFrame = Cursor;
		Entry.EndFrame = FMath::Min(Cursor + FramesPerSegment - 1, InRequest.RangeEnd);
		Entry.FrameCount = Entry.EndFrame - Entry.StartFrame + 1;
		Entry.Label = MakeRangeLabel(Entry.StartFrame, Entry.EndFrame, PadDigits);
		Plan.Segments.Add(Entry);
		Cursor = Entry.EndFrame + 1;
	}

	Plan.bValid = true;
	if (Plan.Message.IsEmpty())
	{
		Plan.Message = FString::Printf(TEXT("%d 帧 → %d 段，每段 %d 帧"),
			Plan.TotalFrames, Plan.SegmentCount, Plan.FramesPerSegment);
	}

	UE_LOG(LogMRQAutoSegment, Log, TEXT("Plan: %s (binding=%d)"), *Plan.Message, static_cast<int32>(Plan.Binding));
	return Plan;
}

int32 FMRQAutoSegmentCore::GenerateJobs(UMoviePipelineQueue* InQueue, ULevelSequence* InSequence, const FString& InMapPath, const FMRQSegmentPlan& InPlan, const FMRQJobTemplate& InTemplate)
{
	if (InQueue == nullptr || !InPlan.bValid || InPlan.Segments.Num() == 0)
	{
		return 0;
	}

	int32 Created = 0;
	for (const FMRQSegmentPlanEntry& Entry : InPlan.Segments)
	{
		UMoviePipelineExecutorJob* Job = InQueue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
		if (Job == nullptr)
		{
			continue;
		}

		Job->JobName = FString::Printf(TEXT("%s %s"), *InTemplate.JobNamePrefix, *Entry.Label);
		Job->Comment = FString::Printf(TEXT("Frames %d-%d (%d frames)"), Entry.StartFrame, Entry.EndFrame, Entry.FrameCount);
		Job->SetConsumed(false);
		Job->SetIsEnabled(true);

		if (InSequence != nullptr)
		{
			Job->Sequence = FSoftObjectPath(InSequence);
		}
		if (!InMapPath.IsEmpty())
		{
			Job->Map = FSoftObjectPath(InMapPath);
		}

		// Basic mode builds its UMovieGraphConfig just-in-time, which is what makes the file
		// name and the playback range settable per job without duplicating the user's graph.
		UMoviePipelineBasicConfig* Basic = Job->SetupBasicConfiguration();
		if (Basic == nullptr)
		{
			UE_LOG(LogMRQAutoSegment, Warning, TEXT("Job '%s' has no Basic config; skipping."), *Job->JobName);
			continue;
		}

		if (!InTemplate.OutputDirectory.IsEmpty())
		{
			Basic->bOverride_OutputDirectory = true;
			Basic->OutputDirectory.Path = InTemplate.OutputDirectory;
		}

		Basic->bOverride_FileNameFormat = true;
		Basic->FileNameFormat = AppendRangeToPattern(InTemplate.FileNameFormat, Entry.Label);

		Basic->bOverride_CustomStartFrame = true;
		Basic->CustomStartFrame = Entry.StartFrame;
		Basic->bOverride_CustomEndFrame = true;
		Basic->CustomEndFrame = Entry.EndFrame;

		if (InTemplate.Resolution.X > 0 && InTemplate.Resolution.Y > 0)
		{
			Basic->bOverride_OutputResolution = true;
			Basic->OutputResolution = FMovieGraphNamedResolution(
				FMovieGraphNamedResolution::CustomEntryName, InTemplate.Resolution, FString());
		}

		if (InTemplate.OutputTypes.Num() > 0)
		{
			Basic->bOverride_EnabledOutputTypes = true;
			Basic->EnabledOutputTypes.Reset();
			for (UClass* OutputType : InTemplate.OutputTypes)
			{
				if (OutputType != nullptr && OutputType->IsChildOf(UMovieGraphFileOutputNode::StaticClass()))
				{
					Basic->EnabledOutputTypes.Add(TSoftClassPtr<UMovieGraphFileOutputNode>(OutputType));
				}
			}
		}

		if (InTemplate.bSetSampleCounts)
		{
			Basic->bUseDeferredRenderer = true;
			Basic->bOverride_DeferredSpatialSampleCount = true;
			Basic->DeferredSpatialSampleCount = FMath::Max(InTemplate.SpatialSampleCount, 1);
			Basic->bOverride_TemporalSampleCount = true;
			Basic->TemporalSampleCount = FMath::Max(InTemplate.TemporalSampleCount, 1);
		}

		UE_LOG(LogMRQAutoSegment, Log, TEXT("Created job '%s'  frames=%d..%d  file=%s"),
			*Job->JobName, Entry.StartFrame, Entry.EndFrame, *Basic->FileNameFormat);

		++Created;
	}

	return Created;
}

int32 FMRQAutoSegmentCore::DeleteGeneratedJobs(UMoviePipelineQueue* InQueue, const FString& InJobNamePrefix)
{
	if (InQueue == nullptr || InJobNamePrefix.IsEmpty())
	{
		return 0;
	}

	TArray<UMoviePipelineExecutorJob*> ToDelete;
	for (UMoviePipelineExecutorJob* Job : InQueue->GetJobs())
	{
		if (Job != nullptr && Job->JobName.StartsWith(InJobNamePrefix))
		{
			ToDelete.Add(Job);
		}
	}

	for (UMoviePipelineExecutorJob* Job : ToDelete)
	{
		InQueue->DeleteJob(Job);
	}

	return ToDelete.Num();
}
