// Copyright Epic Games, Inc. All Rights Reserved.

#include "SMRQAutoSegmentPanel.h"

#include "MRQAutoSegmentCore.h"
#include "MRQAutoSegmentLibrary.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/World.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneTimeHelpers.h"
#include "MoviePipelineQueue.h"
#include "Graph/Nodes/MovieGraphFileOutputNode.h"
#include "Graph/MovieGraphProjectSettings.h"
#include "HAL/FileManager.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Styling/CoreStyle.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MRQAutoSegment"

namespace
{
	/** Width of the label column in every settings row. */
	constexpr float LabelColumnWidth = 96.0f;
}

void SMRQAutoSegmentPanel::Construct(const FArguments& InArgs)
{
	// The engine's own default, written the way the engine writes it: project relative, with the
	// {project_dir} token resolved at render time. Resolving it to an absolute path here would bake
	// this machine's project location into every preset that gets saved.
	OutputDirectory = TEXT("{project_dir}/Saved/MovieRenders");

	ModeOptions.Add(MakeShared<EMRQSegmentMode>(EMRQSegmentMode::Auto));
	ModeOptions.Add(MakeShared<EMRQSegmentMode>(EMRQSegmentMode::FixedCount));
	ModeOptions.Add(MakeShared<EMRQSegmentMode>(EMRQSegmentMode::FixedLength));
	SelectedMode = ModeOptions[0];

	RefreshSequenceList();
	RefreshOutputTypes();
	RefreshResolutionPresets();
	RefreshPresetList();
	UpdateAutoBytesPerPixel();

	ChildSlot
	[
		SNew(SBorder)
		.Padding(10.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				BuildHardwareSection()
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				BuildSettingsSection()
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				BuildPlanSection()
			]
		]
	];

	ProbeAndPlan();
}

// ---------------------------------------------------------------------------- layout helpers

TSharedRef<SWidget> SMRQAutoSegmentPanel::MakeHeader(const FText& Label)
{
	return SNew(STextBlock)
		.Text(Label)
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11));
}

TSharedRef<SWidget> SMRQAutoSegmentPanel::MakeRow(const FText& Label, const TSharedRef<SWidget>& Content)
{
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 2.0f, 8.0f, 2.0f)
		[
			SNew(SBox)
			.WidthOverride(LabelColumnWidth)
			[
				SNew(STextBlock).Text(Label)
			]
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		.Padding(0.0f, 2.0f)
		[
			Content
		];
}

// ---------------------------------------------------------------------------- hardware

TSharedRef<SWidget> SMRQAutoSegmentPanel::BuildHardwareSection()
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeHeader(LOCTEXT("HardwareHeader", "硬件空闲容量"))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRow(LOCTEXT("LabelRAM", "系统内存"),
				SNew(STextBlock).Text_Lambda([this]()
				{
					return FText::FromString(FString::Printf(TEXT("%s 可用  /  %s 共"),
						*UMRQAutoSegmentLibrary::FormatBytes(Budget.AvailablePhysicalRAM),
						*UMRQAutoSegmentLibrary::FormatBytes(Budget.TotalPhysicalRAM)));
				}))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRow(LOCTEXT("LabelVRAM", "显存"),
				SNew(STextBlock).Text_Lambda([this]()
				{
					if (!Budget.bVRAMKnown)
					{
						return LOCTEXT("VRAMUnknown", "RHI 未报告，本项不参与限制");
					}
					return FText::FromString(FString::Printf(TEXT("%s 可用  /  %s 共  ·  引擎已用 %s"),
						*UMRQAutoSegmentLibrary::FormatBytes(Budget.AvailableVRAM),
						*UMRQAutoSegmentLibrary::FormatBytes(Budget.TotalVRAM),
						*UMRQAutoSegmentLibrary::FormatBytes(Budget.EngineUsedVRAM)));
				}))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRow(LOCTEXT("LabelGPU", "显卡"),
				SNew(STextBlock).Text_Lambda([this]()
				{
					return FText::FromString(Budget.AdapterName.IsEmpty() ? TEXT("(未知)") : Budget.AdapterName);
				}))
		];
}

// ---------------------------------------------------------------------------- settings

