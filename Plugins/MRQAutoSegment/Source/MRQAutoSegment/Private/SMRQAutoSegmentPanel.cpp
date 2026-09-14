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
#include "MoviePipelineQueue.h"
#include "Graph/Nodes/MovieGraphFileOutputNode.h"
#include "Misc/Paths.h"
#include "Styling/CoreStyle.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

#include "Widgets/Input/SButton.h"
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
	OutputDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MovieRenders"));

	ModeOptions.Add(MakeShared<EMRQSegmentMode>(EMRQSegmentMode::Auto));
	ModeOptions.Add(MakeShared<EMRQSegmentMode>(EMRQSegmentMode::FixedCount));
	ModeOptions.Add(MakeShared<EMRQSegmentMode>(EMRQSegmentMode::FixedLength));
	SelectedMode = ModeOptions[0];

	RefreshSequenceList();
	RefreshOutputTypes();

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
				SNew(SComboBox<TSharedPtr<FAssetData>>)
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
					ProbeAndPlan();
				}))
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
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(false)
					.MinValue(1)
					.MinDesiredValueWidth(72.0f)
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
					.Value_Lambda([this]() { return TOptional<int32>(Request.Resolution.Y); })
					.OnValueChanged_Lambda([this](int32 NewValue) { Request.Resolution.Y = NewValue; RebuildPlan(); })
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("LabelBytesPerPixel", "每像素字节"))
				]

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(false)
					.MinValue(1)
					.MinDesiredValueWidth(48.0f)
					.Value_Lambda([this]() { return TOptional<int32>(Request.BytesPerPixel); })
					.OnValueChanged_Lambda([this](int32 NewValue) { Request.BytesPerPixel = NewValue; RebuildPlan(); })
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
				.ToolTipText(LOCTEXT("ClearTip", "删除本插件生成的任务（按任务名前缀匹配）"))
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

	if (SequenceAssets.Num() > 0)
	{
		SelectedSequence = SequenceAssets[0];
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

	const UMovieScene* MovieScene = Sequence->GetMovieScene();
	const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameRate DisplayRate = MovieScene->GetDisplayRate();

	const FFrameNumber StartTick = PlaybackRange.GetLowerBoundValue();
	const FFrameNumber EndTick = PlaybackRange.GetUpperBoundValue();

	Request.RangeStart = FFrameRate::TransformTime(FFrameTime(StartTick), TickResolution, DisplayRate).FloorToFrame().Value;
	Request.RangeEnd = FFrameRate::TransformTime(FFrameTime(EndTick), TickResolution, DisplayRate).FloorToFrame().Value;

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
	Template.OutputDirectory = OutputDirectory;
	Template.FileNameFormat = FileNameFormat;
	Template.Resolution = Request.Resolution;
	Template.JobNamePrefix = UMRQAutoSegmentLibrary::GetDefaultJobNamePrefix();

	// Only pin an output type when a concrete one was picked; otherwise the generated graph decides.
	if (SelectedOutputType.IsValid() && SelectedOutputType->Class != nullptr)
	{
		Template.OutputTypes.Add(SelectedOutputType->Class);
	}

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

	if (StatusBlock.IsValid())
	{
		StatusBlock->SetText(FText::FromString(FString::Printf(TEXT("已清除 %d 个由本插件生成的任务。"), Removed)));
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
