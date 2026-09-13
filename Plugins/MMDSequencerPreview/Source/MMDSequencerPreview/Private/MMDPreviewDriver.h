// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"

class ISequencer;
class USkeletalMeshComponent;

/**
 * Drives MMD2Unreal-imported animations while a Level Sequence is open in Sequencer.
 *
 * How it works
 * ------------
 * MMD2Unreal imports VMD motion as a UAnimSequence and then puts the character's
 * SkeletalMeshComponent into EAnimationMode::AnimationSingleNode with that sequence
 * assigned as AnimToPlay. Such a component only evaluates its animation while the game
 * world ticks, which is why the dance is visible in Movie Render Queue output but frozen
 * in the editor viewport.
 *
 * This driver watches for Sequencers being created, and while one is alive it:
 *   - turns on USkeletalMeshComponent::bUpdateAnimationInEditor for MMD-driven components
 *   - keeps them playing
 *   - sets their SingleNode position to the Sequencer's current time, so scrubbing and
 *     playback show the correct frame
 * When the last Sequencer closes, every flag it touched is put back exactly as it was.
 *
 * Non-interference
 * ----------------
 *  - Detection is by asset marker only (a UAssetUserData whose class is named
 *    "MMDVmdAssetUserData", which MMD2Unreal attaches to every VMD-imported sequence).
 *    The plugin never links against, calls into, or depends on MMD2Unreal.
 *  - Only runtime component state is touched. No asset, level or sequence is modified
 *    or saved, and no track, binding or animation mode is changed.
 *  - The module is Editor-only, so packaged builds are unaffected.
 *  - Nothing happens during PIE; the game world animates itself.
 */
class FMMDPreviewDriver
{
public:
	void Startup();
	void Shutdown();
	void SetEnabled(bool bInEnabled);
	bool IsEnabled() const { return bEnabled; }

	/** Number of MMD components currently being driven (for the log / status). */
	int32 GetDrivenComponentCount() const { return SavedComponents.Num(); }

private:
	void HandleSequencerCreated(TSharedRef<ISequencer> InSequencer);
	bool Tick(float DeltaTime);

	/** Console command handler: applies the sync immediately ("MMDSequencerPreview.Apply"). */
	void HandleApplyCommand();

	/**
	 * Fired before the editor tears the worlds down. We stop ticking and put every
	 * component back here, while the objects are still alive - doing it later (from
	 * ShutdownModule) is too late and leaves stale state behind during teardown.
	 */
	void HandleEditorPreExit();

	TSharedPtr<ISequencer> GetActiveSequencer();
	void PruneDeadSequencers();
	void RestoreTouchedComponents();

	static bool IsMMDVmdAsset(const UObject* InAsset);
	static bool IsMMDMotionComponent(const USkeletalMeshComponent* InComponent);
	static bool IsMMDCameraActor(const AActor* InActor);

	/** Everything we overwrote on a component, so it can be put back exactly. */
	struct FSavedComponentState
	{
		bool bUpdateAnimationInEditor = false;
		float PlayRate = 1.0f;
	};

	/** Sequencers seen via ISequencerModule::RegisterOnSequencerCreated. */
	TArray<TWeakPtr<ISequencer>> LiveSequencers;

	/** Original state for every component we changed. */
	TMap<TWeakObjectPtr<USkeletalMeshComponent>, FSavedComponentState> SavedComponents;

	FDelegateHandle SequencerCreatedHandle;
	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle EditorPreExitHandle;
	IConsoleObject* ApplyCommand = nullptr;

	bool bEnabled = true;
};