TSharedRef<SWidget> SMRQAutoSegmentPanel::BuildSettingsSection()
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeHeader(LOCTEXT("SettingsHeader", "参数"))
		]

		// --- sequence -----------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRow(LOCTEXT("LabelSequence", "关卡序列"),
				SAssignNew(SequenceCombo, SComboBox<TSharedPtr<FAssetData>>)
				.OptionsSource(&SequenceAssets)
				.InitiallySelectedItem(SelectedSequence)
				.OnGenerateWidget_Lambda([](TSharedPtr<FAssetData> Item)
				{
					return SNew(STextBlock).Text(Item.IsValid()
						? FText::FromString(Item->AssetName.ToString())
						: LOCTEXT("NoSequence", "(无)"));
				})
				.OnSelectionChanged_Lambda([this](TSharedPtr<FAssetData> Item, ESelectInfo::Type)
				{
					SelectedSequence = Item;
					RebuildPlan();
				})
				[
					SNew(STextBlock).Text_Lambda([this]()
					{
						return SelectedSequence.IsValid()
							? FText::FromString(SelectedSequence->AssetName.ToString())
							: LOCTEXT("PickSequence", "选择序列...");
					})
				])
		]

		// --- output directory ---------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRow(LOCTEXT("LabelOutputDir", "输出目录"),
				SNew(SEditableTextBox)
				.Text_Lambda([this]() { return FText::FromString(OutputDirectory); })
				.OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type)
				{
					OutputDirectory = NewText.ToString();
				}))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRow(FText::GetEmpty(),
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
				.Text(LOCTEXT("OutputDirHint", "支持 {project_dir} 等 token，渲染时由引擎展开——默认值就是引擎自己的默认，只是写成项目相对形式，换机器不用改")))
		]

		// --- file name format ---------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRow(LOCTEXT("LabelFileName", "文件名格式"),
				SNew(SEditableTextBox)
				.Text_Lambda([this]() { return FText::FromString(FileNameFormat); })
				.OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type)
				{
					FileNameFormat = NewText.ToString();
					RebuildPlan();
				}))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRow(FText::GetEmpty(),
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("FileNameHint", "帧范围会自动追加在末尾，例：{sequence_name} → {sequence_name}_0000-1023")))
		]

		// --- output type --------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRow(LOCTEXT("LabelOutputType", "输出格式"),
				SNew(SComboBox<TSharedPtr<FMRQOutputTypeOption>>)
				.OptionsSource(&OutputTypeOptions)
				.InitiallySelectedItem(SelectedOutputType)
				.OnGenerateWidget_Lambda([](TSharedPtr<FMRQOutputTypeOption> Item)
				{
					return SNew(STextBlock).Text(Item.IsValid()
						? FText::FromString(Item->Label)
						: FText::GetEmpty());
				})
				.OnSelectionChanged_Lambda([this](TSharedPtr<FMRQOutputTypeOption> Item, ESelectInfo::Type)
				{
					SelectedOutputType = Item;
					// The pixel format follows the output format, so the frame buffer estimate does too.
					UpdateAutoBytesPerPixel();
					RebuildPlan();
				})
				[
					SNew(STextBlock).Text_Lambda([this]()
					{
						return SelectedOutputType.IsValid()
							? FText::FromString(SelectedOutputType->Label)
							: LOCTEXT("OutputTypeDefault", "(默认)");
					})
				])
		]

		// --- resolution ---------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRow(LOCTEXT("LabelResolution", "输出分辨率"),
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SComboBox<TSharedPtr<FMRQResolutionOption>>)
					.OptionsSource(&ResolutionOptions)
					.InitiallySelectedItem(SelectedResolution)
					.OnGenerateWidget_Lambda([](TSharedPtr<FMRQResolutionOption> Item)
					{
						return SNew(STextBlock).Text(Item.IsValid()
							? FText::FromString(Item->Label)
							: FText::GetEmpty());
					})
					.OnSelectionChanged_Lambda([this](TSharedPtr<FMRQResolutionOption> Item, ESelectInfo::Type)
					{
						if (!Item.IsValid())
						{
							return;
						}
						SelectedResolution = Item;
						if (!Item->bIsCustom)
						{
							Request.Resolution = Item->Resolution;
						}
						RebuildPlan();
					})
					[
						SNew(STextBlock).Text_Lambda([this]()
						{
							return SelectedResolution.IsValid()
								? FText::FromString(SelectedResolution->Label)
								: LOCTEXT("ResolutionCustom", "自定义");
						})
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(false)
					.MinValue(1)
					.MinDesiredValueWidth(72.0f)
					.IsEnabled_Lambda([this]() { return !SelectedResolution.IsValid() || SelectedResolution->bIsCustom; })
					.Value_Lambda([this]() { return TOptional<int32>(Request.Resolution.X); })
					.OnValueChanged_Lambda([this](int32 NewValue) { Request.Resolution.X = NewValue; RebuildPlan(); })
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("Times", "×"))
				]

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(false)
					.MinValue(1)
					.MinDesiredValueWidth(72.0f)
					.IsEnabled_Lambda([this]() { return !SelectedResolution.IsValid() || SelectedResolution->bIsCustom; })
					.Value_Lambda([this]() { return TOptional<int32>(Request.Resolution.Y); })
					.OnValueChanged_Lambda([this](int32 NewValue) { Request.Resolution.Y = NewValue; RebuildPlan(); })
				])
		]

		// --- bytes per pixel ----------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRow(LOCTEXT("LabelBytesPerPixel", "每像素字节"),
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]()
					{
						return Request.bAutoBytesPerPixel ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
					{
						Request.bAutoBytesPerPixel = (NewState == ECheckBoxState::Checked);
						UpdateAutoBytesPerPixel();
						RebuildPlan();
					})
					[
						SNew(STextBlock).Text(LOCTEXT("AutoBytesPerPixel", "跟随输出格式"))
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(false)
					.MinValue(1)
					.MinDesiredValueWidth(48.0f)
					.IsEnabled_Lambda([this]() { return !Request.bAutoBytesPerPixel; })
					.Value_Lambda([this]() { return TOptional<int32>(Request.BytesPerPixel); })
					.OnValueChanged_Lambda([this](int32 NewValue) { Request.BytesPerPixel = NewValue; RebuildPlan(); })
				]

				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
					.Text_Lambda([this]() { return FText::FromString(BytesPerPixelReason); })
				])
		]

		// --- temporal samples ---------------------------------------------------
		// Does not feed the planner, so editing it deliberately leaves the segmentation alone -
		// it only lands on the jobs that get generated.
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRow(LOCTEXT("LabelSamples", "时间采样"),
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(false)
					.MinValue(1)
					.MaxValue(64)
					.MinDesiredValueWidth(48.0f)
					.Value_Lambda([this]() { return TOptional<int32>(TemporalSampleCount); })
					.OnValueChanged_Lambda([this](int32 NewValue) { TemporalSampleCount = NewValue; })
				]

				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(12.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
					.Text(LOCTEXT("SamplesHint", "写进每个生成的任务；走图里的采样方式节点，对延迟渲染和路径追踪都生效"))
				])
		]

		// --- frame range --------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRow(LOCTEXT("LabelRange", "整体帧范围"),
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(false)
					.MinDesiredValueWidth(72.0f)
					.Value_Lambda([this]() { return TOptional<int32>(Request.RangeStart); })
					.OnValueChanged_Lambda([this](int32 NewValue) { Request.RangeStart = NewValue; RebuildPlan(); })
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("To", "→"))
				]

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(false)
					.MinDesiredValueWidth(72.0f)
					.Value_Lambda([this]() { return TOptional<int32>(Request.RangeEnd); })
					.OnValueChanged_Lambda([this](int32 NewValue) { Request.RangeEnd = NewValue; RebuildPlan(); })
				]

				+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("FromSequence", "从序列读取"))
					.ToolTipText(LOCTEXT("FromSequenceTip", "用所选关卡序列的播放范围填充上面两个值"))
					.OnClicked(this, &SMRQAutoSegmentPanel::HandleRangeFromSequenceClicked)
				])
		]

		// --- mode ---------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRow(LOCTEXT("LabelMode", "分段方式"),
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SComboBox<TSharedPtr<EMRQSegmentMode>>)
					.OptionsSource(&ModeOptions)
					.InitiallySelectedItem(SelectedMode)
					.OnGenerateWidget_Lambda([this](TSharedPtr<EMRQSegmentMode> Item)
					{
						return SNew(STextBlock).Text(Item.IsValid() ? GetModeLabel(*Item) : FText::GetEmpty());
					})
					.OnSelectionChanged_Lambda([this](TSharedPtr<EMRQSegmentMode> Item, ESelectInfo::Type)
					{
						if (Item.IsValid())
						{
							SelectedMode = Item;
							Request.Mode = *Item;
							RebuildPlan();
						}
					})
					[
						SNew(STextBlock).Text_Lambda([this]() { return GetModeLabel(Request.Mode); })
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("LabelSegmentCount", "段数"))
				]

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(false)
					.MinValue(1)
					.IsEnabled_Lambda([this]() { return Request.Mode == EMRQSegmentMode::FixedCount; })
					.MinDesiredValueWidth(56.0f)
					.Value_Lambda([this]() { return TOptional<int32>(Request.FixedSegmentCount); })
					.OnValueChanged_Lambda([this](int32 NewValue) { Request.FixedSegmentCount = NewValue; RebuildPlan(); })
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("LabelSegmentLen", "每段帧数"))
				]

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(false)
					.MinValue(1)
					.IsEnabled_Lambda([this]() { return Request.Mode == EMRQSegmentMode::FixedLength; })
					.MinDesiredValueWidth(56.0f)
					.Value_Lambda([this]() { return TOptional<int32>(Request.FixedSegmentLength); })
					.OnValueChanged_Lambda([this](int32 NewValue) { Request.FixedSegmentLength = NewValue; RebuildPlan(); })
				])
		]

		// --- caps ---------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRow(LOCTEXT("LabelCaps", "上限 / 补零"),
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(false)
					.MinValue(1)
					.IsEnabled_Lambda([this]() { return Request.Mode == EMRQSegmentMode::Auto; })
					.MinDesiredValueWidth(56.0f)
					.Value_Lambda([this]() { return TOptional<int32>(Request.MaxSegmentLength); })
					.OnValueChanged_Lambda([this](int32 NewValue) { Request.MaxSegmentLength = NewValue; RebuildPlan(); })
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 12.0f, 0.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("FramesPerSegmentCap", "帧/段上限"))
				]

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(false)
					.MinValue(1)
					.MaxValue(10)
					.MinDesiredValueWidth(40.0f)
					.Value_Lambda([this]() { return TOptional<int32>(Request.LabelPadDigits); })
					.OnValueChanged_Lambda([this](int32 NewValue) { Request.LabelPadDigits = NewValue; RebuildPlan(); })
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("PadDigits", "帧号补零位数"))
				])
		]

		// --- capacity use limits ------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRow(LOCTEXT("LabelUseLimit", "使用上限"),
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(false)
					.MinValue(5)
					.MaxValue(100)
					.MinDesiredValueWidth(48.0f)
					.Value_Lambda([this]() { return TOptional<int32>(FMath::RoundToInt(Request.RAMUseLimit * 100.0f)); })
					.OnValueChanged_Lambda([this](int32 NewValue)
					{
						Request.RAMUseLimit = FMath::Clamp(NewValue, 5, 100) / 100.0f;
						RebuildPlan();
					})
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 16.0f, 0.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("PercentRAM", "% 内存空闲"))
				]

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(false)
					.MinValue(5)
					.MaxValue(100)
					.MinDesiredValueWidth(48.0f)
					.Value_Lambda([this]() { return TOptional<int32>(FMath::RoundToInt(Request.VRAMUseLimit * 100.0f)); })
					.OnValueChanged_Lambda([this](int32 NewValue)
					{
						Request.VRAMUseLimit = FMath::Clamp(NewValue, 5, 100) / 100.0f;
						RebuildPlan();
					})
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 12.0f, 0.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("PercentVRAM", "% 显存空闲"))
				]

				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(LOCTEXT("UseLimitHint", "只占用空闲容量的一部分，其余留给系统和其他程序"))
				])
		]

		// --- presets ------------------------------------------------------------
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeRow(LOCTEXT("LabelPreset", "预设"),
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SEditableTextBox)
					.HintText(LOCTEXT("PresetNameHint", "预设名称"))
					.Text_Lambda([this]() { return FText::FromString(PresetName); })
					.OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type)
					{
						PresetName = NewText.ToString();
					})
				]

				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 12.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("SavePreset", "保存预设"))
					.ToolTipText(LOCTEXT("SavePresetTip", "把当前整份配置（序列、输出、分辨率、帧范围、分段方式、使用上限）存成一个预设"))
					.OnClicked(this, &SMRQAutoSegmentPanel::HandleSavePresetClicked)
				]

				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SAssignNew(PresetCombo, SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&PresetOptions)
					.InitiallySelectedItem(SelectedPreset)
					.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
					{
						return SNew(STextBlock).Text(Item.IsValid() ? FText::FromString(*Item) : FText::GetEmpty());
					})
					.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type)
					{
						SelectedPreset = Item;
						if (Item.IsValid())
						{
							PresetName = *Item;
						}
					})
					[
						SNew(STextBlock).Text_Lambda([this]()
						{
							return SelectedPreset.IsValid()
								? FText::FromString(*SelectedPreset)
								: LOCTEXT("NoPreset", "选择预设...");
						})
					]
				]

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("LoadPreset", "加载预设"))
					.ToolTipText(LOCTEXT("LoadPresetTip", "用选中的预设覆盖当前配置"))
					.OnClicked(this, &SMRQAutoSegmentPanel::HandleLoadPresetClicked)
				])
		];
}

