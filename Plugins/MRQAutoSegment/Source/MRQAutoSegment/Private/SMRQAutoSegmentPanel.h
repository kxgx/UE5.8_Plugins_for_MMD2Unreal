// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MRQAutoSegmentTypes.h"
#include "Widgets/SCompoundWidget.h"

class STextBlock;
class SVerticalBox;
class ULevelSequence;
class UMovieSceneSequence;
struct FAssetData;
template <typename OptionType> class SComboBox;

/** One selectable Movie Graph output node class. Class == nullptr means "leave it to the graph". */
struct FMRQOutputTypeOption
{
	UClass* Class = nullptr;
	FString Label;
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

	TSharedPtr<SVerticalBox> ConstraintBox;
	TSharedPtr<SVerticalBox> SegmentBox;
	TSharedPtr<STextBlock> SummaryBlock;
	TSharedPtr<STextBlock> StatusBlock;

	// ---------------------------------------------------------------- actions

	void RefreshSequenceList();
	void RefreshOutputTypes();
	void ProbeAndPlan();
	void RebuildPlan();
	void RebuildConstraintRows();
	void RebuildSegmentRows();
	void ApplyRangeFromSequence(ULevelSequence* InSequence);

	FReply HandleProbeClicked();
	FReply HandleGenerateClicked();
	FReply HandleClearClicked();
	FReply HandleRangeFromSequenceClicked();

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
