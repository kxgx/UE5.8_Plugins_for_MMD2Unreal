// Copyright Epic Games, Inc. All Rights Reserved.

#include "MRQAutoSegmentCore.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "RHI.h"
#include "RHIGlobals.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "LevelSequence.h"
#include "MoviePipelineBasicConfig.h"
#include "MoviePipelineExecutor.h"
#include "MoviePipelineQueue.h"
#include "MovieScene.h"
#include "Graph/MovieGraphConfig.h"
#include "Graph/MovieGraphNode.h"
#include "Graph/MovieGraphTraversalContext.h"
#include "Graph/Nodes/MovieGraphFileOutputNode.h"
#include "Graph/Nodes/MovieGraphGlobalOutputSettingNode.h"
#include "Graph/MovieGraphNamedResolution.h"
#include "Graph/MovieGraphProjectSettings.h"

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

int32 FMRQAutoSegmentCore::GetBytesPerPixelForOutputType(const UClass* InOutputType, FString& OutReason)
{
	if (InOutputType == nullptr)
	{
		OutReason = TEXT("没选输出格式，按 8-bit RGBA 估");
		return 4;
	}

	const FString ClassName = InOutputType->GetName();

	// --- 16 bits per channel: 4 channels x 2 bytes ------------------------------------------
	if (ClassName.Contains(TEXT("HEVC10")))
	{
		OutReason = TEXT("HEVC 10-bit 按半精度 RGBA 走管线，每通道 2 字节");
		return 8;
	}
	if (ClassName.Contains(TEXT("EXR")))
	{
		OutReason = TEXT("EXR 默认半精度 RGBA，每通道 2 字节");
		return 8;
	}

	// --- 8 bits per channel: 4 channels x 1 byte --------------------------------------------
	if (ClassName.Contains(TEXT("MP4")) || ClassName.Contains(TEXT("CommandLineEncoder")))
	{
		OutReason = TEXT("MP4 / 命令行编码器按 8-bit RGBA 走管线，每通道 1 字节");
		return 4;
	}
	if (ClassName.Contains(TEXT("PNG")) || ClassName.Contains(TEXT("TGA")))
	{
		OutReason = TEXT("PNG / TGA 为 8-bit RGBA，每通道 1 字节");
		return 4;
	}
	if (ClassName.Contains(TEXT("JPG")) || ClassName.Contains(TEXT("JPEG")))
	{
		OutReason = TEXT("JPEG 没有 alpha，按 8-bit RGB 算");
		return 3;
	}
	if (ClassName.Contains(TEXT("BMP")))
	{
		OutReason = TEXT("BMP 为 8-bit，每通道 1 字节");
		return 3;
	}

	OutReason = FString::Printf(TEXT("认不出 '%s' 的像素格式，按 8-bit RGBA 估"), *ClassName);
	return 4;
}