// ---------------------------------------------------------------------------- plan

TSharedRef<SWidget> SMRQAutoSegmentPanel::BuildPlanSection()
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeHeader(LOCTEXT("PlanHeader", "分段预设"))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SAssignNew(ConstraintBox, SVerticalBox)
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f)
		[
			SAssignNew(SummaryBlock, STextBlock)
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SScrollBox)

			+ SScrollBox::Slot()
			[
				SAssignNew(SegmentBox, SVerticalBox)
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Rescan", "重新探测硬件"))
				.OnClicked(this, &SMRQAutoSegmentPanel::HandleProbeClicked)
			]

			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Generate", "生成渲染队列"))
				.ToolTipText(LOCTEXT("GenerateTip", "为每一段在影片渲染队列里创建一个任务"))
				.OnClicked(this, &SMRQAutoSegmentPanel::HandleGenerateClicked)
			]

			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("Clear", "清除生成的任务"))
				.ToolTipText(LOCTEXT("ClearTip", "删除本插件生成的任务（按任务名前缀匹配）及其分段序列"))
				.OnClicked(this, &SMRQAutoSegmentPanel::HandleClearClicked)
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SAssignNew(StatusBlock, STextBlock)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
		];
}

// ---------------------------------------------------------------------------- logic

void SMRQAutoSegmentPanel::FillTemplate(FMRQJobTemplate& OutTemplate) const
{
	OutTemplate.OutputDirectory = OutputDirectory;
	OutTemplate.FileNameFormat = FileNameFormat;
	OutTemplate.Resolution = Request.Resolution;
	OutTemplate.JobNamePrefix = UMRQAutoSegmentLibrary::GetDefaultJobNamePrefix();

	// The temporal sample count only exists on the generated jobs; it never influences segmentation.
	OutTemplate.TemporalSampleCount = TemporalSampleCount;

	// Only pin an output type when a concrete one was picked; otherwise the generated graph decides.
	if (SelectedOutputType.IsValid() && SelectedOutputType->Class != nullptr)
	{
		OutTemplate.OutputTypes.Add(SelectedOutputType->Class);
	}
}

