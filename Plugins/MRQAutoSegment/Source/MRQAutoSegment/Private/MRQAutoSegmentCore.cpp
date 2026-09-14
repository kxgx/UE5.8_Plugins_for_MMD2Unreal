// Copyright Epic Games, Inc. All Rights Reserved.

#include "MRQAutoSegmentCore.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "Misc/PackageName.h"
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

DEFINE_LOG_CATEGORY_STATIC(LogMRQAutoSegment, Log, All);

namespace
{
	/** A limit that does not restrict anything, for constraints that are reported only. */
	constexpr int32 kNoLimit = MAX_int32;

	/**
	 * Writes a copy of InSource whose playback range is exactly one segment.
	 *
	 * The copy is a real asset under MRQ_AUTOSEGMENT_SEGMENT_ROOT rather than a transient object,
	 * because the render may run in a separate process ("Render (New Process)"), where a
	 * transient object would not exist.
	 *
	 * @param InEndFrameInclusive  Last frame of the segment, inclusive. Stored half open.
	 * @return The new sequence, or nullptr if the asset could not be written.
	 */
	ULevelSequence* MakeSegmentSequence(ULevelSequence* InSource, int32 InStartFrame, int32 InEndFrameInclusive, const FString& InLabel)
	{
		if (InSource == nullptr || InSource->GetMovieScene() == nullptr)
		{
			return nullptr;
		}

		const FString AssetName = FString::Printf(TEXT("%s_%s"), *InSource->GetName(), *InLabel);
		const FString PackageName = FString::Printf(TEXT("%s/%s"), MRQ_AUTOSEGMENT_SEGMENT_ROOT, *AssetName);

		UPackage* Package = CreatePackage(*PackageName);
		if (Package == nullptr)
		{
			return nullptr;
		}

		// Re-generating the same queue must not trip over the previous run's assets.
		if (ULevelSequence* Existing = FindObject<ULevelSequence>(Package, *AssetName))
		{
			Existing->ClearFlags(RF_Public | RF_Standalone);
			Existing->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		}

		ULevelSequence* Segment = DuplicateObject<ULevelSequence>(InSource, Package, FName(*AssetName));
		if (Segment == nullptr || Segment->GetMovieScene() == nullptr)
		{
			return nullptr;
		}

		UMovieScene* Scene = Segment->GetMovieScene();

		// The segment bounds are display-rate frames; the playback range is in tick resolution
		// and is half open, so the inclusive end frame becomes End + 1.
		const FFrameRate DisplayRate = Scene->GetDisplayRate();
		const FFrameRate TickResolution = Scene->GetTickResolution();
		const FFrameNumber StartTick =
			FFrameRate::TransformTime(FFrameTime(FFrameNumber(InStartFrame)), DisplayRate, TickResolution).FloorToFrame();
		const FFrameNumber EndTick =
			FFrameRate::TransformTime(FFrameTime(FFrameNumber(InEndFrameInclusive + 1)), DisplayRate, TickResolution).CeilToFrame();

		Scene->SetPlaybackRangeLocked(false);
#if WITH_EDITOR
		Scene->SetReadOnly(false);
#endif
		Scene->SetPlaybackRange(TRange<FFrameNumber>(StartTick, EndTick));

		FAssetRegistryModule::AssetCreated(Segment);
		Package->MarkPackageDirty();

		const FString FileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		UPackage::SavePackage(Package, Segment, *FileName, SaveArgs);

		return Segment;
	}
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

		// Every job pointing at the same sequence asset makes MRQ apply the per-job playback range
		// by mutating that one asset, and the segments then all render the same frames. Give each
		// job its own copy with the range already baked in instead.
		ULevelSequence* JobSequence = InSequence;
		if (InTemplate.bUniqueSequencePerJob && InSequence != nullptr)
		{
			if (ULevelSequence* Segment = MakeSegmentSequence(InSequence, Entry.StartFrame, Entry.EndFrame, Entry.Label))
			{
				JobSequence = Segment;
			}
			else
			{
				UE_LOG(LogMRQAutoSegment, Warning,
					TEXT("Could not write a segment sequence for '%s'; this job falls back to the shared sequence."),
					*Entry.Label);
			}
		}

		if (JobSequence != nullptr)
		{
			Job->Sequence = FSoftObjectPath(JobSequence);
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

		// The engine stores the range as a half open [Start, End) interval and feeds these
		// straight into TRange::SetPlaybackRange, so the end frame has to be exclusive.
		// Passing the inclusive end here loses the last frame of every segment.
		Basic->bOverride_CustomStartFrame = true;
		Basic->CustomStartFrame = Entry.StartFrame;
		Basic->bOverride_CustomEndFrame = true;
		Basic->CustomEndFrame = Entry.EndFrame + 1;

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

		// Temporal samples only. They live on the graph's sampling method node on the Globals
		// branch, so nothing about the renderer has to be forced - unlike the spatial sample
		// count, which is a deferred-renderer property.
		Basic->bOverride_TemporalSampleCount = true;
		Basic->TemporalSampleCount = FMath::Max(InTemplate.TemporalSampleCount, 1);

		UE_LOG(LogMRQAutoSegment, Log, TEXT("Created job '%s'  frames=%d..%d  file=%s  sequence=%s"),
			*Job->JobName, Entry.StartFrame, Entry.EndFrame, *Basic->FileNameFormat,
			*GetNameSafe(JobSequence));

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

FString FMRQAutoSegmentCore::GetSegmentSequenceFolder()
{
	return FString(MRQ_AUTOSEGMENT_SEGMENT_ROOT);
}

int32 FMRQAutoSegmentCore::DeleteGeneratedSequences()
{
	const FString Root = GetSegmentSequenceFolder();
	const FString Folder = FPackageName::LongPackageNameToFilename(Root, TEXT(""));

	if (!IFileManager::Get().DirectoryExists(*Folder))
	{
		return 0;
	}

	// The asset registry does not reliably see packages written earlier in the same session, so
	// the files on disk are the source of truth here.
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Folder, TEXT("*.uasset"), /*Files=*/true, /*Directories=*/false);

	TArray<UObject*> ToDelete;
	for (const FString& File : Files)
	{
		const FString PackageName = FPackageName::FilenameToLongPackageName(File);
		UPackage* Package = LoadPackage(nullptr, *PackageName, LOAD_None);
		if (Package == nullptr)
		{
			continue;
		}

		if (UObject* Asset = FindObject<UObject>(Package, *FPackageName::GetShortName(PackageName)))
		{
			ToDelete.Add(Asset);
		}
	}

	int32 Deleted = 0;
	if (ToDelete.Num() > 0)
	{
		Deleted = ObjectTools::ForceDeleteObjects(ToDelete, /*bShowConfirmation=*/false);
	}

	// Sweep up anything the object deletion left behind, then the folder itself.
	for (const FString& File : Files)
	{
		if (IFileManager::Get().FileExists(*File))
		{
			IFileManager::Get().Delete(*File, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
			IFileManager::Get().Delete(*(FPaths::ChangeExtension(File, TEXT("uexp"))), false, true, true);
		}
	}
	IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists=*/false, /*Tree=*/true);

	UE_LOG(LogMRQAutoSegment, Display, TEXT("Deleted %d per-segment sequence(s) from %s"), Deleted, *Root);
	return Deleted;
}
