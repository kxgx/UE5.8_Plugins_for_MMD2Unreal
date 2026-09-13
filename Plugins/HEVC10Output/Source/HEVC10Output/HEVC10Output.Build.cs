using UnrealBuildTool;

public class HEVC10Output : ModuleRules
{
	public HEVC10Output(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			// Movie Render Pipeline (UMovieGraphVideoOutputNode / UMoviePipelineVideoOutputBase live here)
			"MovieRenderPipelineCore",
			// FImagePixelData, EImagePixelType, ERGBFormat
			"ImageWriteQueue",
			// FSlateIcon, used by the graph node's editor-only overrides
			"SlateCore",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RenderCore",
			"RHI",
			"Slate",
		});
	}
}
