using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EnginePluginModelingModule : ModuleRules
{
    public EnginePluginModelingModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/EnginePluginModeling",
            "EnginePluginModelingPCH.h", "BUILDSYSTEM_BUILDING_ENGINEPLUGINMODELING_LIB");

        PlasmaPackageSdk.ConfigureEditorSdkConsumer(this, context);
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/ModelingPlugin", "plModelingPlugin");
    }
}
