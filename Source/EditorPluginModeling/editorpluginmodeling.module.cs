using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginModelingModule : ModuleRules
{
    public EditorPluginModelingModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/EditorPluginModeling",
            "EditorPluginModelingPCH.h", "BUILDSYSTEM_BUILDING_EDITORPLUGINMODELING_LIB");

        PlasmaPackageSdk.ConfigureEditorSdkConsumer(this, context);

        // the editor plugin reads brush data types (plEditableMesh, generators) for CPU face picking
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/ModelingPlugin", "plModelingPlugin");

        PlasmaPackageQt.ConfigureQtModule(this, context, "EditorPluginModelingModule",
            "Source/EditorPluginModeling", "Core", "Gui", "Widgets");
    }
}
