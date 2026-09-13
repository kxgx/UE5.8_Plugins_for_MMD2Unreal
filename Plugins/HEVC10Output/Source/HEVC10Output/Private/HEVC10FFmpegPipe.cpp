// Copyright Epic Games, Inc. All Rights Reserved.

#include "HEVC10FFmpegPipe.h"

#include "HEVC10OutputModule.h"

#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"

namespace
{
	/** Keep individual WriteFile calls modest; anonymous pipe buffers are small. */
	constexpr int64 GWriteChunkBytes = 8 * 1024 * 1024;
}

int64 FHEVC10FFmpegPipe::GetFrameSizeBytes(const FIntPoint& InResolution)
{
	// rgbaf16le = 4 channels * 16 bits (half float) = 8 bytes per pixel.
	return static_cast<int64>(FMath::Max(1, InResolution.X)) *
	       static_cast<int64>(FMath::Max(1, InResolution.Y)) * 8;
}

bool FHEVC10FFmpegPipe::ResolveFfmpegPath(const FString& InConfigured, FString& OutResolved)
{
	// 1) An explicit path that exists on disk.
	if (!InConfigured.IsEmpty() && FPaths::FileExists(InConfigured))
	{
		OutResolved = FPaths::ConvertRelativePathToFull(InConfigured);
		return true;
	}

	// 2) A bare executable name (or a name with an extension) searched along PATH.
	FString FileName = FPaths::GetCleanFilename(InConfigured);
	if (FileName.IsEmpty())
	{
		FileName = TEXT("ffmpeg.exe");
	}
#if PLATFORM_WINDOWS
	if (!FileName.Contains(TEXT(".")))
	{
		FileName += TEXT(".exe");
	}
#endif

	const FString PathEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("PATH"));
#if PLATFORM_WINDOWS
	const TCHAR* PathDelimiter = TEXT(";");
#else
	const TCHAR* PathDelimiter = TEXT(":");
#endif
	TArray<FString> SearchDirs;
	PathEnv.ParseIntoArray(SearchDirs, PathDelimiter, true);

	for (const FString& Dir : SearchDirs)
	{
		if (Dir.IsEmpty())
		{
			continue;
		}
		const FString Candidate = FPaths::Combine(Dir, FileName);
		if (FPaths::FileExists(Candidate))
		{
			OutResolved = FPaths::ConvertRelativePathToFull(Candidate);
			return true;
		}
	}

	return false;
}

bool FHEVC10FFmpegPipe::Start(const FString& InOutputFile, const FHEVC10EncoderSettings& InSettings)
{
	check(!ProcHandle.IsValid());

	OutputFile = FPaths::ConvertRelativePathToFull(InOutputFile);

	if (!ResolveFfmpegPath(InSettings.FfmpegPath, ResolvedFfmpegPath))
	{
		UE_LOG(LogHEVC10, Error,
			TEXT("Could not locate ffmpeg. Configured path: '%s'. Install ffmpeg or set FfmpegPath on the node."),
			*InSettings.FfmpegPath);
		return false;
	}

	const int32 Width  = FMath::Max(1, InSettings.Resolution.X);
	const int32 Height = FMath::Max(1, InSettings.Resolution.Y);
	const int32 Num    = FMath::Max(1, InSettings.FrameRate.Numerator);
	const int32 Den    = FMath::Max(1, InSettings.FrameRate.Denominator);
	const int32 Quality = FMath::Clamp(InSettings.Quality, 0, 51);

	TArray<FString> Args;
	Args.Reserve(40);

	Args.Add(TEXT("-hide_banner"));
	Args.Add(TEXT("-y"));
	Args.Add(TEXT("-loglevel"));
	Args.Add(TEXT("error"));

	// --- input: raw half-float RGBA straight off stdin -----------------------
	Args.Add(TEXT("-f"));
	Args.Add(TEXT("rawvideo"));
	Args.Add(TEXT("-pix_fmt"));
	Args.Add(TEXT("rgbaf16le"));
	Args.Add(TEXT("-s"));
	Args.Add(FString::Printf(TEXT("%dx%d"), Width, Height));
	Args.Add(TEXT("-r"));
	Args.Add(FString::Printf(TEXT("%d/%d"), Num, Den));
	Args.Add(TEXT("-i"));
	Args.Add(TEXT("-"));

	// --- encoder -------------------------------------------------------------
	if (InSettings.bUseNvenc)
	{
		Args.Add(TEXT("-c:v"));      Args.Add(TEXT("hevc_nvenc"));
		Args.Add(TEXT("-profile:v"));Args.Add(TEXT("main10"));
		Args.Add(TEXT("-pix_fmt"));  Args.Add(TEXT("p010le"));
		Args.Add(TEXT("-rc"));       Args.Add(TEXT("vbr"));
		Args.Add(TEXT("-cq"));       Args.Add(FString::FromInt(Quality));
		Args.Add(TEXT("-b:v"));      Args.Add(TEXT("0"));
		Args.Add(TEXT("-preset"));   Args.Add(TEXT("p7"));
		Args.Add(TEXT("-tune"));     Args.Add(TEXT("hq"));
	}
	else
	{
		Args.Add(TEXT("-c:v"));      Args.Add(TEXT("libx265"));
		Args.Add(TEXT("-crf"));      Args.Add(FString::FromInt(Quality));
		Args.Add(TEXT("-pix_fmt"));  Args.Add(TEXT("yuv420p10le"));
		Args.Add(TEXT("-preset"));   Args.Add(TEXT("slow"));
	}

	if (!InSettings.ExtraArguments.IsEmpty())
	{
		Args.Add(InSettings.ExtraArguments.TrimStartAndEnd());
	}

	// 'hvc1' keeps QuickTime and most NLEs happy; +faststart moves the moov atom to the front.
	// Audio is muxed separately, so suppress it here.
	Args.Add(TEXT("-tag:v"));    Args.Add(TEXT("hvc1"));
	Args.Add(TEXT("-an"));
	Args.Add(TEXT("-movflags")); Args.Add(TEXT("+faststart"));
	Args.Add(FString::Printf(TEXT("\"%s\""), *OutputFile));

	CommandLine = FString::Join(Args, TEXT(" "));

	// --- pipe + process ------------------------------------------------------
	// bWritePipeLocal = true means WE keep the write end and the child inherits the read end,
	// which is what we need to feed frames into ffmpeg's stdin.
	if (!FPlatformProcess::CreatePipe(ChildStdinPipe, ParentStdinPipe, /*bWritePipeLocal=*/ true))
	{
		UE_LOG(LogHEVC10, Error, TEXT("Failed to create the stdin pipe for ffmpeg."));
		ChildStdinPipe = nullptr;
		ParentStdinPipe = nullptr;
		return false;
	}

	uint32 ProcessID = 0;
	ProcHandle = FPlatformProcess::CreateProc(
		*ResolvedFfmpegPath,
		*CommandLine,
		/*bLaunchDetached*/      false,
		/*bLaunchHidden*/        true,
		/*bLaunchReallyHidden*/  true,
		&ProcessID,
		/*PriorityModifier*/     0,
		/*WorkingDirectory*/     nullptr,
		/*PipeWriteChild*/       nullptr,        // no stdout redirection
		/*PipeReadChild*/        ChildStdinPipe  // ffmpeg reads frames from here
	);

	if (!ProcHandle.IsValid())
	{
		UE_LOG(LogHEVC10, Error, TEXT("Failed to launch ffmpeg.\n  Exe: %s\n  Args: %s"),
			*ResolvedFfmpegPath, *CommandLine);
		CloseHandles();
		return false;
	}

	UE_LOG(LogHEVC10, Log, TEXT("ffmpeg started (PID %u).\n  %s %s"),
		ProcessID, *ResolvedFfmpegPath, *CommandLine);

	return true;
}

