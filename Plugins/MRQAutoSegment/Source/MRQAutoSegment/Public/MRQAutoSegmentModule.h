// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class SDockTab;
class FSpawnTabArgs;
class IConsoleObject;

/**
 * Editor module for MRQ Auto Segment.
 *
 * Owns the panel's tab, the toolbar button that opens it and the two console commands.
 * The actual work lives in FMRQAutoSegmentCore and is reachable from Blueprint/Python through
 * UMRQAutoSegmentLibrary; this class only wires up the editor surface.
 */
class FMRQAutoSegmentModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Opens (or focuses) the panel tab. */
	void OpenPanel();

private:
	void RegisterMenus();
	void RegisterConsoleCommands();
	void UnregisterConsoleCommands();

	TSharedRef<SDockTab> OnSpawnPanel(const FSpawnTabArgs& Args);

	void HandleProbeCommand();
	void HandlePlanCommand();

	TArray<IConsoleObject*> ConsoleCommands;
};
