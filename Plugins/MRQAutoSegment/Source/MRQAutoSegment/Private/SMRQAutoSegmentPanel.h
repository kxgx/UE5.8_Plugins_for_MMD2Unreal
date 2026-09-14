// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MRQAutoSegmentTypes.h"
#include "Widgets/SCompoundWidget.h"

class STextBlock;
class SVerticalBox;
class ULevelSequence;
class UMovieSceneSequence;
class UMRQSegmentRenderDriver;
struct FAssetData;
struct FMRQJobTemplate;
template <typename OptionType> class SComboBox;

/** One selectable Movie Graph output node class. Class == nullptr means "leave it to the graph". */
struct FMRQOutputTypeOption
{
	UClass* Class = nullptr;
	FString Label;
};

/** One selectable output resolution: a project preset, or the custom entry. */
struct FMRQResolutionOption
{
	FName ProfileName;
	FIntPoint Resolution = FIntPoint(1920, 1080);
	FString Label;
	bool bIsCustom = false;
};

/**
 * The MRQ Auto Segment panel.
 *
 * Shows what the machine currently has free, turns that into a suggested segment length,
 * explains which limit produced that number, and writes the resulting jobs into the
 * Movie Render Queue.
 */
class SMRQAutoSegmentPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMRQAutoSegmentPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/**
	 * Points the panel at a sequence: selects it in the picker and takes the frame range straight
	 * from its playback range. Called by the Sequencer toolbar button, which passes whatever
	 * sequence that Sequencer currently has open.
	 */
	void AdoptSequence(UMovieSceneSequence* InSequence);

private:
	// ---------------------------------------------------------------- state

	FMRQSegmentRequest Request;
	FMRQHardwareBudget Budget;
	FMRQSegmentPlan Plan;

	FString OutputDirectory;
	FString FileNameFormat = TEXT("{sequence_name}");

	TArray<TSharedPtr<FAssetData>> SequenceAssets;
	TSharedPtr<FAssetData> SelectedSequence;
	TSharedPtr<SComboBox<TSharedPtr<FAssetData>>> SequenceCombo;

	TArray<TSharedPtr<EMRQSegmentMode>> ModeOptions;
	TSharedPtr<EMRQSegmentMode> SelectedMode;

	TArray<TSharedPtr<FMRQOutputTypeOption>> OutputTypeOptions;
	TSharedPtr<FMRQOutputTypeOption> SelectedOutputType;

	TArray<TSharedPtr<FMRQResolutionOption>> ResolutionOptions;
	TSharedPtr<FMRQResolutionOption> SelectedResolution;

	FString PresetName;
	TArray<TSharedPtr<FString>> PresetOptions;
	TSharedPtr<FString> SelectedPreset;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> PresetCombo;

	/** Temporal sub-samples written onto the generated jobs. See FMRQJobTemplate. */
	int32 TemporalSampleCount = 8;

	/** Why the current bytes-per-pixel number is what it is. Shown next to the field. */
	FString BytesPerPixelReason;

	// ------------------------------------------------- "render each segment, then merge"

	/** ffmpeg used to join the segments. Empty = look it up on PATH. */
	FString FfmpegPath;

	/** Remove the per-segment folders once the join succeeded. */
	bool bDeleteSegmentsAfterMerge = false;

	/** Skip the join and leave the per-segment files alone. */
	bool bMergeAfterRender = true;

	/** Drives the one-render-per-segment run. Held in the root set while it is working. */
	TObjectPtr<UMRQSegmentRenderDriver> RenderDriver;

	TSharedPtr<SVerticalBox> ConstraintBox;
	TSharedPtr<SVerticalBox> SegmentBox;
	TSharedPtr<STextBlock> SummaryBlock;
	TSharedPtr<STextBlock> StatusBlock;

	// ---------------------------------------------------------------- actions

	void RefreshSequenceList();
	void RefreshOutputTypes();
	void RefreshResolutionPresets();
	void RefreshPresetList();

	/** Reads the panel into a preset, and writes a preset back into the panel. */
	void CapturePreset(FMRQSegmentPreset& OutPreset) const;
	void ApplyPreset(const FMRQSegmentPreset& InPreset);

	/** Where presets live. Created on demand. */
	static FString GetPresetDirectory();

	/** Recomputes the derived bytes-per-pixel for the selected output format. */
	void UpdateAutoBytesPerPixel();

	void ProbeAndPlan();
	void RebuildPlan();
	void RebuildConstraintRows();
	void RebuildSegmentRows();
	void ApplyRangeFromSequence(ULevelSequence* InSequence);

	FReply HandleProbeClicked();
	FReply HandleGenerateClicked();
	FReply HandleClearClicked();
	FReply HandleRenderSegmentsClicked();
	FReply HandleCancelRenderClicked();
	FReply HandleBrowseFfmpegClicked();
	FReply HandleRangeFromSequenceClicked();
	FReply HandleSavePresetClicked();
	FReply HandleLoadPresetClicked();

	/** Builds the template the jobs and the per-segment run are configured from. */
	void FillTemplate(FMRQJobTemplate& OutTemplate) const;

	/** Renders each segment as its own MRQ run, then joins the results. */
	void StartSegmentRender();
	void SetStatus(const FString& InMessage);

	// ---------------------------------------------------------------- helpers

	TSharedRef<SWidget> BuildHardwareSection();
	TSharedRef<SWidget> BuildSettingsSection();
	TSharedRef<SWidget> BuildPlanSection();
	TSharedRef<SWidget> MakeRow(const FText& Label, const TSharedRef<SWidget>& Content);
	TSharedRef<SWidget> MakeHeader(const FText& Label);

	FText GetModeLabel(EMRQSegmentMode Mode) const;

	FString GetSelectedSequencePath() const;
	FString GetSelectedSequenceName() const;
	FString GetCurrentMapPath() const;
};
