#pragma once

#include <EditorFramework/EditTools/EditTool.h>
#include <EditorFramework/Gizmos/DrawBoxGizmo.h>
#include <EditorPluginModeling/Brush/BrushToolContext.h>
#include <EditorPluginModeling/EditorPluginModelingDLL.h>

struct plGameObjectEvent;
struct plManipulatorManagerEvent;

/// \brief Tool-side state the Modeling panel writes and the create tool reads.
struct PL_EDITORPLUGINMODELING_DLL plBrushToolSettings
{
  /// The plBrushShape a newly drawn brush starts with (the panel's palette selection).
  static plInt32 s_iPendingShape;
};

/// \brief The Brush create tool: drag out a box on the grid (same interaction as grey boxing) and a
/// plBrushComponent is added to the scene, picking up the material selected in the asset browser.
class PL_EDITORPLUGINMODELING_DLL plBrushEditTool : public plGameObjectEditTool
{
  PL_ADD_DYNAMIC_REFLECTION(plBrushEditTool, plGameObjectEditTool);

public:
  plBrushEditTool();
  ~plBrushEditTool();

  virtual plEditorInputContext* GetEditorInputContextOverride() override;
  virtual plEditToolSupportedSpaces GetSupportedSpaces() const override;
  virtual bool GetSupportsMoveParentOnly() const override;
  virtual void GetGridSettings(plGridSettingsMsgToEngine& out_gridSettings) override;

protected:
  virtual void OnConfigured() override;
  virtual void OnActiveChanged(bool bIsActive) override;

private:
  void UpdateGizmoState();
  void GameObjectEventHandler(const plGameObjectEvent& e);
  void ManipulatorManagerEventHandler(const plManipulatorManagerEvent& e);
  void GizmoEventHandler(const plGizmoEvent& e);

  plDrawBoxGizmo m_DrawBoxGizmo;
  plBrushToolContext m_ToolContext; ///< all brush input; forwards to the draw gizmo in Object mode
};
