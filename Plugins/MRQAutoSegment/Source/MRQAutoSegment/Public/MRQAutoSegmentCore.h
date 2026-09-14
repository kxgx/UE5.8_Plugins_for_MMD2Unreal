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

	/**
	 * Temporal sub-samples per output frame, written onto every generated job.
	 *
	 * This lands on the graph's sampling method node, which sits on the Globals branch, so it
	 * applies whatever renderer the graph uses - unlike the spatial sample count, which is a
	 * deferred-renderer property.
	 */
	int32 TemporalSampleCount = 8;

	/** Digits used to zero pad the frame numbers inside the range label. */
	int32 PadDigits = 4;

	/**
	 * Give every job its own copy of the sequence, with the segment's playback range baked into
	 * that copy, instead of pointing every job at the one shared sequence.
	 *
	 * MRQ applies a job's custom playback range by mutating the job's sequence in place. When
	 * every job points at the same sequence asset those mutations overlap and the queue renders
	 * the same frames for every segment - the file names differ but the contents do not. Baking
	 * the range into a per-segment copy removes the overlap entirely: even if the engine falls
	 * back to "use the sequence's own range", each job's sequence already holds its own range.
	 */
	bool bUniqueSequencePerJob = true;
};

/** Content folder the per-segment sequence copies are written into. */
#define MRQ_AUTOSEGMENT_SEGMENT_ROOT TEXT("/Game/MRQAutoSegment/Segments")

/**
 * Hardware probing, segment planning and queue generation.
 *
 * Kept free of UI so the whole thing is drivable from a console command, a Blueprint
 * or a Python script as well as from the panel.
 */
class FMRQAutoSegmentCore
{
public:
	/** Reads free system RAM and free video memory. */
	static FMRQHardwareBudget ProbeHardware();

	/** Turns a request plus a hardware snapshot into a concrete list of segments. */
	static FMRQSegmentPlan BuildPlan(const FMRQSegmentRequest& InRequest, const FMRQHardwareBudget& InBudget);

	/**
	 * Best-effort number of bytes a single output pixel occupies for the given output node class.
	 *
	 * The graph's output nodes do not expose their bit depth as a reflected property -
	 * UMoviePipelineVideoOutputBase takes it as a function argument, and the image sequence nodes
	 * keep EImageFormat as a plain C++ member - so known formats are matched by class name.
	 * OutReason describes how the number was arrived at and is shown in the panel, so a wrong
	 * guess is visible rather than silent.
	 *
	 * @return Bytes per pixel, or 4 when nothing is recognised.
	 */
	static int32 GetBytesPerPixelForOutputType(const UClass* InOutputType, FString& OutReason);

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

	/**
	 * Diagnostic. For every generated job, rebuilds the graph the engine builds at render time and
	 * reports the frame range it carries, so "the plugin wrote the wrong numbers" can be told
	 * apart from "the engine ignored the numbers".
	 *
	 * @return Number of jobs inspected.
	 */
	static int32 DumpGeneratedGraphs(UMoviePipelineQueue* InQueue, const FString& InJobNamePrefix);

	/** Deletes the per-segment sequence copies written by GenerateJobs. @return Number deleted. */
	static int32 DeleteGeneratedSequences();

	/** Content folder holding the per-segment sequence copies, for display. */
	static FString GetSegmentSequenceFolder();

	/** "12.3 GiB" style formatting. */
	static FString FormatBytes(int64 InBytes);
};
