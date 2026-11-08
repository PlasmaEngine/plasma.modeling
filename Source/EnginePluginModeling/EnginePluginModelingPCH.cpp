#include <EnginePluginModeling/EnginePluginModelingPCH.h>

PL_STATICLINK_LIBRARY(EnginePluginModeling)
{
  if (bReturn)
    return;

  PL_STATICLINK_REFERENCE(EnginePluginModeling_SceneExport_BakeBrushesExportModifier);
}
