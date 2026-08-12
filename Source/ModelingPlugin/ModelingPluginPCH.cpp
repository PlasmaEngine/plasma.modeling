#include <ModelingPlugin/ModelingPluginDLL.h>
#include <ModelingPlugin/ModelingPluginPCH.h>

PL_STATICLINK_LIBRARY(ModelingPlugin)
{
  if (bReturn)
    return;

  PL_STATICLINK_REFERENCE(ModelingPlugin_Startup);
  PL_STATICLINK_REFERENCE(ModelingPlugin_Modeling_Implementation_EditableMesh);
  PL_STATICLINK_REFERENCE(ModelingPlugin_Modeling_Implementation_BrushGenerators);
  PL_STATICLINK_REFERENCE(ModelingPlugin_Modeling_Implementation_BrushComponent);
}
