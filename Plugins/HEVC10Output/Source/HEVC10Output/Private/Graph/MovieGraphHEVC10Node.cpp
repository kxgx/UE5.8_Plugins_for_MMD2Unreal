// Copyright Epic Games, Inc. All Rights Reserved.

#include "Graph/MovieGraphHEVC10Node.h"

#include "HEVC10FFmpegPipe.h"
#include "HEVC10OutputModule.h"

#include "Graph/MovieGraphBlueprintLibrary.h"
#include "Graph/MovieGraphConfig.h"
#include "Graph/MovieGraphPipeline.h"
#include "Graph/Nodes/MovieGraphGlobalOutputSettingNode.h"
#include "ImagePixelData.h"
#include "MoviePipelineImageQuantization.h"
#include "Styling/AppStyle.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MovieGraphHEVC10Node)

#define LOCTEXT_NAMESPACE "MovieGraphHEVC10Node"

UMovieGraphHEVC10Node::UMovieGraphHEVC10Node()
{
}

#if WITH_EDITOR
FText UMovieGraphHEVC10Node::GetNodeTitle(const bool bGetDescriptive) const
{
	static const FText NodeName = NSLOCTEXT("MovieGraphNodes", "NodeName_HEVC10", "H.265 HEVC 10-bit MP4");
	return NodeName;
}

FText UMovieGraphHEVC10Node::GetMenuCategory() const
{
	return NSLOCTEXT("MovieGraphNodes", "HEVC10_Category", "Output Type");
}

FText UMovieGraphHEVC10Node::GetKeywords() const
{
	static const FText Keywords = NSLOCTEXT("MovieGraphNodes", "HEVC10_Keywords",
		"hevc h265 10-bit 10bit mp4 hdr hvc1 nvenc ffmpeg x265");
	return Keywords;
}

FLinearColor UMovieGraphHEVC10Node::GetNodeTitleColor() const
{
	static const FLinearColor NodeColor = FLinearColor(0.12f, 0.42f, 0.78f);
	return NodeColor;
}

FSlateIcon UMovieGraphHEVC10Node::GetIconAndTint(FLinearColor& OutColor) const
{
	static const FSlateIcon Icon = FSlateIcon(FName("MovieRenderPipelineStyle"), "MovieRenderPipeline.Graph.Icon.RenderMovieFile");
	OutColor = FLinearColor::White;
	return Icon;
}

FText UMovieGraphHEVC10Node::GetBasicConfigShortDisplayName() const
{
	// Returning empty hides this node from the simplified "Basic" config output list,
	// so keep a non-empty name to make it selectable there.
	return NSLOCTEXT("MovieGraphNodes", "BasicConfigShortName_HEVC10", "MP4 10-bit");
}
#endif // WITH_EDITOR

TUniquePtr<MovieRenderGraph::IVideoCodecWriter> UMovieGraphHEVC10Node::Initialize_GameThread(
	const FMovieGraphVideoNodeInitializationContext& InInitializationContext)
{
	bool bIncludeCDOs = true;
	constexpr bool bExactMatch = true;
	UMovieGraphGlobalOutputSettingNode* OutputSetting =
		InInitializationContext.EvaluatedConfig->GetSettingForBranch<UMovieGraphGlobalOutputSettingNode>(
			GlobalsPinName, bIncludeCDOs, bExactMatch);

	// Find the evaluated (possibly overridden) instance of this node for the current branch.
	bIncludeCDOs = false;
	const UMovieGraphHEVC10Node* EvaluatedNode = Cast<UMovieGraphHEVC10Node>(
		InInitializationContext.EvaluatedConfig->GetSettingForBranch(
			GetClass(), FName(InInitializationContext.PassData->Key.RootBranchName), bIncludeCDOs, bExactMatch));

	if (!EvaluatedNode)
	{
		UE_LOG(LogHEVC10, Error,
			TEXT("HEVC 10-bit node could not be found in the evaluated graph for branch [%s]."),
			*InInitializationContext.PassData->Key.RootBranchName.ToString());
		return nullptr;
	}

	const FFrameRate SourceFrameRate = InInitializationContext.Pipeline->GetDataSourceInstance()->GetDisplayRate();
	const FFrameRate EffectiveFrameRate = UMovieGraphBlueprintLibrary::GetEffectiveFrameRate(OutputSetting, SourceFrameRate);

	TUniquePtr<FHEVC10CodecWriter> NewWriter = MakeUnique<FHEVC10CodecWriter>();
	NewWriter->StableFileName = InInitializationContext.FileName;
	NewWriter->OutputFileName = InInitializationContext.FileName;

	NewWriter->Settings.FfmpegPath    = EvaluatedNode->FfmpegPath;
	NewWriter->Settings.Resolution    = InInitializationContext.Resolution;
	NewWriter->Settings.FrameRate     = EffectiveFrameRate;
	NewWriter->Settings.bUseNvenc     = (EvaluatedNode->Encoder == EHEVC10Encoder::NVENC);
	NewWriter->Settings.Quality       = EvaluatedNode->Quality;
	NewWriter->Settings.ExtraArguments= EvaluatedNode->ExtraArguments;

	// OCIO is not wired up here, so always let the quantizer perform the linear -> sRGB conversion.
	NewWriter->bSkipColorConversions = false;

	CachedPipeline = InInitializationContext.Pipeline;
	CachedFrameRate = EffectiveFrameRate;

	UE_LOG(LogHEVC10, Log,
		TEXT("HEVC 10-bit output prepared: %dx%d @ %d/%d fps, %s, quality %d -> %s"),
		InInitializationContext.Resolution.X, InInitializationContext.Resolution.Y,
		EffectiveFrameRate.Numerator, EffectiveFrameRate.Denominator,
		NewWriter->Settings.bUseNvenc ? TEXT("hevc_nvenc") : TEXT("libx265"),
		NewWriter->Settings.Quality,
		*InInitializationContext.FileName);

	return NewWriter;
}

