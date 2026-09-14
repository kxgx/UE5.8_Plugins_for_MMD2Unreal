// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class SDockTab;
class FSpawnTabArgs;
class IConsoleObject;
class ISequencer;
class SMRQAutoSegmentPanel;

/**
 * Editor module for MRQ Auto Segment.
 *
 * Owns the panel's tab, the buttons that open it (level editor toolbar and Sequencer toolbar)
 * and the console commands. The actual work lives in FMRQAutoSegmentCore and is reachable from
 * Blueprint/Python through UMRQAutoSegmentLibrary; this class only wires up the editor surface.
 */
class FMRQAutoSegmentModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Opens (or focuses) the panel tab, without touching the current selection. */
	void OpenPanel();

	/**
	 * Opens the panel and points it at a specific Sequencer: its sequence becomes the selected
	 * one and its playback range becomes the frame range. Used by the Sequencer toolbar button,
	 * which knows exactly which Sequencer it was clicked from.
	 */
	void OpenPanelForSequencer(const TWeakPtr<ISequencer>& InSequencer);

private:
	void RegisterMenus();
	void RegisterSequencerToolbar();
	void RegisterConsoleCommands();
	void UnregisterConsoleCommands();

	TSharedRef<SDockTab> OnSpawnPanel(const FSpawnTabArgs& Args);

	void HandleProbeCommand();
	void HandlePlanCommand();

	/** The live panel, so toolbar buttons can push work into it after opening the tab. */
	TWeakPtr<SMRQAutoSegmentPanel> PanelInstance;

	TArray<IConsoleObject*> ConsoleCommands;
};
