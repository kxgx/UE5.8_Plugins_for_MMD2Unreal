// Copyright Epic Games, Inc. All Rights Reserved.

#include "MMDSequencerPreviewModule.h"

#include "MMDPreviewDriver.h"

#define LOCTEXT_NAMESPACE "FMMDSequencerPreviewModule"

DEFINE_LOG_CATEGORY(LogMMDSequencerPreview);

TUniquePtr<FMMDPreviewDriver> GMMDPreviewDriver;

void FMMDSequencerPreviewModule::StartupModule()
{
	GMMDPreviewDriver = MakeUnique<FMMDPreviewDriver>();
	GMMDPreviewDriver->Startup();
}

void FMMDSequencerPreviewModule::ShutdownModule()
{
	if (GMMDPreviewDriver.IsValid())
	{
		GMMDPreviewDriver->Shutdown();
		GMMDPreviewDriver.Reset();
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMMDSequencerPreviewModule, MMDSequencerPreview)
