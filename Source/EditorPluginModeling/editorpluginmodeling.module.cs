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

        // the Modeling panel, the mode strip and the marking menu are drawn by the engine UI
        PlasmaPackageSdk.AddImportLibrary(this, context, "plEditorUi");
        PlasmaPackageSdk.AddImportLibrary(this, context, "plGameUiPlugin");
    }
}
