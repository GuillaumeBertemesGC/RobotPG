using UnrealBuildTool;

public class CodexBridge : ModuleRules
{
	public CodexBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"HTTPServer",
			"Json",
			"JsonUtilities",
			"KismetCompiler",
			"UnrealEd"
		});
	}
}
