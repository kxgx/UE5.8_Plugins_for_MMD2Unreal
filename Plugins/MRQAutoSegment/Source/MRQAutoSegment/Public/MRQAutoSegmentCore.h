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
	 * Every job points at InSequence itself and carries its own frame range, file name and output
	 * folder on its Basic config. The plugin writes no assets of its own: a per-segment sequence
	 * looked like a fix for ranges that appeared to be ignored, but the ranges were fine all
	 * along, so the copies only added a content folder nobody needs.
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

	/** "12.3 GiB" style formatting. */
	static FString FormatBytes(int64 InBytes);
};
