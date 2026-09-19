// Copyright Epic Games, Inc. All Rights Reserved.

#include "MMDPreviewDriver.h"

#include "MMDSequencerPreviewModule.h"

#include "Animation/AnimationAsset.h"
#include "Animation/AnimSequenceBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "Engine/AssetUserData.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "ISequencer.h"
#include "ISequencerModule.h"
#include "Interfaces/Interface_AssetUserData.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "Misc/CoreMisc.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectIterator.h"

namespace
{
	/** Marker class name MMD2Unreal attaches to every VMD-imported asset. */
	static const TCHAR* GMmdUserDataClassName = TEXT("MMDVmdAssetUserData");

	static TAutoConsoleVariable<int32> CVarMMDPreviewEnabled(
		TEXT("MMDSequencerPreview.Enable"),
		1,
		TEXT("1 = drive MMD2Unreal animations from Sequencer in the editor viewport, 0 = off."),
		ECVF_Default);

	/**
	 * Drive MMD motion while a sequence plays in PIE as well.
	 *
	 * Movie Render Queue renders through PIE, so without this a render is the one place the
	 * motion is not synced to sequence time: the VMD plays from its own frame 0 in every job and
	 * every rendered segment ends up showing the same motion. That looks exactly like "the frame
	 * ranges are being ignored", which is very misleading.
	 */
	static TAutoConsoleVariable<int32> CVarMMDPreviewInPIE(
		TEXT("MMDSequencerPreview.DriveInPIE"),
		1,
		TEXT("1 = also sync MMD motion to the sequence playing in PIE (needed for MRQ renders), 0 = off."),
		ECVF_Default);

	/** The player currently driving a sequence in InWorld, if there is one. */
	static ULevelSequencePlayer* FindSequencePlayer(UWorld* InWorld)
	{
		if (InWorld == nullptr)
		{
			return nullptr;
		}

		ULevelSequencePlayer* Fallback = nullptr;
		for (TActorIterator<ALevelSequenceActor> It(InWorld); It; ++It)
		{
			ULevelSequencePlayer* Player = It->GetSequencePlayer();
			if (Player == nullptr)
			{
				continue;
			}

			// A world can hold several sequence actors; the running one is the render's.
			if (Player->IsPlaying())
			{
				return Player;
			}
			Fallback = Player;
		}
		return Fallback;
	}

	/** Seconds, from a frame time that may use any frame rate. */
	static double ToSeconds(const FQualifiedFrameTime& InTime)
	{
		return InTime.Rate.AsDecimal() > 0.0
			? InTime.Time.AsDecimal() / InTime.Rate.AsDecimal()
			: 0.0;
	}
}

void FMMDPreviewDriver::Startup()
{
	// Sequencer creation hook.
	if (ISequencerModule* SequencerModule =
			FModuleManager::GetModulePtr<ISequencerModule>("Sequencer"))
	{
		SequencerCreatedHandle = SequencerModule->RegisterOnSequencerCreated(
			FOnSequencerCreated::FDelegate::CreateRaw(this, &FMMDPreviewDriver::HandleSequencerCreated));
	}
	else
	{
		UE_LOG(LogMMDSequencerPreview, Warning,
			TEXT("Sequencer module was not loaded; MMD preview sync will not start."));
		return;
	}

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FMMDPreviewDriver::Tick));

	ApplyCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("MMDSequencerPreview.Apply"),
		TEXT("Apply the MMD preview sync right now (normally it happens every editor tick)."),
		FConsoleCommandDelegate::CreateRaw(this, &FMMDPreviewDriver::HandleApplyCommand),
		ECVF_Default);

	// Restore early: ShutdownModule runs after the worlds are gone, which is too late to
	// put component state back and can leave the process in a bad state at exit.
	EditorPreExitHandle = FEditorDelegates::OnEditorPreExit.AddRaw(
		this, &FMMDPreviewDriver::HandleEditorPreExit);

	UE_LOG(LogMMDSequencerPreview, Log,
		TEXT("Started. Opening a Level Sequence in Sequencer will now drive MMD motion in the viewport, and a sequence playing in PIE will drive it during renders."));
}

