#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorPluginModeling/Brush/BrushActions.h>
#include <EditorPluginModeling/Brush/ModelingPanel.moc.h>

PL_PLUGIN_ON_LOADED()
{
  plBrushActions::RegisterActions();
  plBrushActions::MapActions();
  plModelingPanelManager::Startup();
}

PL_PLUGIN_ON_UNLOADED()
{
  plModelingPanelManager::Shutdown();
  plBrushActions::UnregisterActions();
}

PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_EditorPluginModeling);
