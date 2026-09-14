// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MRQAutoSegmentTypes.generated.h"

/** How the segment length is decided. */
UENUM(BlueprintType)
enum class EMRQSegmentMode : uint8
{
	/** Derive the segment length from the machine's free RAM / VRAM / disk. */
	Auto,
	/** Split the range into a fixed number of equal segments. */
	FixedCount,
	/** Use a fixed number of frames per segment. */
	FixedLength
};

/** Which limit ended up deciding the segment length. */
UENUM(BlueprintType)
enum class EMRQSegmentBound : uint8
{
	None,
	SystemRAM,
	VideoRAM,
	DiskSpace,
	UserCap
};

/**
 * A snapshot of the machine's headroom, taken at the moment the plan is built.
 * All byte counts.
 */
USTRUCT(BlueprintType)
struct FMRQHardwareBudget
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Hardware")
	int64 TotalPhysicalRAM = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Hardware")
	int64 AvailablePhysicalRAM = 0;

	/** Total device video memory reported by the RHI. 0 when the RHI could not tell us. */
	UPROPERTY(BlueprintReadOnly, Category = "Hardware")
	int64 TotalVRAM = 0;

	/** VRAM this process has already allocated (streaming + non-streaming). */
	UPROPERTY(BlueprintReadOnly, Category = "Hardware")
	int64 EngineUsedVRAM = 0;

	/** TotalVRAM - EngineUsedVRAM, clamped at 0. Only counts *this* process's allocations. */
	UPROPERTY(BlueprintReadOnly, Category = "Hardware")
	int64 AvailableVRAM = 0;

	/** Free space on the volume that holds the output directory. */
	UPROPERTY(BlueprintReadOnly, Category = "Hardware")
	int64 FreeDiskBytes = 0;

	/** Adapter name, for display only. */
	UPROPERTY(BlueprintReadOnly, Category = "Hardware")
	FString AdapterName;

	/** False when the RHI returned no usable video memory figures. */
	UPROPERTY(BlueprintReadOnly, Category = "Hardware")
	bool bVRAMKnown = false;
};

/** One limit that was evaluated while planning, kept so the UI can explain the result. */
USTRUCT(BlueprintType)
struct FMRQSegmentConstraint
{
	GENERATED_BODY()

	/** Human readable name of the limit, e.g. "显存". */
	UPROPERTY(BlueprintReadOnly, Category = "Segment")
	FString Name;

	/** Frames per segment this limit allows. MAX_int32 means "no limit from here". */
	UPROPERTY(BlueprintReadOnly, Category = "Segment")
	int32 Frames = 0;

	/** How the number was arrived at, e.g. "空闲 8.2 GiB / 每帧 3.4 MiB". */
	UPROPERTY(BlueprintReadOnly, Category = "Segment")
	FString Detail;

	/** True for the limit that actually decided FramesPerSegment. */
	UPROPERTY(BlueprintReadOnly, Category = "Segment")
	bool bBinding = false;
};

/** One segment of the range. Frames are inclusive, in the sequence's display rate. */
USTRUCT(BlueprintType)
struct FMRQSegmentPlanEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Segment")
	int32 Index = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Segment")
	int32 StartFrame = 0;

	/** Inclusive. */
	UPROPERTY(BlueprintReadOnly, Category = "Segment")
	int32 EndFrame = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Segment")
	int32 FrameCount = 0;

	/** Zero padded "start-end", used as the file name suffix. */
	UPROPERTY(BlueprintReadOnly, Category = "Segment")
	FString Label;

	/** What the output file will be called, for the panel's preview list. */
	UPROPERTY(BlueprintReadOnly, Category = "Segment")
	FString OutputName;
};

/** The inputs the plan is computed from. Everything here is user-editable. */
USTRUCT(BlueprintType)
struct FMRQSegmentRequest
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Range")
	EMRQSegmentMode Mode = EMRQSegmentMode::Auto;

	/** First frame of the whole render, inclusive. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Range")
	int32 RangeStart = 0;

	/** Last frame of the whole render, inclusive. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Range")
	int32 RangeEnd = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Range", meta = (EditCondition = "Mode == EMRQSegmentMode::FixedCount", EditConditionHides, ClampMin = 1))
	int32 FixedSegmentCount = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Range", meta = (EditCondition = "Mode == EMRQSegmentMode::FixedLength", EditConditionHides, ClampMin = 1))
	int32 FixedSegmentLength = 500;

	/** Hard ceiling on frames per segment, whatever the hardware says. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Range", meta = (ClampMin = 1))
	int32 MaxSegmentLength = 1000;

	/** Digits the frame numbers in the range label are zero padded to. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Range", meta = (ClampMin = 1, ClampMax = 10))
	int32 LabelPadDigits = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	FIntPoint Resolution = FIntPoint(1920, 1080);

	/** Bytes per output pixel, including all channels. 4 = 8-bit RGBA, 8 = half-float RGBA. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (ClampMin = 1))
	int32 BytesPerPixel = 4;

	/**
	 * Estimated per-frame growth in RAM, as a fraction of one frame buffer.
	 *
	 * The mechanism this models: rendered frames queue up in the image-write / encoder pipeline
	 * whenever the renderer outruns the write path, and that backlog lives in RAM. A fraction of
	 * one frame buffer per frame is a deliberately cautious stand-in for "how far behind can the
	 * write path fall". Raise it to segment harder.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Estimates", meta = (ClampMin = 0.0))
	float RAMGrowthPerFrame = 0.25f;

	/** Estimated per-frame growth in VRAM, as a fraction of one frame buffer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Estimates", meta = (ClampMin = 0.0))
	float VRAMGrowthPerFrame = 0.5f;

	/** Fraction of free RAM held back for the OS, other apps and the editor itself. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Estimates", meta = (ClampMin = 0.0, ClampMax = 0.95))
	float RAMReserveFraction = 0.25f;

	/** Fraction of free VRAM held back. Covers allocations we cannot see from in-process. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Estimates", meta = (ClampMin = 0.0, ClampMax = 0.95))
	float VRAMReserveFraction = 0.20f;

	/** Whether free disk space should also cap the segment length. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Estimates")
	bool bLimitByDisk = true;

	/** Fraction of free disk space the whole render may consume. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Estimates", meta = (ClampMin = 0.0, ClampMax = 0.95))
	float DiskUseFraction = 0.5f;
};

/** A complete, ready-to-apply segmentation. */
USTRUCT(BlueprintType)
struct FMRQSegmentPlan
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Plan")
	bool bValid = false;

	/** Why the plan is invalid, or a short summary when it is valid. */
	UPROPERTY(BlueprintReadOnly, Category = "Plan")
	FString Message;

	UPROPERTY(BlueprintReadOnly, Category = "Plan")
	EMRQSegmentBound Binding = EMRQSegmentBound::None;

	UPROPERTY(BlueprintReadOnly, Category = "Plan")
	int32 TotalFrames = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Plan")
	int32 FramesPerSegment = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Plan")
	int32 SegmentCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Plan")
	TArray<FMRQSegmentPlanEntry> Segments;

	/** Every limit that was evaluated, with the binding one flagged. */
	UPROPERTY(BlueprintReadOnly, Category = "Plan")
	TArray<FMRQSegmentConstraint> Constraints;
};
