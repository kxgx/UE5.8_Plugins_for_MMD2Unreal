// Copyright Epic Games, Inc. All Rights Reserved.

#include "MRQAutoSegmentModule.h"

#include "MRQAutoSegmentCore.h"
#include "MRQAutoSegmentLibrary.h"
#include "SMRQAutoSegmentPanel.h"

#include "Framework/Docking/TabManager.h"
#include "HAL/IConsoleManager.h"
#include "ISequencer.h"
#include "MovieSceneSequence.h"
#include "SequencerToolMenuContext.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "MRQAutoSegment"

DEFINE_LOG_CATEGORY_STATIC(LogMRQAutoSegment, Log, All);

namespace
{
	const FName PanelTabName(TEXT("MRQAutoSegmentPanel"));
	const FName LevelEditorButtonName(TEXT("MRQAutoSegment.OpenPanel"));
	const FName SequencerButtonName(TEXT("MRQAutoSegment.OpenFromSequencer"));
	const FName SequencerSectionName(TEXT("MRQAutoSegment"));
	const FName LevelEditorOwner(TEXT("MRQAutoSegmentLevelEditor"));
	const FName SequencerOwner(TEXT("MRQAutoSegmentSequencer"));
}

void FMRQAutoSegmentModule::StartupModule()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		PanelTabName,
		FOnSpawnTab::CreateRaw(this, &FMRQAutoSegmentModule::OnSpawnPanel))
		.SetDisplayName(LOCTEXT("TabTitle", "MRQ 分段"))
		.SetTooltipText(LOCTEXT("TabTooltip", "根据内存 / 显存空闲容量，把长镜头切成多个渲染任务"))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

	RegisterMenus();
	RegisterSequencerToolbar();
	RegisterConsoleCommands();
}

void FMRQAutoSegmentModule::ShutdownModule()
{
	UnregisterConsoleCommands();

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(PanelTabName);

	if (UToolMenus::IsToolMenuUIEnabled())
	{
		UToolMenus::Get()->UnregisterOwnerByName(LevelEditorOwner);
		UToolMenus::Get()->UnregisterOwnerByName(SequencerOwner);
	}
}

void FMRQAutoSegmentModule::RegisterMenus()
{
	// Defer until the level editor has built its toolbars, otherwise the extend below is a no-op.
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateLambda([this]()
		{
			FToolMenuOwnerScoped OwnerScoped(LevelEditorOwner);

			UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.PlayToolBar"));
			if (ToolbarMenu == nullptr)
			{
				return;
			}

			FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("MRQAutoSegment");

			FToolMenuEntry Entry = FToolMenuEntry::InitToolBarButton(
				LevelEditorButtonName,
				FUIAction(FExecuteAction::CreateRaw(this, &FMRQAutoSegmentModule::OpenPanel)),
				LOCTEXT("ToolbarLabel", "MRQ 分段"),
				LOCTEXT("ToolbarTooltip", "根据内存 / 显存空闲容量，把长镜头切成多个渲染任务"),
				FSlateIcon());

			Entry.SetCommandList(nullptr);
			Section.AddEntry(Entry);
		}));
}

void FMRQAutoSegmentModule::RegisterSequencerToolbar()
{
	if (!UToolMenus::IsToolMenuUIEnabled())
	{
		return;
	}

	FToolMenuOwnerScoped OwnerScoped(SequencerOwner);

	// ExtendMenu creates the menu when the Sequencer has not registered it yet, and the Sequencer
	// merges whatever is already there when it later builds its own toolbar.
	UToolMenu* SequencerToolbar = UToolMenus::Get()->ExtendMenu(TEXT("Sequencer.MainToolBar"));
	if (SequencerToolbar == nullptr)
	{
		return;
	}

	SequencerToolbar->AddDynamicSection(
		TEXT("MRQAutoSegmentEntries"),
		FNewToolMenuDelegate::CreateLambda([this](UToolMenu* InMenu)
		{
			if (InMenu == nullptr)
			{
				return;
			}

			// The Sequencer puts a USequencerToolMenuContext into its toolbar's menu context, and
			// that is how this button knows exactly which Sequencer it was built for - no guessing
			// about which of several open Sequencers the user meant.
			USequencerToolMenuContext* SequencerContext = InMenu->Context.FindContext<USequencerToolMenuContext>();
			const TWeakPtr<ISequencer> WeakSequencer = (SequencerContext != nullptr)
				? SequencerContext->WeakSequencer
				: TWeakPtr<ISequencer>();

			FToolMenuSection& Section = InMenu->FindOrAddSection(
				SequencerSectionName, LOCTEXT("SequencerSection", "MRQ"));

			Section.AddEntry(FToolMenuEntry::InitToolBarButton(
				SequencerButtonName,
				FUIAction(FExecuteAction::CreateLambda([this, WeakSequencer]()
				{
					OpenPanelForSequencer(WeakSequencer);
				})),
				LOCTEXT("SequencerButtonLabel", "MRQ 分段"),
				LOCTEXT("SequencerButtonTooltip", "按内存 / 显存空闲容量把这条序列切成多个渲染任务，输出文件名带帧范围"),
				FSlateIcon()));
		}),
		FToolMenuInsert(NAME_None, EToolMenuInsertType::Last));

	UE_LOG(LogMRQAutoSegment, Log,
		TEXT("Sequencer toolbar: registered dynamic section '%s' on 'Sequencer.MainToolBar'."),
		TEXT("MRQAutoSegmentEntries"));
}

