// #pl-version 1

using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaModelingWorkspace : WorkspaceRules
{
    public PlasmaModelingWorkspace(BuildContext context) : base(context)
    {
        TargetNames.Add("ModelingPlugin");
        TargetNames.Add("EditorPluginModeling");
    }
}
