// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MRQAutoSegmentCore.h"
#include "MRQAutoSegmentTypes.h"
#include "MRQAutoSegmentDriver.generated.h"

class ULevelSequence;
class UMoviePipeline;
class UMoviePipelineExecutorBase;
class UMoviePipelineQueue;

/**
 * Renders a segmentation one segment at a time, each as its own Movie Render Queue run, then
 * joins the results with ffmpeg.
 *
 * Why not one queue with N jobs: MRQ applies a job's custom playback range by mutating the
 * sequence that job points at. With several jobs in flight the ranges interfere and every
 * segment ends up rendering the same frames - the file names differ, the contents do not.
 * A run with exactly one job has nothing to interfere with, and each job additionally points at
 * its own copy of the sequence with the range already baked in.
 *
 * Each segment writes into its own folder, "<OutputDirectory>/<label>/", so a partial run can be
 * resumed and the join step has an unambiguous order to read them back in.
 *
 * The driver renders into transient queues through
 * UMoviePipelineQueueSubsystem::RenderQueueInstanceWithExecutor(), so the user's own queue is
 * left untouched.
 */
UCLASS()
class UMRQSegmentRenderDriver : public UObject
{
	GENERATED_BODY()

public:
	/** Status line for the panel. */
	DECLARE_DELEGATE_OneParam(FOnStatus, const FString&);

	/** Fired once at the very end. Message describes the outcome. */
	DECLARE_DELEGATE_TwoParams(FOnCompleted, bool /*bSuccess*/, const FString& /*Message*/);

	FOnStatus OnStatus;
	FOnCompleted OnCompleted;

	/**
	 * Starts the run. Safe to call only while no other run is active.
	 *
	 * @param InPlan      Segments to render, in order.
	 * @param InTemplate  Output settings; OutputDirectory is the root the segment folders go under.
	 * @param InRender    Merge settings.
	 */
	void Begin(ULevelSequence* InSequence, const FString& InMapPath, const FMRQSegmentPlan& InPlan,
		const FMRQJobTemplate& InTemplate, const FMRQSegmentRenderRequest& InRender);

	/** Asks the run to stop after the segment currently rendering. */
	void RequestCancel() { bCancelRequested = true; }

	bool IsRunning() const { return bRunning; }

	/** Progress, for the panel. */
	int32 GetCompletedSegments() const { return CompletedSegments; }
	int32 GetTotalSegments() const { return Segments.Segments.Num(); }

private:
	/** One MRQ render. */
	void StartSegment(int32 InIndex);

	/** Bound to the executor's native delegates; its dynamic ones are private to it. */
	void HandleExecutorFinished(UMoviePipelineExecutorBase* InExecutor, bool bSuccess);
	void HandleExecutorErrored(UMoviePipelineExecutorBase* InExecutor, UMoviePipeline* InPipelineWithError, bool bIsFatal, FText InErrorText);

	/** Joins everything that rendered, then reports. */
	void Finish();

	void Report(const FString& InMessage);

	/** The plan being rendered. */
	FMRQSegmentPlan Segments;

	FMRQJobTemplate Template;
	FMRQSegmentRenderRequest Render;
	FString MapPath;

	/** Kept alive for the duration of the run. */
	UPROPERTY()
	TObjectPtr<ULevelSequence> Sequence;

	/** Kept alive until the executor reports back, otherwise the delegate unbinds mid-render. */
	UPROPERTY()
	TObjectPtr<UMoviePipelineQueue> ActiveQueue;

	UPROPERTY()
	TObjectPtr<UMoviePipelineExecutorBase> ActiveExecutor;

	/** Files that rendered, in segment order, ready for the join. */
	TArray<FString> RenderedFiles;

	int32 CurrentIndex = 0;
	int32 CompletedSegments = 0;
	bool bRunning = false;
	bool bCancelRequested = false;
	bool bHadError = false;
	FString LastError;
};
