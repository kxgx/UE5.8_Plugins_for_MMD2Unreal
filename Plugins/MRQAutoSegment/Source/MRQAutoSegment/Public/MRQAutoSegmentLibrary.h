// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MRQAutoSegmentTypes.h"
#include "MRQAutoSegmentLibrary.generated.h"

class ULevelSequence;
class UMovieGraphFileOutputNode;
class UMoviePipelineQueue;

/**
 * Scriptable entry points for the segmentation planner.
 *
 * Everything the panel does goes through here, so the same work can be driven from Blueprint,
 * from Python, or from the console without the UI being involved.
 */
UCLASS()
class MRQAUTOSEGMENT_API UMRQAutoSegmentLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Reads free system RAM and free video memory right now. */
	UFUNCTION(BlueprintCallable, Category = "MRQ Auto Segment")
	static FMRQHardwareBudget ProbeHardware();

	/** Computes the segment list from an explicit hardware snapshot. */
	UFUNCTION(BlueprintCallable, Category = "MRQ Auto Segment")
	static FMRQSegmentPlan BuildPlan(const FMRQSegmentRequest& Request, const FMRQHardwareBudget& Budget);

	/** Probes the hardware and plans in one call - the usual entry point. */
	UFUNCTION(BlueprintCallable, Category = "MRQ Auto Segment")
	static FMRQSegmentPlan PlanFromHardware(const FMRQSegmentRequest& Request);

	/**
	 * Best-effort bytes-per-pixel for an output node class, with a human readable reason.
	 * See FMRQAutoSegmentCore::GetBytesPerPixelForOutputType for why this is a guess.
	 */
	UFUNCTION(BlueprintCallable, Category = "MRQ Auto Segment")
	static int32 GetBytesPerPixelForOutputType(TSubclassOf<UMovieGraphFileOutputNode> OutputType, FString& OutReason);

	/**
	 * Writes one Movie Render Queue job per segment.
	 *
	 * The range label is appended to FileNameFormat, so a FileNameFormat of "{sequence_name}"
	 * produces files named like "<sequence>_0000-1023" and "<sequence>_1024-2047".
	 *
	 * @param TemporalSampleCount  Temporal sub-samples written onto every job. It lands on the
	 *                             graph's sampling method node, so it applies to any renderer.
	 * @return Number of jobs created.
	 */
	UFUNCTION(BlueprintCallable, Category = "MRQ Auto Segment")
	static int32 GenerateJobs(
		UMoviePipelineQueue* Queue,
		ULevelSequence* Sequence,
		const FString& MapPath,
		const FMRQSegmentPlan& Plan,
		const FString& OutputDirectory,
		const FString& FileNameFormat,
		FIntPoint Resolution,
		int32 TemporalSampleCount = 8);

	/** Removes jobs whose name starts with JobNamePrefix. @return Number removed. */
	UFUNCTION(BlueprintCallable, Category = "MRQ Auto Segment")
	static int32 DeleteGeneratedJobs(UMoviePipelineQueue* Queue, const FString& JobNamePrefix);

	/**
	 * Diagnostic. Rebuilds the graph the engine would build for each generated job and logs the
	 * frame range that graph carries. @return Number of jobs inspected.
	 */
	UFUNCTION(BlueprintCallable, Category = "MRQ Auto Segment")
	static int32 DumpGeneratedGraphs(UMoviePipelineQueue* Queue, const FString& JobNamePrefix);

	/** Deletes the per-segment sequence copies GenerateJobs wrote. @return Number deleted. */
	UFUNCTION(BlueprintCallable, Category = "MRQ Auto Segment")
	static int32 DeleteGeneratedSequences();

	/** Content folder holding the per-segment sequence copies. */
	UFUNCTION(BlueprintPure, Category = "MRQ Auto Segment")
	static FString GetSegmentSequenceFolder();

	/** The Movie Render Queue the editor is currently showing. */
	UFUNCTION(BlueprintCallable, Category = "MRQ Auto Segment")
	static UMoviePipelineQueue* GetEditorQueue();

	/** "12.34 GiB" style formatting. */
	UFUNCTION(BlueprintPure, Category = "MRQ Auto Segment")
	static FString FormatBytes(int64 Bytes);

	/** Default job name prefix used by the panel, so generated jobs can be found again. */
	UFUNCTION(BlueprintPure, Category = "MRQ Auto Segment")
	static FString GetDefaultJobNamePrefix();
};
