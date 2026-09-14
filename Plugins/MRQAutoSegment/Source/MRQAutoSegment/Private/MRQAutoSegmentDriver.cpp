// Copyright Epic Games, Inc. All Rights Reserved.

#include "MRQAutoSegmentDriver.h"

#include "MRQAutoSegmentCore.h"

#include "Editor.h"
#include "HAL/FileManager.h"
#include "LevelSequence.h"
#include "Misc/Paths.h"
#include "MoviePipelineBasicConfig.h"
#include "MoviePipelineExecutor.h"
#include "MoviePipelinePIEExecutor.h"
#include "MoviePipelineQueue.h"
#include "MoviePipelineQueueSubsystem.h"
#include "Graph/MovieGraphNamedResolution.h"
#include "Graph/Nodes/MovieGraphFileOutputNode.h"
#include "Graph/Nodes/MovieGraphVideoOutputNode.h"

DEFINE_LOG_CATEGORY_STATIC(LogMRQAutoSegmentDriver, Log, All);

namespace
{
	/** "ffmpeg failed (code 1)" style prefixing for delegate messages. */
	const TCHAR* kAlreadyRunning = TEXT("上一轮渲染还没结束。");

	/**
	 * Turns a directory that may still contain MRQ's own tokens into a real path.
	 *
	 * The job gets the token form, because MRQ resolves those itself, but finding the rendered
	 * files and joining them happens on the game thread where we need the concrete path.
	 */
	FString ResolveOutputDirectory(const FString& InDirectory)
	{
		if (InDirectory.IsEmpty())
		{
			return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MovieRenders"));
		}

		FString Directory = InDirectory;
		Directory.ReplaceInline(TEXT("{project_dir}"), *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		Directory.ReplaceInline(TEXT("{project_dir}/"), *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		Directory.RemoveFromEnd(TEXT("/"));

		return FPaths::ConvertRelativePathToFull(Directory);
	}
}

void UMRQSegmentRenderDriver::Report(const FString& InMessage)
{
	UE_LOG(LogMRQAutoSegmentDriver, Display, TEXT("%s"), *InMessage);
	OnStatus.ExecuteIfBound(InMessage);
}

void UMRQSegmentRenderDriver::Begin(ULevelSequence* InSequence, const FString& InMapPath, const FMRQSegmentPlan& InPlan,
	const FMRQJobTemplate& InTemplate, const FMRQSegmentRenderRequest& InRender)
{
	if (bRunning)
	{
		Report(kAlreadyRunning);
		return;
	}

	Segments = InPlan;
	Template = InTemplate;
	Render = InRender;
	MapPath = InMapPath;
	Sequence = InSequence;

	RenderedFiles.Reset();
	CurrentIndex = 0;
	CompletedSegments = 0;
	bCancelRequested = false;
	bHadError = false;
	LastError.Reset();

	if (!Segments.bValid || Segments.Segments.Num() == 0)
	{
		OnCompleted.ExecuteIfBound(false, TEXT("没有可渲染的分段。"));
		return;
	}

	if (Sequence == nullptr)
	{
		OnCompleted.ExecuteIfBound(false, TEXT("没有可渲染的序列。"));
		return;
	}

	UMoviePipelineQueueSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
	if (Subsystem == nullptr)
	{
		OnCompleted.ExecuteIfBound(false, TEXT("拿不到 Movie Render Queue 子系统。"));
		return;
	}

	bRunning = true;
	StartSegment(0);
}

void UMRQSegmentRenderDriver::StartSegment(int32 InIndex)
{
	if (bCancelRequested || !Segments.Segments.IsValidIndex(InIndex))
	{
		Finish();
		return;
	}

	CurrentIndex = InIndex;
	const FMRQSegmentPlanEntry& Entry = Segments.Segments[InIndex];

	Report(FString::Printf(TEXT("[%d/%d] 正在渲染 %s …"), InIndex + 1, Segments.Segments.Num(), *Entry.Label));

	// A fresh transient queue per segment. The user's own queue in the MRQ window is never
	// touched, and a run with exactly one job has no second range to collide with.
	UMoviePipelineQueue* Queue = NewObject<UMoviePipelineQueue>(GetTransientPackage());
	ActiveQueue = Queue;

	UMoviePipelineExecutorJob* Job = Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
	if (Job == nullptr)
	{
		bHadError = true;
		LastError = TEXT("无法创建 MRQ 任务。");
		Finish();
		return;
	}

	Job->JobName = FString::Printf(TEXT("%s %s"), *Template.JobNamePrefix, *Entry.Label);
	Job->Comment = FString::Printf(TEXT("Frames %d-%d (%d frames)"), Entry.StartFrame, Entry.EndFrame, Entry.FrameCount);
	Job->SetConsumed(false);
	Job->SetIsEnabled(true);

	// A copy of the sequence whose own playback range is already this segment. Belt and braces:
	// even if the range override below were ignored, the sequence alone is enough.
	ULevelSequence* JobSequence = FMRQAutoSegmentCore::MakeSegmentSequence(Sequence, Entry.StartFrame, Entry.EndFrame, Entry.Label);
	Job->Sequence = FSoftObjectPath(JobSequence != nullptr ? JobSequence : Sequence.Get());

	if (!MapPath.IsEmpty())
	{
		Job->Map = FSoftObjectPath(MapPath);
	}

	UMoviePipelineBasicConfig* Basic = Job->SetupBasicConfiguration();
	if (Basic == nullptr)
	{
		bHadError = true;
		LastError = TEXT("任务没有 Basic 配置。");
		Finish();
		return;
	}

	// One folder per segment, so a partial run stays readable and the join order is unambiguous.
	const FString SegmentFolder = FMRQAutoSegmentCore::GetSegmentOutputFolder(Template.OutputDirectory, Entry.Label);
	Basic->bOverride_OutputDirectory = true;
	Basic->OutputDirectory.Path = SegmentFolder;

	Basic->bOverride_FileNameFormat = true;
	Basic->FileNameFormat = FMRQAutoSegmentCore::AppendRangeToPattern(Template.FileNameFormat, Entry.Label);

	// The engine stores the range half open, so the inclusive last frame becomes End + 1.
	Basic->bOverride_CustomStartFrame = true;
	Basic->CustomStartFrame = Entry.StartFrame;
	Basic->bOverride_CustomEndFrame = true;
	Basic->CustomEndFrame = Entry.EndFrame + 1;

	if (Template.Resolution.X > 0 && Template.Resolution.Y > 0)
	{
		Basic->bOverride_OutputResolution = true;
		Basic->OutputResolution = FMovieGraphNamedResolution(
			FMovieGraphNamedResolution::CustomEntryName, Template.Resolution, FString());
	}

	if (Template.OutputTypes.Num() > 0)
	{
		Basic->bOverride_EnabledOutputTypes = true;
		Basic->EnabledOutputTypes.Reset();
		for (UClass* OutputType : Template.OutputTypes)
		{
			if (OutputType != nullptr && OutputType->IsChildOf(UMovieGraphFileOutputNode::StaticClass()))
			{
				Basic->EnabledOutputTypes.Add(TSoftClassPtr<UMovieGraphFileOutputNode>(OutputType));
			}
		}
	}

	Basic->bOverride_TemporalSampleCount = true;
	Basic->TemporalSampleCount = FMath::Max(Template.TemporalSampleCount, 1);

	UMoviePipelineQueueSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
	if (Subsystem == nullptr)
	{
		bHadError = true;
		LastError = TEXT("拿不到 Movie Render Queue 子系统。");
		Finish();
		return;
	}

	UMoviePipelineExecutorBase* Executor = Subsystem->RenderQueueInstanceWithExecutor(Queue, UMoviePipelinePIEExecutor::StaticClass());
	if (Executor == nullptr)
	{
		bHadError = true;
		LastError = TEXT("启动渲染执行器失败。");
		Finish();
		return;
	}

	ActiveExecutor = Executor;
	Executor->OnExecutorFinished().AddUObject(this, &UMRQSegmentRenderDriver::HandleExecutorFinished);
	Executor->OnExecutorErrored().AddUObject(this, &UMRQSegmentRenderDriver::HandleExecutorErrored);
}

void UMRQSegmentRenderDriver::HandleExecutorErrored(UMoviePipelineExecutorBase* InExecutor, UMoviePipeline* InPipelineWithError,
	bool bIsFatal, FText InErrorText)
{
	if (bIsFatal)
	{
		bHadError = true;
		LastError = InErrorText.ToString();
	}
}

void UMRQSegmentRenderDriver::HandleExecutorFinished(UMoviePipelineExecutorBase* InExecutor, bool bSuccess)
{
	if (!bRunning || !Segments.Segments.IsValidIndex(CurrentIndex))
	{
		return;
	}

	const FMRQSegmentPlanEntry& Entry = Segments.Segments[CurrentIndex];

	if (bSuccess)
	{
		const FString Folder = FMRQAutoSegmentCore::GetSegmentOutputFolder(
			ResolveOutputDirectory(Template.OutputDirectory), Entry.Label);

		// Match on the range label: it is unique per segment and survives token resolution,
		// unlike the raw pattern which still contains things like {sequence_name}. The extension
		// is left open - the segment has its own folder, so whatever single file is in there is
		// the result, and an image sequence (many files) correctly fails this test.
		FString File;
		if (FMRQAutoSegmentCore::FindSegmentOutputFile(Folder, Entry.Label, FString(), File))
		{
			RenderedFiles.Add(File);
			++CompletedSegments;
		}
		else
		{
			bHadError = true;
			LastError = FString::Printf(
				TEXT("%s 段渲染结束但没在 %s 里找到唯一的输出文件（图像序列无法合并）。"), *Entry.Label, *Folder);
		}
	}
	else
	{
		bHadError = true;
		LastError = FString::Printf(TEXT("%s 段渲染失败。"), *Entry.Label);
	}

	ActiveExecutor = nullptr;
	ActiveQueue = nullptr;

	StartSegment(CurrentIndex + 1);
}

void UMRQSegmentRenderDriver::Finish()
{
	bRunning = false;

	if (bCancelRequested)
	{
		OnCompleted.ExecuteIfBound(false, FString::Printf(
			TEXT("已取消，完成 %d/%d 段。"), CompletedSegments, Segments.Segments.Num()));
		return;
	}

	if (bHadError)
	{
		OnCompleted.ExecuteIfBound(false, LastError.IsEmpty() ? TEXT("渲染失败。") : LastError);
		return;
	}

	if (RenderedFiles.Num() != Segments.Segments.Num())
	{
		OnCompleted.ExecuteIfBound(false, FString::Printf(
			TEXT("只拿到 %d/%d 段输出。"), RenderedFiles.Num(), Segments.Segments.Num()));
		return;
	}

	if (!Render.bMergeAfterRender)
	{
		OnCompleted.ExecuteIfBound(true, FString::Printf(
			TEXT("%d 段全部渲染完成（按设置未合并）。"), RenderedFiles.Num()));
		return;
	}

	// Whatever the output node produced is what gets joined, so take the extension from the
	// file that actually landed on disk rather than guessing it per node type.
	const FString Extension = FPaths::GetExtension(RenderedFiles[0]);
	const FString Root = ResolveOutputDirectory(Template.OutputDirectory);

	FString MergedName = Render.MergedFileName;
	if (MergedName.IsEmpty())
	{
		MergedName = FString::Printf(TEXT("%s_full"), Sequence != nullptr ? *Sequence->GetName() : TEXT("merged"));
	}
	const FString OutputFile = FPaths::Combine(Root, FString::Printf(TEXT("%s.%s"), *MergedName, *Extension));

	FString Reason;
	const FString Ffmpeg = FMRQAutoSegmentCore::ResolveFfmpegPath(Render.FfmpegPath, Reason);
	Report(FString::Printf(TEXT("正在合并 %d 段 → %s（ffmpeg %s）"), RenderedFiles.Num(), *OutputFile, *Reason));

	FString Error;
	if (!FMRQAutoSegmentCore::MergeVideos(Ffmpeg, RenderedFiles, OutputFile, Error))
	{
		OnCompleted.ExecuteIfBound(false, FString::Printf(TEXT("合并失败：%s"), *Error));
		return;
	}

	if (Render.bDeleteSegmentsAfterMerge)
	{
		for (const FString& File : RenderedFiles)
		{
			IFileManager::Get().DeleteDirectory(*FPaths::GetPath(File), /*RequireExists=*/false, /*Tree=*/true);
		}
	}

	OnCompleted.ExecuteIfBound(true, FString::Printf(TEXT("完成：%d 段已合并为 %s"), RenderedFiles.Num(), *OutputFile));
}
