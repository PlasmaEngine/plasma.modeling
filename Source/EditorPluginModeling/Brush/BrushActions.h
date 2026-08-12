#pragma once

#include <EditorPluginModeling/EditorPluginModelingDLL.h>
#include <GuiFoundation/Action/BaseActions.h>

/// \brief Registers the Brush edit-tool action and maps it into the scene document's existing menus and
/// toolbars — the package injects into maps the scene plugin registered, it owns none itself.
class PL_EDITORPLUGINMODELING_DLL plBrushActions
{
public:
  static void RegisterActions();
  static void UnregisterActions();

  static void MapActions();

  static plActionDescriptorHandle s_hBrushTool;
};
