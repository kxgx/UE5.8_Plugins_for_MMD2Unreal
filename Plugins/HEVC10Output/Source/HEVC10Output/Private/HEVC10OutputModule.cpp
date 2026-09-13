#include "HEVC10OutputModule.h"

#define LOCTEXT_NAMESPACE "FHEVC10OutputModule"

DEFINE_LOG_CATEGORY(LogHEVC10);

void FHEVC10OutputModule::StartupModule()
{
	UE_LOG(LogHEVC10, Log, TEXT("HEVC 10-bit Output module started."));
}

void FHEVC10OutputModule::ShutdownModule()
{
	UE_LOG(LogHEVC10, Log, TEXT("HEVC 10-bit Output module shut down."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FHEVC10OutputModule, HEVC10Output)
