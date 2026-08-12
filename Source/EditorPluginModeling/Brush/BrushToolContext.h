#pragma once

#include <EditorFramework/Gizmos/GizmoBase.h>
#include <EditorPluginModeling/EditorPluginModelingDLL.h>
#include <Foundation/Containers/HashSet.h>
#include <ModelingPlugin/Modeling/EditableMesh.h>

class plDrawBoxGizmo;
class plGameObjectDocument;

struct plBrushSubMode
{
  enum Enum
  {
    Object, ///< the draw tool: clicks go to the DrawBoxGizmo, sub-object state is dormant
    Vertex,
    Edge,
    Face,
  };
};

class plBrushToolContext;

/// \brief Broadcast whenever a brush context changes its sub-object mode, so UI (the Modeling panel's
/// mode buttons) can mirror keyboard-driven changes. Static because contexts are created lazily with
/// their edit tool, after any panel exists.
struct plBrushModeEvent
{
  const plBrushToolContext* m_pContext = nullptr;
  const plGameObjectDocument* m_pDocument = nullptr;
  plBrushSubMode::Enum m_Mode = plBrushSubMode::Object;
};

/// \brief The single input context for all brush editing while the Brush tool is active.
///
/// In Object mode every event is forwarded to the edit tool's plDrawBoxGizmo, so drawing behaves as in
/// P1. Keys 1/2/3 enter Vertex/Edge/Face mode on the selected brush: hover and click-select run against
/// a CPU-rebuilt mesh (GPU picking only supplies the surface point), and G/R/S start Blender-style modal
/// operations — X/Y/Z axis lock, typed numeric values, Enter/click commits, Esc cancels.
///
/// The first geometry edit converts the brush to Shape=Editable and writes the StoredMesh blob. The
/// modal preview runs through the temporary-command protocol (one undo step per operation, Escape
/// restores) — the plan's delta preview channel is deferred until mesh sizes demand it.
class PL_EDITORPLUGINMODELING_DLL plBrushToolContext : public plGizmo
{
  PL_ADD_DYNAMIC_REFLECTION(plBrushToolContext, plGizmo);

public:
  plBrushToolContext();
  ~plBrushToolContext();

  void SetDrawGizmo(plDrawBoxGizmo* pGizmo) { m_pDrawGizmo = pGizmo; }
  void SetDocument(plGameObjectDocument* pDocument) { m_pDocument = pDocument; }
  plGameObjectDocument* GetDocument() const { return m_pDocument; }

  plBrushSubMode::Enum GetSubMode() const { return m_SubMode; }
  void SetSubMode(plBrushSubMode::Enum mode);

  static plEvent<const plBrushModeEvent&> s_ModeEvents;

  virtual void UpdateStatusBarText(plQtEngineDocumentWindow* pWindow) override;

protected:
  virtual void OnSetOwner(plQtEngineDocumentWindow* pOwnerWindow, plQtEngineViewWidget* pOwnerView) override;
  virtual void OnVisibleChanged(bool bVisible) override {}
  virtual void OnTransformationChanged(const plTransform& transform) override {}
  virtual void DoFocusLost(bool bCancel) override;

  virtual plEditorInput DoKeyPressEvent(QKeyEvent* e) override;
  virtual plEditorInput DoKeyReleaseEvent(QKeyEvent* e) override;
  virtual plEditorInput DoMousePressEvent(QMouseEvent* e) override;
  virtual plEditorInput DoMouseReleaseEvent(QMouseEvent* e) override;
  virtual plEditorInput DoMouseMoveEvent(QMouseEvent* e) override;

private:
  struct ModalOp
  {
    enum Enum
    {
      None,
      Move,
      Rotate,
      Scale,
    };
  };

  struct AxisLock
  {
    enum Enum
    {
      None,
      X,
      Y,
      Z,
    };
  };

