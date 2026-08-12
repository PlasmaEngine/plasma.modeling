#include <EditorPluginModeling/EditorPluginModelingPCH.h>

PL_STATICLINK_LIBRARY(EditorPluginModeling)
{
  if (bReturn)
    return;

  PL_STATICLINK_REFERENCE(EditorPluginModeling_EditorPluginModeling);
  PL_STATICLINK_REFERENCE(EditorPluginModeling_Brush_BrushActions);
  PL_STATICLINK_REFERENCE(EditorPluginModeling_Brush_BrushEditTool);
  PL_STATICLINK_REFERENCE(EditorPluginModeling_Brush_BrushPicking);
  PL_STATICLINK_REFERENCE(EditorPluginModeling_Brush_BrushFaceMaterialDragDropHandler);
  PL_STATICLINK_REFERENCE(EditorPluginModeling_Brush_ModelingPanel);
  PL_STATICLINK_REFERENCE(EditorPluginModeling_Brush_BrushToolContext);
  PL_STATICLINK_REFERENCE(EditorPluginModeling_Brush_BrushModeStrip);
}