void FMMDPreviewDriver::HandleEditorPreExit()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	bEnabled = false;
	RestoreTouchedComponents();
	LiveSequencers.Reset();
}

void FMMDPreviewDriver::HandleApplyCommand()
{
	Tick(0.0f);
	UE_LOG(LogMMDSequencerPreview, Log, TEXT("Applied on demand; %d component(s) driven."),
		SavedComponents.Num());
}

void FMMDPreviewDriver::Shutdown()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	if (SequencerCreatedHandle.IsValid())
	{
		if (ISequencerModule* SequencerModule =
				FModuleManager::GetModulePtr<ISequencerModule>("Sequencer"))
		{
			SequencerModule->UnregisterOnSequencerCreated(SequencerCreatedHandle);
		}
		SequencerCreatedHandle.Reset();
	}

	if (EditorPreExitHandle.IsValid())
	{
		FEditorDelegates::OnEditorPreExit.Remove(EditorPreExitHandle);
		EditorPreExitHandle.Reset();
	}

	if (ApplyCommand != nullptr)
	{
		IConsoleManager::Get().UnregisterConsoleObject(ApplyCommand);
		ApplyCommand = nullptr;
	}

	RestoreTouchedComponents();
	LiveSequencers.Reset();
}

void FMMDPreviewDriver::SetEnabled(bool bInEnabled)
{
	bEnabled = bInEnabled;
	if (!bEnabled)
	{
		RestoreTouchedComponents();
	}
}

void FMMDPreviewDriver::HandleSequencerCreated(TSharedRef<ISequencer> InSequencer)
{
	LiveSequencers.Add(InSequencer);

	// Deliberately no work here: this fires while Sequencer is still constructing itself,
	// and touching skeletal meshes at that moment is unsafe. The ticker picks it up on the
	// next editor frame, and "MMDSequencerPreview.Apply" can force it on demand.

	UE_LOG(LogMMDSequencerPreview, Verbose, TEXT("Sequencer created; %d tracked."), LiveSequencers.Num());
}

void FMMDPreviewDriver::PruneDeadSequencers()
{
	LiveSequencers.RemoveAll([](const TWeakPtr<ISequencer>& Entry)
	{
		return !Entry.IsValid();
	});
}

TSharedPtr<ISequencer> FMMDPreviewDriver::GetActiveSequencer()
{
	PruneDeadSequencers();

	// Most recently created still-alive Sequencer wins.
	for (int32 Index = LiveSequencers.Num() - 1; Index >= 0; --Index)
	{
		if (TSharedPtr<ISequencer> Pinned = LiveSequencers[Index].Pin())
		{
			return Pinned;
		}
	}
	return nullptr;
}

bool FMMDPreviewDriver::IsMMDVmdAsset(const UObject* InAsset)
{
	if (InAsset == nullptr)
	{
		return false;
	}

	const IInterface_AssetUserData* UserDataInterface = Cast<const IInterface_AssetUserData>(InAsset);
	if (UserDataInterface == nullptr)
	{
		return false;
	}

	const TArray<UAssetUserData*>* UserDataArray = UserDataInterface->GetAssetUserDataArray();
	if (UserDataArray == nullptr)
	{
		return false;
	}

	for (const UAssetUserData* Entry : *UserDataArray)
	{
		if (Entry != nullptr && Entry->GetClass()->GetName().Contains(GMmdUserDataClassName))
		{
			return true;
		}
	}
	return false;
}

bool FMMDPreviewDriver::IsMMDMotionComponent(const USkeletalMeshComponent* InComponent)
{
	if (InComponent == nullptr)
	{
		return false;
	}

	// Only SingleNode-driven components are ours to drive; anything using an AnimBP or
	// a Sequencer track must be left alone.
	if (InComponent->GetAnimationMode() != EAnimationMode::AnimationSingleNode)
	{
		return false;
	}

	UAnimationAsset* AnimToPlay = InComponent->AnimationData.AnimToPlay;
	if (AnimToPlay == nullptr)
	{
		return false;
	}

	// Precise marker: MMD2Unreal stamps VMD-imported sequences with this user data.
	return IsMMDVmdAsset(AnimToPlay);
}

