// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FrameRate.h"

/**
 * Configuration for the ffmpeg-backed 10-bit H.265 encoder.
 *
 * Frames are handed to ffmpeg as interleaved half-float RGBA (rgbaf16le) on stdin, so no
 * intermediate image sequence is ever written to disk and encoding proceeds in parallel
 * with the render.
 */
struct FHEVC10EncoderSettings
{
	/** Path to ffmpeg. A bare name such as "ffmpeg.exe" is resolved through PATH. */
	FString FfmpegPath = TEXT("ffmpeg.exe");

	/** Output resolution in pixels. */
	FIntPoint Resolution = FIntPoint(1920, 1080);

	/** Output frame rate; taken from the sequence's effective frame rate. */
	FFrameRate FrameRate = FFrameRate(60, 1);

	/** true = hevc_nvenc (GPU), false = libx265 (CPU). */
	bool bUseNvenc = true;

	/** -cq for NVENC, -crf for libx265. Lower is better quality. Sensible range 16-24. */
	int32 Quality = 19;

	/** Raw arguments inserted just before the output path. */
	FString ExtraArguments;
};

/**
 * Owns an ffmpeg child process whose stdin is a raw video stream.
 *
 * Lifecycle: Start() -> WriteFrame() * N -> Finish(). All methods are expected to be called
 * from the same thread (the Movie Render Pipeline encode thread), which is exactly what
 * UMovieGraphVideoOutputNode provides.
 */
class FHEVC10FFmpegPipe
{
public:
	FHEVC10FFmpegPipe() = default;
	~FHEVC10FFmpegPipe();

	FHEVC10FFmpegPipe(const FHEVC10FFmpegPipe&) = delete;
	FHEVC10FFmpegPipe& operator=(const FHEVC10FFmpegPipe&) = delete;

	/** Launches ffmpeg. Returns false and logs on failure. */
	bool Start(const FString& InOutputFile, const FHEVC10EncoderSettings& InSettings);

	/** Writes one entire frame. Returns false if the pipe broke (usually ffmpeg died). */
	bool WriteFrame(const void* InData, const int64 InNumBytes);

	/** Closes stdin, waits for ffmpeg to exit and reports its return code. */
	bool Finish(int32& OutReturnCode);

	/** Kills ffmpeg if it is still running and releases all handles. Safe to call twice. */
	void Abort();

	bool IsRunning();

	int64 GetBytesWritten() const { return BytesWritten; }
	int64 GetFramesWritten() const { return FramesWritten; }
	const FString& GetCommandLine() const { return CommandLine; }
	const FString& GetResolvedFfmpegPath() const { return ResolvedFfmpegPath; }

	/** Resolves a configured ffmpeg path, falling back to a PATH search. */
	static bool ResolveFfmpegPath(const FString& InConfigured, FString& OutResolved);

	/** Size in bytes of a single rgbaf16le frame at the given resolution. */
	static int64 GetFrameSizeBytes(const FIntPoint& InResolution);

private:
	void CloseHandles();

	FProcHandle ProcHandle;
	/** Child's end of the stdin pipe (inherited by ffmpeg). */
	void* ChildStdinPipe = nullptr;
	/** Parent's end of the stdin pipe; this is where frames are written. */
	void* ParentStdinPipe = nullptr;

	FString CommandLine;
	FString ResolvedFfmpegPath;
	FString OutputFile;

	int64 BytesWritten = 0;
	int64 FramesWritten = 0;
};