void SMRQAutoSegmentPanel::SetStatus(const FString& InMessage)
{
	if (StatusBlock.IsValid())
	{
		StatusBlock->SetText(FText::FromString(InMessage));
	}
}

FText SMRQAutoSegmentPanel::GetModeLabel(EMRQSegmentMode Mode) const
{
	switch (Mode)
	{
	case EMRQSegmentMode::FixedCount:  return LOCTEXT("ModeFixedCount", "固定段数");
	case EMRQSegmentMode::FixedLength: return LOCTEXT("ModeFixedLength", "固定每段长度");
	case EMRQSegmentMode::Auto:
	default:                           return LOCTEXT("ModeAuto", "自动（按硬件空闲）");
	}
}

void SMRQAutoSegmentPanel::RefreshSequenceList()
{
	// Remember the current target first. Rescanning is about hardware, and silently moving the
	// selection to whatever happens to sort first would throw away the user's configuration.
	const FString PreviouslySelected = GetSelectedSequencePath();

	SequenceAssets.Reset();
	SelectedSequence = nullptr;

	FAssetRegistryModule& RegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& Registry = RegistryModule.Get();

	TArray<FAssetData> Found;
	Registry.GetAssetsByClass(ULevelSequence::StaticClass()->GetClassPathName(), Found, /*bSearchSubClasses=*/true);

	Found.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.AssetName.LexicalLess(B.AssetName);
	});

	for (const FAssetData& Asset : Found)
	{
		SequenceAssets.Add(MakeShared<FAssetData>(Asset));
	}

	// Put the previous selection back when it is still around.
	for (const TSharedPtr<FAssetData>& Asset : SequenceAssets)
	{
		if (Asset.IsValid() && Asset->GetObjectPathString() == PreviouslySelected)
		{
			SelectedSequence = Asset;
			break;
		}
	}

	// Only pick a default on a genuinely cold start, never as a side effect of a rescan.
	if (!SelectedSequence.IsValid() && PreviouslySelected.IsEmpty() && SequenceAssets.Num() > 0)
	{
		SelectedSequence = SequenceAssets[0];
	}

	if (SequenceCombo.IsValid())
	{
		SequenceCombo->RefreshOptions();
		SequenceCombo->SetSelectedItem(SelectedSequence);
	}
}