bool FMMDPreviewDriver::Tick(float DeltaTime)
{
	if (!bEnabled || CVarMMDPreviewEnabled.GetValueOnGameThread() == 0)
	{
		return true;
	}

	// During shutdown GEditor and the worlds are going away; touching components then is
	// what makes editors crash on exit.
	if (IsEngineExitRequested())
	{
		return true;
	}

	if (GEditor == nullptr)
	{
		return true;
	}

	// PIE used to be left strictly alone, on the grounds that the game world plays these
	// animations itself. It does not, for MMD motion: the VMD lives on the skeletal mesh
	// component, not on a Sequencer track, so nothing ties it to sequence time. Movie Render
	// Queue renders through PIE, so that gap made every render job replay the motion from its
	// own frame 0. Follow the sequence player in PIE for the same reason we follow Sequencer in
	// the editor - but only while a sequence is actually running there.
	UWorld* TargetWorld = nullptr;
	double TimeSeconds = 0.0;

	if (GEditor->PlayWorld != nullptr)
	{
		if (CVarMMDPreviewInPIE.GetValueOnGameThread() == 0)
		{
			return true;
		}

		ULevelSequencePlayer* Player = FindSequencePlayer(GEditor->PlayWorld);
		if (Player == nullptr)
		{
			// A plain play-in-editor session with no sequence running: nothing to sync to.
			return true;
		}

		TargetWorld = GEditor->PlayWorld;
		TimeSeconds = ToSeconds(Player->GetCurrentTime());
	}
	else
	{
		TargetWorld = GEditor->GetEditorWorldContext().World();
		if (TargetWorld == nullptr)
		{
			RestoreTouchedComponents();
			return true;
		}

		TSharedPtr<ISequencer> Sequencer = GetActiveSequencer();
		if (!Sequencer.IsValid())
		{
			// No sequence open: put everything back the way we found it.
			RestoreTouchedComponents();
			return true;
		}

		TimeSeconds = ToSeconds(Sequencer->GetGlobalTime());
	}

	const float FrameTime = static_cast<float>(TimeSeconds);

	// --- MMD motion: keep SingleNode components evaluating at the sequence's time ----
	for (TObjectIterator<USkeletalMeshComponent> It; It; ++It)
	{
		USkeletalMeshComponent* Component = *It;
		if (!IsValid(Component) || Component->GetWorld() != TargetWorld)
		{
			continue;
		}
		if (!IsMMDMotionComponent(Component))
		{
			continue;
		}

		if (!SavedComponents.Contains(Component))
		{
			FSavedComponentState Saved;
			Saved.bUpdateAnimationInEditor = Component->GetUpdateAnimationInEditor();
			Saved.PlayRate = Component->GetPlayRate();
			SavedComponents.Add(Component, Saved);
		}

		if (!Component->GetUpdateAnimationInEditor())
		{
			Component->SetUpdateAnimationInEditor(true);
		}

		if (!Component->IsPlaying())
		{
			Component->Play(false);
		}

		// Freeze the animation so it cannot advance on its own between our ticks.
		// Without this the pose creeps forward by one frame's delta time whenever the
		// viewport redraws (e.g. while the mouse moves), which reads as the animation
		// drifting even though the playhead is parked. SetPosition below is the only
		// thing allowed to move it.
		if (!FMath::IsNearlyZero(Component->GetPlayRate()))
		{
			Component->SetPlayRate(0.0f);
		}

		Component->SetPosition(FrameTime, /*bFireNotifies=*/false);
	}

	return true;
}

void FMMDPreviewDriver::RestoreTouchedComponents()
{
	if (SavedComponents.Num() == 0)
	{
		return;
	}

	int32 Restored = 0;
	for (const TPair<TWeakObjectPtr<USkeletalMeshComponent>, FSavedComponentState>& Pair : SavedComponents)
	{
		if (USkeletalMeshComponent* Component = Pair.Key.Get())
		{
			Component->SetUpdateAnimationInEditor(Pair.Value.bUpdateAnimationInEditor);
			Component->SetPlayRate(Pair.Value.PlayRate);
			++Restored;
		}
	}
	SavedComponents.Reset();

	UE_LOG(LogMMDSequencerPreview, Log,
		TEXT("Restored original state on %d component(s)."), Restored);
}
