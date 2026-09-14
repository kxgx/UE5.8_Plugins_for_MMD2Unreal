// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MRQAutoSegmentTypes.h"

class ULevelSequence;
class UMoviePipelineQueue;

/** Everything the generated jobs inherit that is not per-segment. */
struct FMRQJobTemplate
{
	/** Directory every segment writes into. */
	FString OutputDirectory;

	/**
	 * File name pattern, using the engine's usual tokens ({sequence_name}, {date}, ...).
	 * The range label is appended to this, so "{sequence_name}" becomes
	 * "{sequence_name}_0000-1023" for the first segment.
	 */
	FString FileNameFormat = TEXT("{sequence_name}");

	/** Only used to make the panel's preview look like a real file name. */
	FString PreviewExtension = TEXT(".mp4");

	/** Prefix for the job's display name in the queue. */
	FString JobNamePrefix = TEXT("Segment");

	FIntPoint Resolution = FIntPoint(1920, 1080);

	/** Output node classes to enable. Empty leaves the generated graph at its default. */
	TArray<UClass*> OutputTypes;

	/** When true the generated jobs also pin the sample counts. */
	bool bSetSampleCounts = false;
	int32 SpatialSampleCount = 1;
	int32 TemporalSampleCount = 1;

	/** Digits used to zero pad the frame numbers inside the range label. */
	int32 PadDigits = 4;
};

/**
 * Hardware probing, segment planning and queue generation.
 *
 * Kept free of UI so the whole thing is drivable from a console command, a Blueprint
 * or a Python script as well as from the panel.
 */
class FMRQAutoSegmentCore
{
public:
	/**
	 * Reads free RAM, free video memory and free disk space.
	 * @param InOutputDirectory  Volume to measure for free space. Falls back to the project dir.
	 */
	static FMRQHardwareBudget ProbeHardware(const FString& InOutputDirectory);

	/** Turns a request plus a hardware snapshot into a concrete list of segments. */
	static FMRQSegmentPlan BuildPlan(const FMRQSegmentRequest& InRequest, const FMRQHardwareBudget& InBudget);

	/** "0000-1023", zero padded to PadDigits. */
	static FString MakeRangeLabel(int32 StartFrame, int32 EndFrame, int32 PadDigits);

	/** Appends the range label to a file name pattern, keeping the engine's tokens intact. */
	static FString AppendRangeToPattern(const FString& InPattern, const FString& InRangeLabel);

	/** Resolves just enough tokens to give the user a readable file name in the panel. */
	static FString PreviewFileName(const FString& InPattern, const FString& InRangeLabel, const FString& InSequenceName, const FString& InExtension);

	/**
	 * Writes one job per segment into InQueue.
	 *
	 * Each job is put into Basic configuration mode: that mode builds its UMovieGraphConfig
	 * just-in-time at render time, which is what lets us set the file name format and the
	 * playback range per job without touching - or duplicating - the user's own graph asset.
	 *
	 * @return Number of jobs actually created.
	 */
	static int32 GenerateJobs(UMoviePipelineQueue* InQueue, ULevelSequence* InSequence, const FString& InMapPath, const FMRQSegmentPlan& InPlan, const FMRQJobTemplate& InTemplate);

	/** Removes jobs this plugin previously generated, identified by their JobName prefix. */
	static int32 DeleteGeneratedJobs(UMoviePipelineQueue* InQueue, const FString& InJobNamePrefix);

	/** "12.3 GiB" style formatting. */
	static FString FormatBytes(int64 InBytes);
};
