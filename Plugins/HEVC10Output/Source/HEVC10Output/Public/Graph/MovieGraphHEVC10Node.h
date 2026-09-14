// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Graph/MovieGraphNode.h"
#include "Graph/Nodes/MovieGraphVideoOutputNode.h"
#include "HEVC10FFmpegPipe.h"
#include "UObject/SoftObjectPath.h"

#include "MovieGraphHEVC10Node.generated.h"

class FHEVC10FFmpegPipe;

/** Which encoder backend ffmpeg should use. */
UENUM()
enum class EHEVC10Encoder : uint8
{
	/** NVIDIA hardware HEVC (hevc_nvenc). Very fast; needs a GPU with HEVC Main10 encode. */
	NVENC,

	/** libx265 software encoding. Slower, but better quality per bitrate. */
	X265,
};

/**
 * Outputs a 10-bit H.265 (HEVC) MP4.
 *
 * Unlike UE's built-in MP4 output (H.264, 8-bit only), this node keeps the pipeline in
 * 16-bit half-float right up to the encoder. Frames are fed to an ffmpeg child process over
 * stdin as raw rgbaf16le, so nothing is written to disk between render and encode and the
 * encoding happens in parallel with rendering.
 */
UCLASS(MinimalAPI, BlueprintType)
class UMovieGraphHEVC10Node : public UMovieGraphVideoOutputNode
{
	GENERATED_BODY()

public:
	UMovieGraphHEVC10Node();

#if WITH_EDITOR
	virtual FText GetNodeTitle(const bool bGetDescriptive = false) const override;
	virtual FText GetMenuCategory() const override;
	virtual FText GetKeywords() const override;
	virtual FLinearColor GetNodeTitleColor() const override;
	virtual FSlateIcon GetIconAndTint(FLinearColor& OutColor) const override;
	virtual FText GetBasicConfigShortDisplayName() const override;
#endif

protected:
	// UMovieGraphVideoOutputNode Interface
	virtual TUniquePtr<MovieRenderGraph::IVideoCodecWriter> Initialize_GameThread(const FMovieGraphVideoNodeInitializationContext& InInitializationContext) override;
	virtual bool Initialize_EncodeThread(MovieRenderGraph::IVideoCodecWriter* InWriter) override;
	virtual void WriteFrame_EncodeThread(MovieRenderGraph::IVideoCodecWriter* InWriter, FImagePixelData* InPixelData, TArray<FMovieGraphPassData>&& InCompositePasses, TObjectPtr<UMovieGraphEvaluatedConfig> InEvaluatedConfig, const FString& InBranchName) override;
	virtual void BeginFinalize_EncodeThread(MovieRenderGraph::IVideoCodecWriter* InWriter) override;
	virtual void Finalize_EncodeThread(MovieRenderGraph::IVideoCodecWriter* InWriter) override;
	virtual const TCHAR* GetFilenameExtension() const override;
	virtual bool IsAudioSupported() const override;
	// ~UMovieGraphVideoOutputNode Interface

protected:
	/** Per-output writer state; one instance exists per output file. */
	struct FHEVC10CodecWriter : public MovieRenderGraph::IVideoCodecWriter
	{
		/** Resolved on the game thread, used to launch ffmpeg on the encode thread. */
		FHEVC10EncoderSettings Settings;

		FString OutputFileName;

		/** Created lazily in Initialize_EncodeThread so process creation stays off the game thread. */
		TUniquePtr<FHEVC10FFmpegPipe> Pipe;

		/** When OCIO has already converted the image we must not also do an sRGB conversion. */
		bool bSkipColorConversions = false;

		/** Avoid spamming the log if a frame arrives with an unexpected size. */
		bool bLoggedSizeMismatch = false;
	};

	/** Kept so we can fetch the audio state during finalize. */
	TWeakObjectPtr<UMovieGraphPipeline> CachedPipeline;

	/** Frame rate actually used, resolved during Initialize_GameThread. */
	FFrameRate CachedFrameRate = FFrameRate(60, 1);

public:
	/** Which encoder backend to use. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HEVC 10-bit")
	EHEVC10Encoder Encoder = EHEVC10Encoder::NVENC;

	/**
	 * Lower is better quality and a bigger file.
	 * Used as -cq for NVENC and -crf for libx265. 16 is near-lossless, 19 is high, 23 is medium.
	 * Note the built-in MP4 output defaults to a fixed 8 Mbps, which is far too low for 4K.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HEVC 10-bit",
		meta = (ClampMin = 0, ClampMax = 51, UIMin = 0, UIMax = 32))
	int32 Quality = 19;

	/**
	 * Leave empty to look ffmpeg up on PATH - that is what a normal install needs.
	 *
	 * Only set this when the automatic lookup fails, or to force a specific build. FFilePath
	 * rather than FString so the details panel offers a browse button instead of making you
	 * type a path from memory.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HEVC 10-bit",
		meta = (FilePathFilter = "exe", DisplayName = "FFmpeg Path"))
	FFilePath FfmpegPath;

	/** Raw ffmpeg arguments inserted just before the output path. Use for exotic tuning only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HEVC 10-bit", meta = (AdvancedDisplay))
	FString ExtraArguments;
};