void SMRQAutoSegmentPanel::RefreshOutputTypes()
{
	OutputTypeOptions.Reset();

	// "(default)" first, so a job can keep whatever the generated graph would otherwise pick.
	{
		TSharedPtr<FMRQOutputTypeOption> Option = MakeShared<FMRQOutputTypeOption>();
		Option->Class = nullptr;
		Option->Label = TEXT("(默认，交给图决定)");
		OutputTypeOptions.Add(Option);
		SelectedOutputType = Option;
	}

	TArray<UClass*> Derived;
	GetDerivedClasses(UMovieGraphFileOutputNode::StaticClass(), Derived, /*bRecursive=*/true);

	for (UClass* Class : Derived)
	{
		if (Class == nullptr || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
		{
			continue;
		}

		TSharedPtr<FMRQOutputTypeOption> Option = MakeShared<FMRQOutputTypeOption>();
		Option->Class = Class;
		Option->Label = Class->GetDisplayNameText().ToString();
		OutputTypeOptions.Add(Option);
	}

	OutputTypeOptions.Sort([](const TSharedPtr<FMRQOutputTypeOption>& A, const TSharedPtr<FMRQOutputTypeOption>& B)
	{
		const bool bANone = (A->Class == nullptr);
		const bool bBNone = (B->Class == nullptr);
		if (bANone != bBNone)
		{
			return bANone;
		}
		return A->Label < B->Label;
	});
}

void SMRQAutoSegmentPanel::RefreshResolutionPresets()
{
	ResolutionOptions.Reset();

	// The same list the Movie Graph's own output setting nodes offer, straight from project settings.
	if (const UMovieGraphProjectSettings* Settings = GetDefault<UMovieGraphProjectSettings>())
	{
		for (const FMovieGraphNamedResolution& Preset : Settings->DefaultNamedResolutions)
		{
			if (!Preset.IsValid())
			{
				continue;
			}

			TSharedPtr<FMRQResolutionOption> Option = MakeShared<FMRQResolutionOption>();
			Option->ProfileName = Preset.ProfileName;
			Option->Resolution = Preset.Resolution;
			Option->Label = FString::Printf(TEXT("%s  (%d × %d)"),
				*Preset.ProfileName.ToString(), Preset.Resolution.X, Preset.Resolution.Y);
			ResolutionOptions.Add(Option);
		}
	}

	// "Custom" is always last, and is what the width/height boxes are enabled by.
	{
		TSharedPtr<FMRQResolutionOption> Option = MakeShared<FMRQResolutionOption>();
		Option->ProfileName = FMovieGraphNamedResolution::CustomEntryName;
		Option->Resolution = Request.Resolution;
		Option->Label = TEXT("自定义");
		Option->bIsCustom = true;
		ResolutionOptions.Add(Option);
	}

	// Preselect whichever preset already matches the current resolution; otherwise stay custom.
	SelectedResolution = ResolutionOptions.Last();
	for (const TSharedPtr<FMRQResolutionOption>& Option : ResolutionOptions)
	{
		if (!Option->bIsCustom && Option->Resolution == Request.Resolution)
		{
			SelectedResolution = Option;
			break;
		}
	}
}

void SMRQAutoSegmentPanel::UpdateAutoBytesPerPixel()
{
	const UClass* OutputType = SelectedOutputType.IsValid() ? SelectedOutputType->Class : nullptr;
	const int32 Derived = FMRQAutoSegmentCore::GetBytesPerPixelForOutputType(OutputType, BytesPerPixelReason);

	// Always recompute the explanation, but only overwrite the value when the user let us.
	if (Request.bAutoBytesPerPixel)
	{
		Request.BytesPerPixel = Derived;
	}
}

FString SMRQAutoSegmentPanel::GetPresetDirectory()
{
	// Saved/ rather than Config/, so presets never end up committed by accident. Copy the folder
	// between machines if they should travel.
	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MRQAutoSegment"), TEXT("Presets")));
}

void SMRQAutoSegmentPanel::RefreshPresetList()
{
	const FString PreviouslySelected = SelectedPreset.IsValid() ? *SelectedPreset : FString();

	PresetOptions.Reset();
	SelectedPreset = nullptr;

	const FString Directory = GetPresetDirectory();
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, TEXT("*.json")), /*Files=*/true, /*Directories=*/false);
	Files.Sort([](const FString& A, const FString& B) { return A < B; });

	for (const FString& File : Files)
	{
		PresetOptions.Add(MakeShared<FString>(FPaths::GetBaseFilename(File)));
	}

	for (const TSharedPtr<FString>& Option : PresetOptions)
	{
		if (Option.IsValid() && *Option == PreviouslySelected)
		{
			SelectedPreset = Option;
			break;
		}
	}

	if (PresetCombo.IsValid())
	{
		PresetCombo->RefreshOptions();
		PresetCombo->SetSelectedItem(SelectedPreset);
	}
}