void FMRQAutoSegmentModule::OpenPanelForSequencer(const TWeakPtr<ISequencer>& InSequencer)
{
	// Spawning the tab is synchronous, so PanelInstance is valid immediately afterwards.
	OpenPanel();

	TSharedPtr<SMRQAutoSegmentPanel> Panel = PanelInstance.Pin();
	if (!Panel.IsValid())
	{
		return;
	}

	UMovieSceneSequence* Sequence = nullptr;
	if (TSharedPtr<ISequencer> Sequencer = InSequencer.Pin())
	{
		Sequence = Sequencer->GetRootMovieSceneSequence();
	}

	Panel->AdoptSequence(Sequence);
}

void FMRQAutoSegmentModule::RegisterConsoleCommands()
{
	IConsoleManager& Manager = IConsoleManager::Get();

	ConsoleCommands.Add(Manager.RegisterConsoleCommand(
		TEXT("MRQAutoSegment.Probe"),
		TEXT("Logs free system RAM, free video memory and free disk space."),
		FConsoleCommandDelegate::CreateRaw(this, &FMRQAutoSegmentModule::HandleProbeCommand)));

	ConsoleCommands.Add(Manager.RegisterConsoleCommand(
		TEXT("MRQAutoSegment.Plan"),
		TEXT("Runs a default segmentation plan against the current hardware and logs the result."),
		FConsoleCommandDelegate::CreateRaw(this, &FMRQAutoSegmentModule::HandlePlanCommand)));

	ConsoleCommands.Add(Manager.RegisterConsoleCommand(
		TEXT("MRQAutoSegment.Open"),
		TEXT("Opens the MRQ Auto Segment panel."),
		FConsoleCommandDelegate::CreateRaw(this, &FMRQAutoSegmentModule::OpenPanel)));
}

void FMRQAutoSegmentModule::UnregisterConsoleCommands()
{
	IConsoleManager& Manager = IConsoleManager::Get();
	for (IConsoleObject* Command : ConsoleCommands)
	{
		if (Command != nullptr)
		{
			Manager.UnregisterConsoleObject(Command);
		}
	}
	ConsoleCommands.Reset();
}

void FMRQAutoSegmentModule::HandleProbeCommand()
{
	const FMRQHardwareBudget Budget = FMRQAutoSegmentCore::ProbeHardware();
	UE_LOG(LogTemp, Display, TEXT("[MRQAutoSegment] RAM  free %s / total %s"),
		*FMRQAutoSegmentCore::FormatBytes(Budget.AvailablePhysicalRAM),
		*FMRQAutoSegmentCore::FormatBytes(Budget.TotalPhysicalRAM));
	UE_LOG(LogTemp, Display, TEXT("[MRQAutoSegment] VRAM free %s / total %s (engine using %s), adapter '%s'"),
		*FMRQAutoSegmentCore::FormatBytes(Budget.AvailableVRAM),
		*FMRQAutoSegmentCore::FormatBytes(Budget.TotalVRAM),
		*FMRQAutoSegmentCore::FormatBytes(Budget.EngineUsedVRAM),
		*Budget.AdapterName);
}

void FMRQAutoSegmentModule::HandlePlanCommand()
{
	const FMRQHardwareBudget Budget = FMRQAutoSegmentCore::ProbeHardware();

	FMRQSegmentRequest Request;
	Request.RangeStart = 0;
	Request.RangeEnd = 599;
	Request.Resolution = FIntPoint(1920, 1080);

	const FMRQSegmentPlan Plan = FMRQAutoSegmentCore::BuildPlan(Request, Budget);

	UE_LOG(LogTemp, Display, TEXT("[MRQAutoSegment] %s"), *Plan.Message);
	for (const FMRQSegmentConstraint& Row : Plan.Constraints)
	{
		UE_LOG(LogTemp, Display, TEXT("[MRQAutoSegment]   %s%s: %s"),
			Row.bBinding ? TEXT("* ") : TEXT("  "), *Row.Name, *Row.Detail);
	}
}

void FMRQAutoSegmentModule::OpenPanel()
{
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(PanelTabName));
}

TSharedRef<SDockTab> FMRQAutoSegmentModule::OnSpawnPanel(const FSpawnTabArgs& Args)
{
	TSharedRef<SMRQAutoSegmentPanel> Panel = SNew(SMRQAutoSegmentPanel);
	PanelInstance = Panel;

	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			Panel
		];
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMRQAutoSegmentModule, MRQAutoSegment)