bool UMovieGraphHEVC10Node::Initialize_EncodeThread(MovieRenderGraph::IVideoCodecWriter* InWriter)
{
	FHEVC10CodecWriter* CodecWriter = static_cast<FHEVC10CodecWriter*>(InWriter);
	if (!CodecWriter)
	{
		return false;
	}

	// ffmpeg is launched here rather than on the game thread so process creation never
	// stalls a render tick.
	CodecWriter->Pipe = MakeUnique<FHEVC10FFmpegPipe>();
	if (!CodecWriter->Pipe->Start(CodecWriter->OutputFileName, CodecWriter->Settings))
	{
		UE_LOG(LogHEVC10, Error, TEXT("Failed to launch ffmpeg for '%s'."), *CodecWriter->OutputFileName);
		CodecWriter->Pipe.Reset();
		return false;
	}

	return true;
}

void UMovieGraphHEVC10Node::WriteFrame_EncodeThread(
	MovieRenderGraph::IVideoCodecWriter* InWriter,
	FImagePixelData* InPixelData,
	TArray<FMovieGraphPassData>&& InCompositePasses,
	TObjectPtr<UMovieGraphEvaluatedConfig> InEvaluatedConfig,
	const FString& InBranchName)
{
	FHEVC10CodecWriter* CodecWriter = static_cast<FHEVC10CodecWriter*>(InWriter);
	if (!CodecWriter || !CodecWriter->Pipe || !InPixelData)
	{
		return;
	}

	// This is the whole point of the plugin: quantize to 16 bits instead of the 8 bits the
	// built-in MP4 encoder uses, which yields FFloat16Color (interleaved half-float RGBA).
	constexpr int32 TargetBitDepth = 16;
	const bool bConvertToSrgb = !CodecWriter->bSkipColorConversions;

	TUniquePtr<FImagePixelData> QuantizedPixelData =
		UE::MoviePipeline::QuantizeImagePixelDataToBitDepth(InPixelData, TargetBitDepth, nullptr, bConvertToSrgb);

	if (!QuantizedPixelData)
	{
		UE_LOG(LogHEVC10, Warning, TEXT("Failed to quantize a frame to 16 bits; frame dropped."));
		return;
	}

	const void* Data = nullptr;
	int64 DataSize = 0;
	if (!QuantizedPixelData->GetRawData(Data, DataSize) || Data == nullptr)
	{
		UE_LOG(LogHEVC10, Warning, TEXT("Quantized frame had no readable pixel data; frame dropped."));
		return;
	}

	// rgbaf16le is 8 bytes per pixel. Only ever send exactly one frame's worth so ffmpeg's
	// rawvideo demuxer cannot drift out of sync.
	const int64 ExpectedBytes = FHEVC10FFmpegPipe::GetFrameSizeBytes(QuantizedPixelData->GetSize());
	if (DataSize < ExpectedBytes)
	{
		if (!CodecWriter->bLoggedSizeMismatch)
		{
			CodecWriter->bLoggedSizeMismatch = true;
			UE_LOG(LogHEVC10, Error,
				TEXT("Frame is smaller than expected (%lld < %lld bytes for %dx%d). Encoding will be aborted."),
				DataSize, ExpectedBytes, QuantizedPixelData->GetSize().X, QuantizedPixelData->GetSize().Y);
		}
		return;
	}

	CodecWriter->Pipe->WriteFrame(Data, ExpectedBytes);
}

void UMovieGraphHEVC10Node::BeginFinalize_EncodeThread(MovieRenderGraph::IVideoCodecWriter* InWriter)
{
	// Audio is not muxed by this node yet; the Movie Render Queue WAV output can be combined
	// with the resulting MP4 afterwards.
}

void UMovieGraphHEVC10Node::Finalize_EncodeThread(MovieRenderGraph::IVideoCodecWriter* InWriter)
{
	FHEVC10CodecWriter* CodecWriter = static_cast<FHEVC10CodecWriter*>(InWriter);
	if (!CodecWriter || !CodecWriter->Pipe)
	{
		return;
	}

	int32 ReturnCode = -1;
	const bool bSuccess = CodecWriter->Pipe->Finish(ReturnCode);

	if (bSuccess)
	{
		UE_LOG(LogHEVC10, Log, TEXT("Wrote 10-bit HEVC: %s"), *CodecWriter->OutputFileName);
	}
	else
	{
		UE_LOG(LogHEVC10, Error, TEXT("ffmpeg failed (code %d) for %s"), ReturnCode, *CodecWriter->OutputFileName);
	}

	CodecWriter->Pipe.Reset();
}

const TCHAR* UMovieGraphHEVC10Node::GetFilenameExtension() const
{
	return TEXT("mp4");
}

bool UMovieGraphHEVC10Node::IsAudioSupported() const
{
	return false;
}

#undef LOCTEXT_NAMESPACE