FMRQHardwareBudget FMRQAutoSegmentCore::ProbeHardware()
{
	FMRQHardwareBudget Budget;

	// --- System RAM -----------------------------------------------------------------------
	const FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();
	Budget.TotalPhysicalRAM = static_cast<int64>(MemStats.TotalPhysical);
	Budget.AvailablePhysicalRAM = static_cast<int64>(MemStats.AvailablePhysical);

	// --- Video memory ---------------------------------------------------------------------
	// The RHI can only report what this process has allocated, so AvailableVRAM is an upper
	// bound: memory held by other applications is invisible from in here. The planner therefore
	// only ever uses a fraction of it - see RAMUseLimit / VRAMUseLimit, 80% by default.
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

	UE_LOG(LogMRQAutoSegment, Log,
		TEXT("Hardware: RAM %s free / %s total, VRAM %s free / %s total (engine using %s)%s"),
		*FormatBytes(Budget.AvailablePhysicalRAM), *FormatBytes(Budget.TotalPhysicalRAM),
		*FormatBytes(Budget.AvailableVRAM), *FormatBytes(Budget.TotalVRAM), *FormatBytes(Budget.EngineUsedVRAM),
		Budget.bVRAMKnown ? TEXT("") : TEXT(" [VRAM unknown]"));

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

	// The render is held to a fraction of what is *currently free*, so the rest stays available to
	// the OS, to other applications and to the editor itself.
	const float RAMLimit = FMath::Clamp(InRequest.RAMUseLimit, 0.05f, 1.0f);
	const float VRAMLimit = FMath::Clamp(InRequest.VRAMUseLimit, 0.05f, 1.0f);

	const int64 UsableRAM = FMath::Max<int64>(
		static_cast<int64>(static_cast<double>(InBudget.AvailablePhysicalRAM) * RAMLimit), 0);
	const int64 UsableVRAM = FMath::Max<int64>(
		static_cast<int64>(static_cast<double>(InBudget.AvailableVRAM) * VRAMLimit), 0);

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
			Row.Detail = FString::Printf(TEXT("空闲的 %d%% = %s ÷ 每帧约 %s"),
				FMath::RoundToInt(RAMLimit * 100.0f), *FormatBytes(UsableRAM), *FormatBytes(RAMPerFrame));
			Row.bBinding = (Binding == EMRQSegmentBound::SystemRAM);
			Plan.Constraints.Add(Row);
		}

		{
			FMRQSegmentConstraint Row;
			Row.Name = TEXT("显存");
			Row.Frames = FramesByVRAM;
			if (InBudget.bVRAMKnown)
			{
				Row.Detail = FString::Printf(TEXT("空闲的 %d%% = %s ÷ 每帧约 %s"),
					FMath::RoundToInt(VRAMLimit * 100.0f), *FormatBytes(UsableVRAM), *FormatBytes(VRAMPerFrame));
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

		// Every job points at the sequence the user picked. The per-segment frame range lives on
		// the job's Basic config and nowhere else - the plugin deliberately does not write extra
		// sequence assets, because copies of the user's sequence end up being identical to each
		// other and the render queue then repeats the first job.
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

		// One folder per segment: <output directory>/<frame range>/
		if (!InTemplate.OutputDirectory.IsEmpty())
		{
			Basic->bOverride_OutputDirectory = true;
			Basic->OutputDirectory.Path = GetSegmentOutputFolder(InTemplate.OutputDirectory, Entry.Label);
		}

		Basic->bOverride_FileNameFormat = true;
		Basic->FileNameFormat = AppendRangeToPattern(InTemplate.FileNameFormat, Entry.Label);

		// The engine stores the range as a half open [Start, End) interval and feeds these
		// straight into TRange::SetPlaybackRange, so the end frame has to be exclusive.
		// Passing the inclusive end here loses the last frame of every segment.
		Basic->bOverride_CustomStartFrame = true;
		Basic->CustomStartFrame = Entry.StartFrame;
		Basic->bOverride_CustomEndFrame = true;
		Basic->CustomEndFrame = Entry.EndFrame + 1;

		// Copy the project's own named resolution onto the job, verbatim.
		//
		// Writing a hand built "Custom" entry here instead is what made the render queue repeat
		// the first job: the job's Basic config ended up carrying a resolution the queue was not
		// prepared for. Using an entry straight out of
		// UMovieGraphProjectSettings::DefaultNamedResolutions avoids that entirely.
		if (!InTemplate.ResolutionProfile.IsNone())
		{
			const UMovieGraphProjectSettings* Settings = GetDefault<UMovieGraphProjectSettings>();
			const FMovieGraphNamedResolution* Match = Settings != nullptr
				? Settings->DefaultNamedResolutions.FindByPredicate(
					[&InTemplate](const FMovieGraphNamedResolution& Preset)
					{
						return Preset.ProfileName == InTemplate.ResolutionProfile;
					})
				: nullptr;

			if (Match != nullptr)
			{
				Basic->bOverride_OutputResolution = true;
				Basic->OutputResolution = *Match;
			}
			else
			{
				UE_LOG(LogMRQAutoSegment, Warning,
					TEXT("Named resolution '%s' is not in the project settings; leaving the resolution to the graph."),
					*InTemplate.ResolutionProfile.ToString());
			}
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

		// Temporal samples only. They live on the graph's sampling method node on the Globals
		// branch, so nothing about the renderer has to be forced - unlike the spatial sample
		// count, which is a deferred-renderer property.
		Basic->bOverride_TemporalSampleCount = true;
		Basic->TemporalSampleCount = FMath::Max(InTemplate.TemporalSampleCount, 1);

		UE_LOG(LogMRQAutoSegment, Log, TEXT("Created job '%s'  frames=%d..%d  dir=%s  file=%s"),
			*Job->JobName, Entry.StartFrame, Entry.EndFrame,
			*Basic->OutputDirectory.Path, *Basic->FileNameFormat);

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

int32 FMRQAutoSegmentCore::DumpGeneratedGraphs(UMoviePipelineQueue* InQueue, const FString& InJobNamePrefix)
{
	if (InQueue == nullptr)
	{
		return 0;
	}

	int32 Inspected = 0;
	for (UMoviePipelineExecutorJob* Job : InQueue->GetJobs())
	{
		if (Job == nullptr || !Job->JobName.StartsWith(InJobNamePrefix))
		{
			continue;
		}

		UMoviePipelineBasicConfig* Basic = Job->GetBasicConfig();
		UE_LOG(LogMRQAutoSegment, Display, TEXT("[dump] job '%s'  config: start=%d(ovr=%d) end=%d(ovr=%d) file='%s'"),
			*Job->JobName,
			Basic ? Basic->CustomStartFrame : -1, (Basic && Basic->bOverride_CustomStartFrame) ? 1 : 0,
			Basic ? Basic->CustomEndFrame : -1, (Basic && Basic->bOverride_CustomEndFrame) ? 1 : 0,
			Basic ? *Basic->FileNameFormat : TEXT("<no basic config>"));

		// This is exactly what the renderer does before it applies the range to the sequence.
		UMovieGraphConfig* Graph = UMoviePipelineBasicConfig::GenerateGraph(Basic, GetTransientPackage());
		if (Graph == nullptr)
		{
			UE_LOG(LogMRQAutoSegment, Warning, TEXT("[dump]   GenerateGraph returned null"));
			continue;
		}

		FMovieGraphTraversalContext Context;
		Context.Job = Job;

		FString Error;
		const UMovieGraphEvaluatedConfig* Flat = Graph->CreateFlattenedGraph(Context, Error);
		if (Flat == nullptr)
		{
			UE_LOG(LogMRQAutoSegment, Warning, TEXT("[dump]   flatten failed: %s"), *Error);
			continue;
		}

		const UMovieGraphGlobalOutputSettingNode* Out =
			Flat->GetSettingForBranch<UMovieGraphGlobalOutputSettingNode>(UMovieGraphNode::GlobalsPinName, true, false);
		if (Out == nullptr)
		{
			UE_LOG(LogMRQAutoSegment, Warning, TEXT("[dump]   no global output setting node in the evaluated graph"));
			continue;
		}

		UE_LOG(LogMRQAutoSegment, Display,
			TEXT("[dump]   graph: start=%d(ovr=%d type=%d)  end=%d(ovr=%d type=%d)"),
			Out->CustomPlaybackRangeStart.Value, Out->bOverride_CustomPlaybackRangeStart ? 1 : 0,
			static_cast<int32>(Out->CustomPlaybackRangeStart.Type),
			Out->CustomPlaybackRangeEnd.Value, Out->bOverride_CustomPlaybackRangeEnd ? 1 : 0,
			static_cast<int32>(Out->CustomPlaybackRangeEnd.Type));

		++Inspected;
	}

	UE_LOG(LogMRQAutoSegment, Display, TEXT("[dump] inspected %d job(s)"), Inspected);
	return Inspected;
}

FString FMRQAutoSegmentCore::GetSegmentOutputFolder(const FString& InOutputDirectory, const FString& InLabel)
{
	return FPaths::Combine(InOutputDirectory, InLabel);
}
