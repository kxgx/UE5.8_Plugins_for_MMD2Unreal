using UnrealBuildTool;

public class MRQAutoSegment : ModuleRules
{
	public MRQAutoSegment(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			// UMoviePipelineExecutorJob / UMoviePipelineQueue / UMoviePipelineBasicConfig
			"MovieRenderPipelineCore",
			// UMoviePipelineQueueSubsystem, the live queue the panel writes into
			"MovieRenderPipelineEditor",
			// ULevelSequence
			"LevelSequence",
			// UMovieScene::GetPlaybackRange and the Discrete*Lower/Upper helpers in MovieSceneTimeHelpers.h
			"MovieScene",
			// RHIGetTextureMemoryStats, FTextureMemoryStats
			"RHI",
			// FSlateIcon on the toolbar entry
			"SlateCore",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"UnrealEd",
			"ToolMenus",
			"RenderCore",
			// EKeys - referenced by the Slate input widgets' template code
			"InputCore",
			// IAssetRegistry, used to populate the sequence picker
			"AssetRegistry",
			// FJsonObjectConverter, for the save/load preset files
			"JsonUtilities",
			// FJsonValue / FJsonSerializer / LogJson - the converter's templates instantiate here
			"Json",
			"Projects",
			// WorkspaceMenu::GetMenuStructure(), to place the panel tab under Tools
			"WorkspaceMenuStructure",
			// ISequencer + USequencerToolMenuContext, for the Sequencer toolbar button
			"Sequencer",
			// FDesktopPlatformModule::OpenFileDialog, for the ffmpeg browse button
			"DesktopPlatform",
		});
	}
}
