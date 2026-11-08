using System;
using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginModelingMcpModule : ModuleRules
{
    public EditorPluginModelingMcpModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/EditorPluginModelingMcp",
            "EditorPluginModelingMcpPCH.h", "BUILDSYSTEM_BUILDING_EDITORPLUGINMODELINGMCP_LIB");

        PlasmaPackageSdk.ConfigureEditorSdkConsumer(this, context);
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/ModelingPlugin", "plModelingPlugin");
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/EditorPluginModeling", "plEditorPluginModeling");

        // the MCP package is a build-time sibling; at run time this plugin is only loaded when it is present
        var overridden = Environment.GetEnvironmentVariable("PLASMA_PACKAGE_PATH");
        var searchRoot = string.IsNullOrWhiteSpace(overridden)
            ? Directory.GetParent(Path.GetFullPath(context.ProjectRoot)).FullName
            : Path.GetFullPath(overridden);
        var mcpRoot = Path.Combine(searchRoot, "plasma.mcp");

        PublicIncludePaths.Add(Path.Combine(mcpRoot, "Source"));
        PublicLibraries.Add(Path.Combine(mcpRoot, "Bin", $"{context.Platform}_x64_{context.Configuration}", "plEditorPluginPlasmaMcp.lib"));
    }
}