void SMRQAutoSegmentPanel::CapturePreset(FMRQSegmentPreset& OutPreset) const
{
	OutPreset.Name = PresetName;
	OutPreset.SequencePath = GetSelectedSequencePath();
	OutPreset.OutputDirectory = OutputDirectory;
	OutPreset.FileNameFormat = FileNameFormat;
	OutPreset.OutputTypeClass = (SelectedOutputType.IsValid() && SelectedOutputType->Class != nullptr)
		? SelectedOutputType->Class->GetPathName()
		: FString();
	OutPreset.ResolutionProfile = SelectedResolution.IsValid()
		? SelectedResolution->ProfileName.ToString()
		: FString();
	OutPreset.Request = Request;
	OutPreset.TemporalSampleCount = TemporalSampleCount;
}

void SMRQAutoSegmentPanel::ApplyPreset(const FMRQSegmentPreset& InPreset)
{
	OutputDirectory = InPreset.OutputDirectory;
	FileNameFormat = InPreset.FileNameFormat;
	Request = InPreset.Request;

	TemporalSampleCount = FMath::Max(InPreset.TemporalSampleCount, 1);

	// Output format. An empty class path means the preset was saved on the "(default)" entry.
	SelectedOutputType = nullptr;
	for (const TSharedPtr<FMRQOutputTypeOption>& Option : OutputTypeOptions)
	{
		if (Option.IsValid() && Option->Class != nullptr
			&& Option->Class->GetPathName() == InPreset.OutputTypeClass)
		{
			SelectedOutputType = Option;
			break;
		}
	}
	if (!SelectedOutputType.IsValid() && OutputTypeOptions.Num() > 0)
	{
		SelectedOutputType = OutputTypeOptions[0];
	}
	UpdateAutoBytesPerPixel();

	// Sequence.
	SelectedSequence = nullptr;
	for (const TSharedPtr<FAssetData>& Asset : SequenceAssets)
	{
		if (Asset.IsValid() && Asset->GetObjectPathString() == InPreset.SequencePath)
		{
			SelectedSequence = Asset;
			break;
		}
	}
	if (SequenceCombo.IsValid())
	{
		SequenceCombo->SetSelectedItem(SelectedSequence);
	}

	// Resolution preset. A profile we no longer know about leaves the plain resolution from
	// Request in place, which is the right fallback.
	SelectedResolution = nullptr;
	for (const TSharedPtr<FMRQResolutionOption>& Option : ResolutionOptions)
	{
		if (Option.IsValid() && Option->ProfileName.ToString() == InPreset.ResolutionProfile)
		{
			SelectedResolution = Option;
			break;
		}
	}

	RebuildPlan();
}

FReply SMRQAutoSegmentPanel::HandleSavePresetClicked()
{
	FMRQSegmentPreset Preset;
	CapturePreset(Preset);

	// A nameless save still has to produce something findable.
	if (Preset.Name.IsEmpty())
	{
		Preset.Name = GetSelectedSequenceName();
	}
	Preset.Name = FPaths::MakeValidFileName(Preset.Name, TEXT('_'));
	PresetName = Preset.Name;

	const FString Directory = GetPresetDirectory();
	IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true);

	const FString FilePath = FPaths::Combine(Directory, Preset.Name + TEXT(".json"));

	FString Json;
	if (!FJsonObjectConverter::UStructToJsonObjectString(Preset, Json))
	{
		if (StatusBlock.IsValid())
		{
			StatusBlock->SetText(LOCTEXT("PresetSerializeFailed", "预设序列化失败。"));
		}
		return FReply::Handled();
	}

	if (!FFileHelper::SaveStringToFile(Json, *FilePath))
	{
		if (StatusBlock.IsValid())
		{
			StatusBlock->SetText(FText::FromString(FString::Printf(TEXT("写文件失败：%s"), *FilePath)));
		}
		return FReply::Handled();
	}

	RefreshPresetList();
	if (StatusBlock.IsValid())
	{
		StatusBlock->SetText(FText::FromString(FString::Printf(
			TEXT("已保存预设「%s」→ %s"), *Preset.Name, *FilePath)));
	}
	return FReply::Handled();
}

