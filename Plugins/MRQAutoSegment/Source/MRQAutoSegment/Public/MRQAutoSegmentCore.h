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

	/**
	 * Name of the project's named resolution to render at, i.e. one of the entries in
	 * UMovieGraphProjectSettings::DefaultNamedResolutions - the same list the Movie Graph output
	 * node offers.
	 *
	 * The entry is copied onto the job verbatim. The plugin deliberately does not write a hand
	 * built "Custom" entry: a job whose Basic config carries one is not what the render queue
	 * expects, and the queue then re-renders the first job over and over. To render at a size
	 * that is not in the list yet, add it under
	 * Project Settings -> Movie Render Pipeline -> Named Resolutions; it will show up here.
	 *
	 * Empty means "leave the resolution to the generated graph".
	 */
	FName ResolutionProfile;

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
	 * Write a level sequence per segment, carrying that segment's frame range, and point the job
	 * at it.
	 *
	 * This is the "generate a sequence for the specified frame range" mode. The copies are real
	 * assets under MRQ_AUTOSEGMENT_SEGMENT_ROOT; each one gets both its playback range and its
	 * working range set, and is saved to disk, so the range is part of the asset rather than
	 * something a later step has to apply.
	 *
	 * Off means every job points at the sequence you picked and the range only lives on the job.
	 */
	bool bSequencePerSegment = true;
};

/** Content folder the per-segment sequences are written into. */
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
	 * Every job carries its own frame range, file name and output folder on its Basic config.
	 * With bSequencePerSegment set, each job also points at its own level sequence whose range is
	 * already baked in.
	 *
	 * @return Number of jobs actually created.
	 */
	static int32 GenerateJobs(UMoviePipelineQueue* InQueue, ULevelSequence* InSequence, const FString& InMapPath, const FMRQSegmentPlan& InPlan, const FMRQJobTemplate& InTemplate);

	/** Removes jobs this plugin previously generated, identified by their JobName prefix. */
	static int32 DeleteGeneratedJobs(UMoviePipelineQueue* InQueue, const FString& InJobNamePrefix);

	/**
	 * Writes, or refreshes, the level sequence for one segment and returns it.
	 *
	 * Sets the playback range and the working range from InStartFrame / InEndFrameInclusive and
	 * saves the asset, so the range survives a reload and a render in a separate process.
	 *
	 * @param InEndFrameInclusive  Last frame of the segment, inclusive. Stored half open.
	 * @return The new sequence, or nullptr if the asset could not be written.
	 */
	static ULevelSequence* MakeSegmentSequence(ULevelSequence* InSource, int32 InStartFrame, int32 InEndFrameInclusive, const FString& InLabel);

	/** Deletes the per-segment sequences written by GenerateJobs. @return Number deleted. */
	static int32 DeleteGeneratedSequences();

	/** Content folder holding the per-segment sequences. */
	static FString GetSegmentSequenceFolder();

	/**
	 * Diagnostic. For every generated job, rebuilds the graph the engine builds at render time and
	 * reports the frame range it carries, so "the plugin wrote the wrong numbers" can be told
	 * apart from "the engine ignored the numbers".
	 *
	 * @return Number of jobs inspected.
	 */
	static int32 DumpGeneratedGraphs(UMoviePipelineQueue* InQueue, const FString& InJobNamePrefix);

	/**
	 * Per-segment output folder: "<OutputDirectory>/<label>".
	 *
	 * Each segment writing into its own folder keeps the pieces from overwriting or mixing, and
	 * makes it obvious which file belongs to which range.
	 */
	static FString GetSegmentOutputFolder(const FString& InOutputDirectory, const FString& InLabel);

	/** "12.3 GiB" style formatting. */
	static FString FormatBytes(int64 InBytes);
};
