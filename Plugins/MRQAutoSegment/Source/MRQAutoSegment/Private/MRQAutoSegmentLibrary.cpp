// Copyright Epic Games, Inc. All Rights Reserved.

#include "MRQAutoSegmentLibrary.h"

#include "MRQAutoSegmentCore.h"

#include "Editor.h"
#include "LevelSequence.h"
#include "MoviePipelineQueue.h"
#include "MoviePipelineQueueSubsystem.h"

FMRQHardwareBudget UMRQAutoSegmentLibrary::ProbeHardware()
{
	return FMRQAutoSegmentCore::ProbeHardware();
}

FMRQSegmentPlan UMRQAutoSegmentLibrary::BuildPlan(const FMRQSegmentRequest& Request, const FMRQHardwareBudget& Budget)
{
	return FMRQAutoSegmentCore::BuildPlan(Request, Budget);
}

FMRQSegmentPlan UMRQAutoSegmentLibrary::PlanFromHardware(const FMRQSegmentRequest& Request)
{
	const FMRQHardwareBudget Budget = FMRQAutoSegmentCore::ProbeHardware();
	return FMRQAutoSegmentCore::BuildPlan(Request, Budget);
}

int32 UMRQAutoSegmentLibrary::GetBytesPerPixelForOutputType(TSubclassOf<UMovieGraphFileOutputNode> OutputType, FString& OutReason)
{
	return FMRQAutoSegmentCore::GetBytesPerPixelForOutputType(OutputType.Get(), OutReason);
}

int32 UMRQAutoSegmentLibrary::GenerateJobs(
	UMoviePipelineQueue* Queue,
	ULevelSequence* Sequence,
	const FString& MapPath,
	const FMRQSegmentPlan& Plan,
	const FString& OutputDirectory,
	const FString& FileNameFormat,
	const FString& ResolutionProfile,
	int32 TemporalSampleCount)
{
	FMRQJobTemplate Template;
	Template.OutputDirectory = OutputDirectory;
	Template.FileNameFormat = FileNameFormat;
	Template.ResolutionProfile = ResolutionProfile.IsEmpty() ? NAME_None : FName(*ResolutionProfile);
	Template.JobNamePrefix = GetDefaultJobNamePrefix();
	Template.TemporalSampleCount = FMath::Max(TemporalSampleCount, 1);

	return FMRQAutoSegmentCore::GenerateJobs(Queue, Sequence, MapPath, Plan, Template);
}

int32 UMRQAutoSegmentLibrary::DeleteGeneratedJobs(UMoviePipelineQueue* Queue, const FString& JobNamePrefix)
{
	return FMRQAutoSegmentCore::DeleteGeneratedJobs(Queue, JobNamePrefix);
}

int32 UMRQAutoSegmentLibrary::DumpGeneratedGraphs(UMoviePipelineQueue* Queue, const FString& JobNamePrefix)
{
	return FMRQAutoSegmentCore::DumpGeneratedGraphs(Queue, JobNamePrefix);
}

UMoviePipelineQueue* UMRQAutoSegmentLibrary::GetEditorQueue()
{
	if (GEditor == nullptr)
	{
		return nullptr;
	}

	if (UMoviePipelineQueueSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>())
	{
		return Subsystem->GetQueue();
	}
	return nullptr;
}

FString UMRQAutoSegmentLibrary::FormatBytes(int64 Bytes)
{
	return FMRQAutoSegmentCore::FormatBytes(Bytes);
}

FString UMRQAutoSegmentLibrary::GetDefaultJobNamePrefix()
{
	return TEXT("Segment");
}