  /// What the cursor is over, resolved per sub-object mode.
  struct HoverHit
  {
    plInt32 m_iFace = -1; ///< face INDEX in the rebuilt mesh (all modes hit a face first)
    plInt32 m_iVertex = -1;
    plInt32 m_iEdgeV0 = -1;
    plInt32 m_iEdgeV1 = -1;

    bool IsValid() const { return m_iFace >= 0; }
    void Reset() { *this = HoverHit(); }
  };

  // target brush
  bool AcquireTargetBrush();
  bool RebuildBaseMesh();
  const plDocumentObject* GetBrushObject() const;

  // picking & selection
  bool PickAt(const QPoint& viewPos, HoverHit& out_hit);
  void ApplyClickSelection(const HoverHit& hit, bool bAdditive);
  void GatherSelectedVertices(plHashSet<plUInt32>& out_vertices) const;
  static plUInt64 MakeEdgeKey(plUInt32 uiV0, plUInt32 uiV1);

  // modal operation
  void BeginModalOp(ModalOp::Enum op);
  void UpdateModalPreview();
  void CommitModalOp();
  void CancelModalOp();
  void EndModalCommon();
  void WritePreview(const plEditableMesh& mesh, bool bFinal);
  plResult PickOnWorkPlane(const QPoint& viewPos, plVec3& out_vPoint) const;
  plVec3 GetAxisVector() const;

  // overlays
  void UpdateOverlays();
  void ClearOverlays();
  void AppendVertexBox(plDynamicArray<plVec3>& ref_lines, const plVec3& vPos, float fSize) const;
  void AppendVertexBoxFilled(plDynamicArray<plVec3>& ref_lines, const plVec3& vPos, float fSize) const;
  void AppendEdgeBar(plDynamicArray<plVec3>& ref_lines, const plVec3& v0, const plVec3& v1, float fRadius) const;
  void AppendFaceOutline(plDynamicArray<plVec3>& ref_lines, plUInt32 uiFace) const;
  void AppendFaceInset(plDynamicArray<plVec3>& ref_lines, plUInt32 uiFace, float fInset) const;
  void AppendWireframe(plDynamicArray<plVec3>& ref_lines) const;
  float GetMarkerSize() const;

  plDrawBoxGizmo* m_pDrawGizmo = nullptr;
  plGameObjectDocument* m_pDocument = nullptr;

  plBrushSubMode::Enum m_SubMode = plBrushSubMode::Object;
  plBrushSubMode::Enum m_LastSubMode = plBrushSubMode::Vertex;

  plUuid m_BrushComponent;
  plUuid m_BrushOwnerObject;
  plEditableMesh m_BaseMesh;
  plTransform m_ObjectTransform = plTransform::MakeIdentity();

  HoverHit m_Hover;
  plHashSet<plUInt32> m_SelectedVertices;
  plHashSet<plUInt64> m_SelectedEdges;
  plHashSet<plUInt32> m_SelectedFaces; ///< face ids, not indices — they survive re-generation

  // modal state
  ModalOp::Enum m_ModalOp = ModalOp::None;
  AxisLock::Enum m_AxisLock = AxisLock::None;
  plStringBuilder m_sNumericInput;
  plHashSet<plUInt32> m_ModalVertices;
  plVec3 m_vPivot = plVec3::MakeZero();      ///< world space
  plVec3 m_vStartPoint = plVec3::MakeZero(); ///< world point on the work plane at op start
  plVec3 m_vCurrentPoint = plVec3::MakeZero();
  plVec2I32 m_vLastMouse = plVec2I32(0, 0);
  plEditableMesh m_PreviewMesh;
  bool m_bInTemporaryCommands = false;

  plEngineGizmoHandle m_hHoverLines;
  plEngineGizmoHandle m_hSelectionLines;
  plEngineGizmoHandle m_hCandidateLines; ///< what CAN be selected: wireframe + per-mode markers, dim
};