FReply SMRQAutoSegmentPanel::HandleLoadPresetClicked()
{
	if (!SelectedPreset.IsValid())
	{
		if (StatusBlock.IsValid())
		{
			StatusBlock->SetText(LOCTEXT("NoPresetChosen", "请先在下拉里选一个预设。"));
		}
		return FReply::Handled();
	}

	const FString FilePath = FPaths::Combine(GetPresetDirectory(), (*SelectedPreset) + TEXT(".json"));

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *FilePath))
	{
		if (StatusBlock.IsValid())
		{
			StatusBlock->SetText(FText::FromString(FString::Printf(TEXT("读不到预设文件：%s"), *FilePath)));
		}
		return FReply::Handled();
	}

	FMRQSegmentPreset Preset;
	if (!FJsonObjectConverter::JsonObjectStringToUStruct(Json, &Preset))
	{
		if (StatusBlock.IsValid())
		{
			StatusBlock->SetText(FText::FromString(FString::Printf(TEXT("预设解析失败：%s"), *FilePath)));
		}
		return FReply::Handled();
	}

	PresetName = Preset.Name;
	ApplyPreset(Preset);

	if (StatusBlock.IsValid())
	{
		StatusBlock->SetText(FText::FromString(FString::Printf(TEXT("已加载预设「%s」。"), *PresetName)));
	}
	return FReply::Handled();
}

void SMRQAutoSegmentPanel::ProbeAndPlan()
{
	Budget = UMRQAutoSegmentLibrary::ProbeHardware();
	RebuildPlan();
}

void SMRQAutoSegmentPanel::RebuildPlan()
{
	Plan = UMRQAutoSegmentLibrary::BuildPlan(Request, Budget);

	// Fill in the file names so the preview shows what will actually land on disk.
	const FString SequenceName = GetSelectedSequenceName();
	const FString Extension = TEXT(".mp4");
	for (FMRQSegmentPlanEntry& Entry : Plan.Segments)
	{
		Entry.OutputName = FMRQAutoSegmentCore::PreviewFileName(FileNameFormat, Entry.Label, SequenceName, Extension);
	}

	RebuildConstraintRows();
	RebuildSegmentRows();
}

void SMRQAutoSegmentPanel::RebuildConstraintRows()
{
	if (!ConstraintBox.IsValid())
	{
		return;
	}

	ConstraintBox->ClearChildren();

	for (const FMRQSegmentConstraint& Row : Plan.Constraints)
	{
		const FString Prefix = Row.bBinding ? TEXT("★ ") : TEXT("   ");
		const FString Frames = (Row.Frames == MAX_int32)
			? TEXT("—")
			: FString::Printf(TEXT("%d 帧"), Row.Frames);

		ConstraintBox->AddSlot()
			.AutoHeight()
			[
				MakeRow(FText::FromString(Prefix + Row.Name),
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("%s    %s"), *Frames, *Row.Detail)))
					.ColorAndOpacity(Row.bBinding
						? FSlateColor(FLinearColor(0.95f, 0.75f, 0.25f))
						: FSlateColor::UseSubduedForeground()))
			];
	}

	if (SummaryBlock.IsValid())
	{
		SummaryBlock->SetText(FText::FromString(Plan.Message));
	}
}

void SMRQAutoSegmentPanel::RebuildSegmentRows()
{
	if (!SegmentBox.IsValid())
	{
		return;
	}

	SegmentBox->ClearChildren();

	if (!Plan.bValid)
	{
		return;
	}

	for (const FMRQSegmentPlanEntry& Entry : Plan.Segments)
	{
		SegmentBox->AddSlot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 1.0f, 8.0f, 1.0f)
				[
					SNew(SBox)
					.WidthOverride(32.0f)
					[
						SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%d"), Entry.Index + 1)))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 1.0f, 12.0f, 1.0f)
				[
					SNew(SBox)
					.WidthOverride(120.0f)
					[
						SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%d → %d"), Entry.StartFrame, Entry.EndFrame)))
					]
				]

				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 1.0f)
				[
					SNew(STextBlock).Text(FText::FromString(Entry.OutputName))
				]
			];
	}
}

FString SMRQAutoSegmentPanel::GetSelectedSequencePath() const
{
	if (!SelectedSequence.IsValid())
	{
		return FString();
	}
	return SelectedSequence->GetObjectPathString();
}

FString SMRQAutoSegmentPanel::GetSelectedSequenceName() const
{
	if (!SelectedSequence.IsValid())
	{
		return TEXT("Sequence");
	}
	return SelectedSequence->AssetName.ToString();
}

FString SMRQAutoSegmentPanel::GetCurrentMapPath() const
{
	if (GEditor != nullptr)
	{
		if (UWorld* World = GEditor->GetEditorWorldContext().World())
		{
			return World->GetPathName();
		}
	}
	return FString();
}

// ---------------------------------------------------------------------------- buttons

FReply SMRQAutoSegmentPanel::HandleProbeClicked()
{
	RefreshSequenceList();
	ProbeAndPlan();

	if (StatusBlock.IsValid())
	{
		StatusBlock->SetText(LOCTEXT("Rescanned", "已重新探测硬件并重算分段。"));
	}
	return FReply::Handled();
}

void SMRQAutoSegmentPanel::ApplyRangeFromSequence(ULevelSequence* InSequence)
{
	if (InSequence == nullptr || InSequence->GetMovieScene() == nullptr)
	{
		return;
	}

	const UMovieScene* MovieScene = InSequence->GetMovieScene();
	const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();

	// The stored range is half-open [Start, End), while this panel's range is inclusive. Use the
	// engine's own discrete helpers - the same ones UMovieSceneSequenceExtensions uses to answer
	// GetPlaybackStart/GetPlaybackEnd - so the numbers here match what Sequencer displays.
	// Reading the raw upper bound would land one frame past the last frame that is actually played.
	const FFrameNumber FirstTick = UE::MovieScene::DiscreteInclusiveLower(PlaybackRange);
	const FFrameNumber LastTick = UE::MovieScene::DiscreteExclusiveUpper(PlaybackRange);

	Request.RangeStart = FFrameRate::TransformTime(
		FFrameTime(FirstTick), MovieScene->GetTickResolution(), MovieScene->GetDisplayRate())
		.FloorToFrame().Value;
	Request.RangeEnd = FFrameRate::TransformTime(
		FFrameTime(LastTick), MovieScene->GetTickResolution(), MovieScene->GetDisplayRate())
		.FloorToFrame().Value;
}

