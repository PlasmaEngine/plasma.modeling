#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorFramework/Actions/TransformGizmoActions.h>
#include <EditorPluginModeling/Brush/BrushActions.h>
#include <EditorPluginModeling/Brush/BrushEditTool.h>
#include <GuiFoundation/Action/ActionManager.h>
#include <GuiFoundation/Action/ActionMapManager.h>

plActionDescriptorHandle plBrushActions::s_hBrushTool;

void plBrushActions::RegisterActions()
{
  // "B" — freed from grey boxing by the one-line change in the scene plugin's GizmoActions
  s_hBrushTool = PL_REGISTER_ACTION_1("Gizmo.Mode.Brush", plActionScope::Document, "Gizmo", "B", plGizmoAction, plGetStaticRTTI<plBrushEditTool>());
}

void plBrushActions::UnregisterActions()
{
  plActionManager::UnregisterAction(s_hBrushTool);
}

void plBrushActions::MapActions()
{
  // Injecting into another plugin's maps: they exist only if the scene plugin loaded first, so a
  // missing map is a logged degradation rather than an assert.
  const char* szMenuBars[] = {"EditorPluginScene_DocumentMenuBar", "EditorPluginScene_Scene2MenuBar"};

  for (const char* szMapping : szMenuBars)
  {
    if (plActionMap* pMap = plActionMapManager::GetActionMap(szMapping))
    {
      pMap->MapAction(s_hBrushTool, "G.Gizmos", 5.5f);
    }
    else
    {
      plLog::Warning("Action map '{0}' not found - the Brush tool will not appear in that menu.", szMapping);
    }
  }

  const char* szToolBars[] = {"EditorPluginScene_DocumentToolBar", "EditorPluginScene_Scene2ToolBar"};

  for (const char* szMapping : szToolBars)
  {
    if (plActionMap* pMap = plActionMapManager::GetActionMap(szMapping))
    {
      pMap->MapAction(s_hBrushTool, "GizmoCategory", 5.5f);
    }
    else
    {
      plLog::Warning("Action map '{0}' not found - the Brush tool will not appear in that toolbar.", szMapping);
    }
  }
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_BrushActions);