bool FHEVC10FFmpegPipe::WriteFrame(const void* InData, const int64 InNumBytes)
{
	if (ParentStdinPipe == nullptr || InData == nullptr || InNumBytes <= 0)
	{
		return false;
	}

	const uint8* Cursor = static_cast<const uint8*>(InData);
	int64 Remaining = InNumBytes;

	while (Remaining > 0)
	{
		const int32 ThisChunk = static_cast<int32>(FMath::Min<int64>(Remaining, GWriteChunkBytes));

		int32 Written = 0;
		if (!FPlatformProcess::WritePipe(ParentStdinPipe, Cursor, ThisChunk, &Written) || Written <= 0)
		{
			UE_LOG(LogHEVC10, Error,
				TEXT("Write to ffmpeg stdin failed after %lld bytes (%lld frame(s) sent). ffmpeg has probably exited."),
				BytesWritten, FramesWritten);
			return false;
		}

		Cursor += Written;
		Remaining -= Written;
		BytesWritten += Written;
	}

	++FramesWritten;
	return true;
}

bool FHEVC10FFmpegPipe::Finish(int32& OutReturnCode)
{
	OutReturnCode = -1;

	if (!ProcHandle.IsValid())
	{
		return false;
	}

	// Closing the pipe delivers EOF on ffmpeg's stdin, which lets it write the trailer.
	CloseHandles();

	FPlatformProcess::WaitForProc(ProcHandle);

	int32 ReturnCode = -1;
	FPlatformProcess::GetProcReturnCode(ProcHandle, &ReturnCode);
	OutReturnCode = ReturnCode;

	FPlatformProcess::CloseProc(ProcHandle);
	ProcHandle.Reset();

	if (ReturnCode != 0)
	{
		UE_LOG(LogHEVC10, Error, TEXT("ffmpeg exited with code %d."), ReturnCode);
	}
	else
	{
		UE_LOG(LogHEVC10, Log, TEXT("ffmpeg finished cleanly. %lld frame(s), %.1f MB of raw video piped in."),
			FramesWritten, BytesWritten / (1024.0 * 1024.0));
	}

	return ReturnCode == 0;
}

void FHEVC10FFmpegPipe::Abort()
{
	if (ProcHandle.IsValid())
	{
		FPlatformProcess::TerminateProc(ProcHandle, /*bKillTree*/ true);
		FPlatformProcess::WaitForProc(ProcHandle);
		FPlatformProcess::CloseProc(ProcHandle);
		ProcHandle.Reset();
	}

	CloseHandles();
}

bool FHEVC10FFmpegPipe::IsRunning()
{
	return ProcHandle.IsValid() && FPlatformProcess::IsProcRunning(ProcHandle);
}

void FHEVC10FFmpegPipe::CloseHandles()
{
	if (ChildStdinPipe != nullptr || ParentStdinPipe != nullptr)
	{
		// ClosePipe null-checks both handles.
		FPlatformProcess::ClosePipe(ChildStdinPipe, ParentStdinPipe);
		ChildStdinPipe = nullptr;
		ParentStdinPipe = nullptr;
	}
}

FHEVC10FFmpegPipe::~FHEVC10FFmpegPipe()
{
	Abort();
}