void SMRQAutoSegmentPanel::AdoptSequence(UMovieSceneSequence* InSequence)
{
	ULevelSequence* LevelSequence = Cast<ULevelSequence>(InSequence);
	if (LevelSequence == nullptr)
	{
		return;
	}

	// Select it in the picker when we know about it, so the panel and the Sequencer agree.
	const FString ObjectPath = LevelSequence->GetPathName();
	SelectedSequence = nullptr;
	for (const TSharedPtr<FAssetData>& Candidate : SequenceAssets)
	{
		if (Candidate.IsValid() && Candidate->ToSoftObjectPath().ToString() == ObjectPath)
		{
			SelectedSequence = Candidate;
			break;
		}
	}

	if (SelectedSequence.IsValid() && SequenceCombo.IsValid())
	{
		SequenceCombo->SetSelectedItem(SelectedSequence);
	}

	// The frame range the user is actually looking at is the one they want segmented.
	ApplyRangeFromSequence(LevelSequence);
	RebuildPlan();

	if (StatusBlock.IsValid())
	{
		StatusBlock->SetText(FText::FromString(FString::Printf(
			TEXT("已从 Sequencer 取用序列「%s」，帧范围 %d → %d。"),
			*LevelSequence->GetName(), Request.RangeStart, Request.RangeEnd)));
	}
}

FReply SMRQAutoSegmentPanel::HandleRangeFromSequenceClicked()
{
	const FString SequencePath = GetSelectedSequencePath();
	ULevelSequence* Sequence = SequencePath.IsEmpty() ? nullptr : LoadObject<ULevelSequence>(nullptr, *SequencePath);

	if (Sequence == nullptr || Sequence->GetMovieScene() == nullptr)
	{
		if (StatusBlock.IsValid())
		{
			StatusBlock->SetText(LOCTEXT("NoSequenceLoaded", "请先选择一个关卡序列。"));
		}
		return FReply::Handled();
	}

	ApplyRangeFromSequence(Sequence);
	RebuildPlan();

	if (StatusBlock.IsValid())
	{
		StatusBlock->SetText(FText::FromString(FString::Printf(
			TEXT("已从序列读取播放范围：%d → %d"), Request.RangeStart, Request.RangeEnd)));
	}
	return FReply::Handled();
}

FReply SMRQAutoSegmentPanel::HandleGenerateClicked()
{
	UMoviePipelineQueue* Queue = UMRQAutoSegmentLibrary::GetEditorQueue();
	if (Queue == nullptr)
	{
		if (StatusBlock.IsValid())
		{
			StatusBlock->SetText(LOCTEXT("NoQueue", "拿不到影片渲染队列。请先打开「影片渲染队列」窗口。"));
		}
		return FReply::Handled();
	}

	const FString SequencePath = GetSelectedSequencePath();
	ULevelSequence* Sequence = SequencePath.IsEmpty() ? nullptr : LoadObject<ULevelSequence>(nullptr, *SequencePath);
	if (Sequence == nullptr)
	{
		if (StatusBlock.IsValid())
		{
			StatusBlock->SetText(LOCTEXT("NeedSequence", "请先选择一个关卡序列。"));
		}
		return FReply::Handled();
	}

	if (!Plan.bValid)
	{
		if (StatusBlock.IsValid())
		{
			StatusBlock->SetText(FText::FromString(Plan.Message));
		}
		return FReply::Handled();
	}

	FMRQJobTemplate Template;
	FillTemplate(Template);

	const int32 Created = FMRQAutoSegmentCore::GenerateJobs(
		Queue, Sequence, GetCurrentMapPath(), Plan, Template);

	if (StatusBlock.IsValid())
	{
		StatusBlock->SetText(FText::FromString(FString::Printf(
			TEXT("已生成 %d 个渲染任务，文件名为「%s_<起始帧>-<结束帧>」。打开「影片渲染队列」即可看到。"),
			Created, *FileNameFormat)));
	}
	return FReply::Handled();
}

FReply SMRQAutoSegmentPanel::HandleClearClicked()
{
	UMoviePipelineQueue* Queue = UMRQAutoSegmentLibrary::GetEditorQueue();
	if (Queue == nullptr)
	{
		return FReply::Handled();
	}

	const int32 Removed = UMRQAutoSegmentLibrary::DeleteGeneratedJobs(
		Queue, UMRQAutoSegmentLibrary::GetDefaultJobNamePrefix());

	// The per-segment sequence copies exist only to carry each segment's playback range, so they
	// go with the jobs that pointed at them.
	const int32 RemovedSequences = UMRQAutoSegmentLibrary::DeleteGeneratedSequences();

	if (StatusBlock.IsValid())
	{
		StatusBlock->SetText(FText::FromString(FString::Printf(
			TEXT("已清除 %d 个由本插件生成的任务，以及 %d 个分段序列。"), Removed, RemovedSequences)));
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
