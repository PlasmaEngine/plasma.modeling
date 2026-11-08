#pragma once

#include <EditorFramework/EditTools/EditTool.h>
#include <EditorFramework/Gizmos/DrawBoxGizmo.h>
#include <EditorPluginModeling/Brush/BrushToolContext.h>
#include <EditorPluginModeling/EditorPluginModelingDLL.h>
#include <Foundation/Types/UniquePtr.h>

class plEngineBrushModeStrip;

struct plGameObjectEvent;
struct plManipulatorManagerEvent;

/// \brief Tool-side state the Modeling panel writes and the create tool reads.
struct PL_EDITORPLUGINMODELING_DLL plBrushToolSettings
{
  /// The plBrushShape a newly drawn brush starts with (the panel's palette selection).
  static plInt32 s_iPendingShape;

  /// \brief Whether drawing, moving, extruding and clipping land on the grid. On by default.
  static bool GetSnapToGrid() { return s_bSnapToGrid; }
  static void SetSnapToGrid(bool bSnap);

  /// \brief The grid step: the editor's translation snap, or 0.25 m while the editor's snapping is off.
  static float GetGridStep();

  /// \brief The point moved onto the nearest grid point.
  static plVec3 SnapToGrid(const plVec3& vWorld);

  /// \brief Broadcast with the new state when snapping is switched.
  static plEvent<bool> s_SnapToGridEvents;

private:
  static bool s_bSnapToGrid;
};

/// \brief The Brush create tool: drag out a box on the grid (same interaction as grey boxing) and a
/// plBrushComponent is added to the scene, picking up the material selected in the asset browser. With a
/// brush group (or anything inside one) selected, the new brush goes into that group.
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

  plBrushToolContext& GetToolContext() { return m_ToolContext; }

protected:
  virtual void OnConfigured() override;
  virtual void OnActiveChanged(bool bIsActive) override;

private:
  void UpdateGizmoState();
  void GameObjectEventHandler(const plGameObjectEvent& e);
  void ManipulatorManagerEventHandler(const plManipulatorManagerEvent& e);
  void GizmoEventHandler(const plGizmoEvent& e);

  /// \brief The game object a newly drawn brush is parented to: the nearest brush group at or above the
  /// selection, or nullptr for the scene root.
  const plDocumentObject* FindTargetGroup() const;

  void CreateModeStrips();
  void DestroyModeStrips();

  plDrawBoxGizmo m_DrawBoxGizmo;
  plBrushToolContext m_ToolContext; ///< all brush input; forwards to the draw gizmo in Object mode
  plHybridArray<plUniquePtr<plEngineBrushModeStrip>, 4> m_ModeStrips; ///< one mode strip per view while active
};
