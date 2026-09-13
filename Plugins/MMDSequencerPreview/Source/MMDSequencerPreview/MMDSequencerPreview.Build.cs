using UnrealBuildTool;

public class MMDSequencerPreview : ModuleRules
{
	public MMDSequencerPreview(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"UnrealEd",       // GEditor, editor world context
			"Sequencer",      // ISequencer, ISequencerModule
			"MovieScene",     // FQualifiedFrameTime, IMovieScenePlayer
			"LevelSequence",  // ULevelSequence
		});
	}
}
