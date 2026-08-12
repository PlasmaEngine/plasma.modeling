using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class ModelingPluginTarget : TargetRules
{
    public ModelingPluginTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plModelingPlugin";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);

        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;

        ExtraModules.Add("ModelingPluginModule");
    }
}
