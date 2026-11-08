#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorPluginModeling/Brush/BrushActions.h>
#include <EditorPluginModeling/Brush/BrushPropertyStates.h>
#include <EditorPluginModeling/Brush/ModelingPanel.h>
#include <EditorUi/Runtime.h>
#include <EditorUi/Widgets.h>
#include <Foundation/Configuration/Plugin.h>

namespace
{
  /// The package's icon folder: the icons under their ids (":/ModelingIcons/Tools/ModeFace.svg" is
  /// "EditorIcons/ModelingIcons/Tools/ModeFace.svg"). The plugin binary lives in <package>/Bin/<configuration>.
  plString s_sIconFolder;

  void RegisterIconFolder()
  {
    plStringBuilder sPlugin, sCopy;
    plPlugin::GetPluginPaths("plEditorPluginModeling", sPlugin, sCopy, 0);

    plStringBuilder sFolder = sPlugin.GetFileDirectory();
    sFolder.AppendPath("..", "..", "EditorIcons");
    sFolder.MakeCleanPath();

    s_sIconFolder = sFolder;
    plEditorUi::AddIconFolder(s_sIconFolder);
  }

  void UnregisterIconFolder()
  {
    plEditorUi::RemoveIconFolder(s_sIconFolder);
    s_sIconFolder.Clear();
  }

  // The MCP tools live in their own plugin, loaded only once the MCP package is: a hard link would make
  // this plugin fail to load (with an error dialog) in every project without plasma.mcp.
  constexpr const char* s_szMcpPlugin = "plEditorPluginPlasmaMcp";
  constexpr const char* s_szModelingMcpPlugin = "plEditorPluginModelingMcp";

  bool IsPluginLoaded(plStringView sName)
  {
    plDynamicArray<plPlugin::PluginInfo> infos;
    plPlugin::GetAllPluginInfos(infos);

    for (const auto& info : infos)
    {
      if (info.m_sName.GetFileName() == sName)
        return true;
    }

    return false;
  }

  void LoadMcpToolsIfPossible()
  {
    if (IsPluginLoaded(s_szMcpPlugin) && !IsPluginLoaded(s_szModelingMcpPlugin) && plPlugin::ExistsPluginFile(s_szModelingMcpPlugin))
    {
      plPlugin::LoadPlugin(s_szModelingMcpPlugin).IgnoreResult();
    }
  }

  // Runs the deferred load; destroying it on unload cancels a load that has not run yet.
  plEditorUiTimerGroup* s_pDeferredLoad = nullptr;

  void PluginEventHandler(const plPluginEvent& e)
  {
    // Loading a plugin broadcasts plugin events, and this handler runs inside such a broadcast, so the
    // load waits for the next pass of the editor's main loop.
    if (e.m_EventType == plPluginEvent::AfterPluginChanges && s_pDeferredLoad != nullptr)
    {
      s_pDeferredLoad->Start(plTime::MakeZero(), []()
        { LoadMcpToolsIfPossible(); });
    }
  }
} // namespace

PL_PLUGIN_ON_LOADED()
{
  RegisterIconFolder();

  plBrushActions::RegisterActions();
  plBrushActions::MapActions();
  plBrushPropertyStates::Startup();
  plModelingPanelManager::Startup();

  s_pDeferredLoad = PL_DEFAULT_NEW(plEditorUiTimerGroup);
  plPlugin::Events().AddEventHandler(PluginEventHandler);
}

PL_PLUGIN_ON_UNLOADED()
{
  plPlugin::Events().RemoveEventHandler(PluginEventHandler);
  PL_DEFAULT_DELETE(s_pDeferredLoad);

  plModelingPanelManager::Shutdown();
  plBrushPropertyStates::Shutdown();
  plBrushActions::UnregisterActions();

  UnregisterIconFolder();
}

PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_EditorPluginModeling);
