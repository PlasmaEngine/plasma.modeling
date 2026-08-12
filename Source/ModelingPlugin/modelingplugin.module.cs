using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class ModelingPluginModule : ModuleRules
{
    public ModelingPluginModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/ModelingPlugin", "ModelingPluginPCH.h",
            "BUILDSYSTEM_BUILDING_MODELINGPLUGIN_LIB");
    }
}
