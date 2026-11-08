#pragma once

#include <EditorFramework/Gizmos/GizmoBase.h>
#include <EditorFramework/Gizmos/RotateGizmo.h>
#include <EditorFramework/Gizmos/ScaleGizmo.h>
#include <EditorFramework/Gizmos/TranslateGizmo.h>
#include <EditorPluginModeling/EditorPluginModelingDLL.h>
#include <Foundation/Containers/HashSet.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Types/Status.h>
#include <Foundation/Types/UniquePtr.h>
#include <ModelingPlugin/Modeling/EditableMesh.h>

class plDrawBoxGizmo;
class plGameObjectDocument;
struct plObjectPickingResult;
struct plDocumentObjectPropertyEvent;
struct plDocumentObjectStructureEvent;

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

/// \brief What a click selects in Face mode.
struct plBrushFaceGranularity
{
  enum Enum
  {
    Surface, ///< "CSG face": a whole face as rendered, all its CSG pieces and cut faces included
    Polygon, ///< "Mesh face": one polygon of the brush's own mesh
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

/// \brief Broadcast when the sub-object selection or the target brush's mesh changes, so the Modeling
/// panel can enable operations and show the selected faces' texture settings.
struct plBrushSelectionEvent
{
  const plBrushToolContext* m_pContext = nullptr;
  const plGameObjectDocument* m_pDocument = nullptr;
};

/// \brief Selection helpers on the current sub-object selection.
struct plBrushSelectionOp
{
  enum Enum
  {
    All,
    None,
    Invert,
    Grow,
    Shrink,
    Loop,         ///< edge mode: the loops through the selected edges
    Ring,         ///< edge mode: the rings through the selected edges
    SameMaterial, ///< face mode: every face sharing a selected face's material
  };
};

/// \brief Polygon operations on the current sub-object selection.
struct plBrushMeshOp
{
  enum Enum
  {
    Extrude,  ///< modal: drag sets the distance
    Inset,    ///< modal
    Bevel,    ///< modal, wheel or +/- for segments
    LoopCut,  ///< modal slide, wheel or +/- for the number of cuts
    Bridge,
    Weld,
    Flip,
    Subdivide,
    Detach,
    Delete,
    FillHole,
    Connect,
    Triangulate,
    Flatten,
  };
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
  void SetDocument(plGameObjectDocument* pDocument);
  plGameObjectDocument* GetDocument() const { return m_pDocument; }

  plBrushSubMode::Enum GetSubMode() const { return m_SubMode; }
  void SetSubMode(plBrushSubMode::Enum mode);

  /// \brief Whether Face mode selects whole faces as rendered (CSG faces) or single polygons (mesh faces).
  /// Selecting a mesh face that shares its id with other CSG pieces gives it its own id first (one undo
  /// step), since everything face-related works by id. Broadcasts s_ModeEvents.
  void SetFaceGranularity(plBrushFaceGranularity::Enum granularity);
  plBrushFaceGranularity::Enum GetFaceGranularity() const { return m_FaceGranularity; }

  static plEvent<const plBrushModeEvent&> s_ModeEvents;
  static plEvent<const plBrushSelectionEvent&> s_SelectionEvents;

  /// \brief The brush component being edited in a sub-object mode, or nullptr.
  const plDocumentObject* GetTargetBrush() const { return GetBrushObject(); }

  const plHashSet<plUInt32>& GetSelectedVertices() const { return m_SelectedVertices; }
  const plHashSet<plUInt64>& GetSelectedEdges() const { return m_SelectedEdges; }
  const plHashSet<plUInt32>& GetSelectedFaces() const { return m_SelectedFaces; }

  /// \brief The target brush's authored mesh, as the selection indices refer to it.
  const plEditableMesh& GetBaseMesh() const { return m_BaseMesh; }

  bool IsModalOperationActive() const { return m_ModalOp != ModalOp::None; }

  void ApplySelectionOp(plBrushSelectionOp::Enum op);

  /// \brief Replaces the sub-object selection (vertex indices, edge keys, face ids of the base mesh).
  void SetSelection(const plHashSet<plUInt32>& vertices, const plHashSet<plUInt64>& edges, const plHashSet<plUInt32>& faces);

  static plUInt64 MakeEdgeKey(plUInt32 uiV0, plUInt32 uiV1);

  /// \brief Runs a polygon operation on the selection. Extrude, inset, bevel and loop cut start a modal
  /// drag; the rest apply at once as one undo step.
  plStatus RunMeshOp(plBrushMeshOp::Enum op);

  /// \brief Starts the modal move (G) or scale (S) on the selection.
  plStatus RunModalTransform(bool bScale);

  /// \brief Starts the clip tool on the selected brushes: click three points to define the plane, Tab
  /// cycles which side is kept, Enter applies, Esc cancels.
  plStatus StartClip();

  /// \brief Whether a key belongs to the Brush tool in its current state: it then runs the tool's command instead of
  /// an editor-wide shortcut (1, G, W... mean something else outside the tool).
  bool WantsKey(const plEditorInputEvent& e) const;

  /// \brief Select-through: marquee selection also takes elements facing away from the camera.
  /// Broadcasts s_ModeEvents, like SetShowHiddenHandles, so UI toggles follow key presses.
  void SetSelectThrough(bool b);
  bool GetSelectThrough() const { return m_bSelectThrough; }

  /// \brief Element handles (the vertex / edge / face boxes) show through the brush when set; otherwise
  /// handles behind the brush's own rendered surface are hidden, and so not pickable.
  void SetShowHiddenHandles(bool b);
  bool GetShowHiddenHandles() const { return m_bShowHiddenHandles; }

  /// \brief True while a drag or modal operation writes preview meshes to the document. Listeners can
  /// skip expensive refreshes until it ends.
  bool IsPreviewing() const { return m_bInTemporaryCommands; }

  /// \brief Re-tests handle occlusion if the camera moved since the last test. Polled by the view's mode
  /// strip, since camera navigation does not go through this context.
  void UpdateForCamera();

  /// \brief Edit Pivot: a move gizmo on the brush's origin (the sub-object target, else the first selected
  /// brush). Dragging it moves only the origin; the geometry stays where it is. Ctrl snaps like any move.
  plStatus SetPivotEdit(bool bEnable);
  bool IsPivotEditActive() const { return m_bPivotEdit; }

  /// \brief Moves the edited brush's origin onto the center of the sub-object selection.
  plStatus MovePivotToSelection();

  /// \brief Opens the marking menu at the cursor, or at the centre of a view when the cursor is not over one (the
  /// toolbar button).
  void OpenMarkingMenu();

  /// \brief Shown in the view's info line while drawing or editing: sizes, edge lengths, op amounts.
  plStringView GetInfoText() const { return m_sInfoText; }

  virtual void UpdateStatusBarText(plEngineDocumentWindow* pWindow) override;

protected:
  virtual void OnSetOwner(plEngineDocumentWindow* pOwnerWindow, plEngineViewWidget* pOwnerView) override;
  virtual void OnVisibleChanged(bool bVisible) override {}
  virtual void OnTransformationChanged(const plTransform& transform) override {}
  virtual void DoFocusLost(bool bCancel) override;

  virtual plEditorInput DoKeyPressEvent(const plEditorInputEvent& e) override;
  virtual plEditorInput DoKeyReleaseEvent(const plEditorInputEvent& e) override;
  virtual plEditorInput DoMousePressEvent(const plEditorInputEvent& e) override;
  virtual plEditorInput DoMouseReleaseEvent(const plEditorInputEvent& e) override;
  virtual plEditorInput DoMouseMoveEvent(const plEditorInputEvent& e) override;
  virtual plEditorInput DoWheelEvent(const plEditorInputEvent& e) override;

private:
  struct ModalOp
  {
    enum Enum
    {
      None,
      Move,
      Rotate,
      Scale,
      Extrude,
      Inset,
      Bevel,
      LoopCut,
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

  // document changes made behind the context's back (undo/redo, Properties panel edits, moving the
  // owner object) must refresh the cached mesh, the cubes and the gizmo
  void PropertyEventHandler(const plDocumentObjectPropertyEvent& e);
  void StructureEventHandler(const plDocumentObjectStructureEvent& e);
  void DocumentManagerEventHandler(const plDocumentManager::Event& e);
  void RefreshFromDocument();

  // picking & selection
  bool PickAt(const plVec2I32& vViewPos, HoverHit& out_hit);
  void ApplyClickSelection(const HoverHit& hit, bool bAdditive);
  void GatherSelectedVertices(plHashSet<plUInt32>& out_vertices) const;

  /// Plain: the ids become the face selection. bAdditive toggles them as one unit (removed when all are
  /// selected already, else added). bSubtract removes them.
  void SelectFaceIds(const plHashSet<plUInt32>& ids, bool bAdditive, bool bSubtract);

  /// The ids of the listed authored polygons, separated first where CSG pieces share an id.
  void GetPolygonIds(plArrayPtr<const plUInt32> faceIndices, plHashSet<plUInt32>& out_ids);

  // CSG faces: the brush as rendered (cut, attributes applied) grouped into surfaces
  bool UsesSurfaces() const;
  const plEditableMesh& GetSurfaceMesh() const;
  void UpdateSurfaces() const;
  void GatherSurfaceIds(plUInt32 uiSurface, plHashSet<plUInt32>& out_ids) const;

  struct ActiveGizmo
  {
    enum Enum
    {
      None,
      Translate,
      Rotate,
      Scale,
    };
  };

  // modal operation
  void BeginModalOp(ModalOp::Enum op);
  bool HasViewForModalOp() const;
  void UpdateModalPreview();
  void CommitModalOp();
  void CancelModalOp();
  void EndModalCommon();
  void WritePreview(const plEditableMesh& mesh, bool bFinal);
  plResult PickOnWorkPlane(const plVec2I32& vViewPos, plVec3& out_vPoint) const;

  /// The cursor in the owner view, in logical units from its top-left; false without an owner view.
  bool GetCursorInView(plVec2I32& out_vViewPos) const;
  plVec3 GetAxisVector() const;

  /// Rebuilds the preview from the base mesh: rotate/scale around m_vPivot, then translate — in
  /// world space. Shared by the modal ops and the gizmo drags.
  void ApplyWorldTransformToSelection(const plVec3& vTranslate, const plQuat& qRotation, const plVec3& vScale);

  // gizmo workflow (W / E / R)
  void ShowGizmo(ActiveGizmo::Enum which);
  void HideGizmos();
  void RepositionGizmos();
  plGizmo* GetActiveGizmoPtr();
  void GizmoEventHandler(const plGizmoEvent& e);
  plResult ComputeSelectionPivot(plVec3& out_vPivot);

  /// The shown gizmo's handle under the cursor (picked earlier, as res), or nullptr.
  plGizmoHandle* FindActiveGizmoHandle(const plObjectPickingResult& res);
  bool PressActiveGizmoHandle(const plEditorInputEvent& e, const plObjectPickingResult& res, plEditorInput& out_result);
  void HighlightHandle(const plUuid& handle);

  // pivot editing
  const plDocumentObject* FindPivotBrush() const;
  void UpdatePivotGizmo();
  void PivotGizmoEventHandler(const plGizmoEvent& e);

  // snapping: Ctrl held during a move snaps to the nearest brush vertex near the cursor, else the grid
  static bool IsSnapHeld();
  static plVec3 SnapToGrid(const plVec3& vWorld);

  /// With Snap To Grid on, the move becomes one that puts the low corner of the moving vertices on a grid
  /// point: geometry on the grid stays on it, geometry off it lands on it. Restricted to vAxis if set.
  plVec3 AlignMoveToGrid(const plVec3& vDelta, const plVec3& vAxis) const;
  void GatherSnapVertices(bool bExcludeMovingVertices);
  plVec3 SnapGizmoTranslation(const plVec3& vTranslate);

  // mesh ops, selection ops, marquee, snapping
  void BeginMeshModalOp(ModalOp::Enum op);
  void UpdateMeshModalPreview(float fAmount);
  plStatus ApplyImmediateMeshOp(plBrushMeshOp::Enum op);
  void NotifySelectionChanged();
  void UpdateMarquee(const plVec2I32& vCurrentPos);
  void FinishMarquee(const plVec2I32& vCurrentPos, bool bAdditive, bool bSubtract);
  bool IsElementFacingCamera(const plEditableMesh& mesh, plUInt32 uiElement) const;
  bool ProjectToView(const plVec3& vWorld, plVec2& out_vScreen) const;
  /// The snap vertex nearest to the cursor within fMaxPixels on screen.
  bool FindSnapVertex(const plVec2I32& vViewPos, plVec3& out_vWorld, float fMaxPixels = 30.0f) const;
  bool HandleActionShortcut(const plEditorInputEvent& e);

  /// Keys while a modal operation runs: Esc, Enter, axis locks, snapping, segments and typed amounts. It takes every key.
  plEditorInput HandleModalKey(const plEditorInputEvent& e);

  // overlays
  /// The mesh the overlays show: the preview while a drag or modal operation runs, else the authored mesh.
  const plEditableMesh& GetDisplayMesh() const { return (m_ModalOp != ModalOp::None || m_bInTemporaryCommands) ? m_PreviewMesh : m_BaseMesh; }
  void UpdateOverlays();
  void ClearOverlays();
  /// The outline of the faces carrying the ids, without the edges between them.
  static void AppendSelectionBorder(plDynamicArray<plVec3>& ref_lines, const plEditableMesh& mesh, const plHashSet<plUInt32>& faceIds);
  static void AppendFaceWireframe(plDynamicArray<plVec3>& ref_lines, const plEditableMesh& mesh, plUInt32 uiFace);

  // element cube handles — the greybox-manipulator visual: solid constant-size cubes on every
  // selectable element (vertex / edge midpoint / face center), grey when idle, brand when selected,
  // engine-highlighted on hover, and pickable so a click on a cube selects its element exactly
  void UpdateElementHandles();
  void HideElementHandles();
  void BuildEdgeList(const plEditableMesh& mesh, plDynamicArray<plUInt32>& out_edgePairs) const;
  bool MakeHoverFromElement(plUInt32 uiElement, HoverHit& out_hit) const;
  plInt32 FindFaceUsingVertex(const plEditableMesh& mesh, plUInt32 uiVertex) const;

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

  // modal mesh operations
  plVec3 m_vOpAxis = plVec3::MakeZero(); ///< world axis the amount is measured along (extrude)
  float m_fOpAmount = 0.0f;
  plUInt32 m_uiOpSegments = 1;         ///< bevel segments / loop cut count
  plUInt32 m_uiLoopEdgeV0 = plInvalidIndex;
  plUInt32 m_uiLoopEdgeV1 = plInvalidIndex;

  // click / marquee
  bool m_bPressPending = false;
  bool m_bMarqueeActive = false;
  plVec2I32 m_vPressPos = plVec2I32(0, 0);
  plUuid m_PressHandle;          ///< element cube under the cursor at press time
  plVec3 m_vPressSurface;        ///< surface point under the cursor at press time
  bool m_bPressHitBrush = false;
  plEngineGizmoHandle m_hMarquee;
  bool m_bSelectThrough = false;
  bool m_bShowHiddenHandles = false;
  plVec3 m_vHandleCameraPos = plVec3::MakeZero(); ///< camera the handle occlusion was last tested from
  plVec3 m_vHandleCameraDir = plVec3::MakeZero();
  bool m_bVertexSnap = false;    ///< V held during a move
  plDynamicArray<plVec3> m_SnapVertices; ///< world positions a move can snap to, gathered at its start
  const char* m_szSnapInfo = nullptr;    ///< what the last snapped move landed on, for the info line
  bool m_bCarved = false;        ///< live cutters change the rendered mesh: show the authored cage
  plEditableMesh m_CarvedMesh;   ///< the rendered (cut) mesh while m_bCarved, brush-local

  plBrushFaceGranularity::Enum m_FaceGranularity = plBrushFaceGranularity::Surface;
  mutable bool m_bSurfacesDirty = true;
  mutable plEditableMesh m_SurfaceMesh;                     ///< rendered mesh, brush-local, attributes applied
  mutable plDynamicArray<plUInt32> m_SurfaceOfFace;         ///< per m_SurfaceMesh face
  mutable plDynamicArray<plUInt32> m_SurfaceRepresentative; ///< per surface: its largest face, where its handle sits

  plStringBuilder m_sInfoText;

  // clip tool
  bool m_bClipActive = false;
  plHybridArray<plVec3, 3> m_ClipPoints; ///< world space
  plInt32 m_iClipKeep = 0;               ///< plBrushCommands::ClipKeep
  plEngineGizmoHandle m_hClipLines;
  void UpdateClipPreview();
  void FinishClip(bool bApply);

  plEngineGizmoHandle m_hHoverLines;
  plEngineGizmoHandle m_hSelectionLines;
  plEngineGizmoHandle m_hFaceWireLines;  ///< triangle wireframe of the selected faces, over their accent
  plEngineGizmoHandle m_hCandidateLines; ///< the brush wireframe — structural context, dim

  plDynamicArray<plUniquePtr<plEngineGizmoHandle>> m_ElementHandles; ///< cube pool; index = element index
  plHashTable<plUuid, plUInt32> m_HandleToElement;                   ///< picked sync-object guid -> element
  plDynamicArray<plUInt32> m_CurrentEdges;                           ///< edge mode: (v0,v1) pairs, element order

  // gizmo state
  ActiveGizmo::Enum m_ActiveGizmo = ActiveGizmo::None;
  plTranslateGizmo m_TranslateGizmo;
  plRotateGizmo m_RotateGizmo;
  plScaleGizmo m_ScaleGizmo;
  plTransform m_GizmoStartTransform = plTransform::MakeIdentity();
  plUuid m_LastHighlightedHandle;

  // pivot editing: m_TranslateGizmo sits on the brush origin instead of the sub-object selection
  bool m_bPivotEdit = false;
  bool m_bPivotDragging = false;
  plUuid m_PivotBrush; ///< brush component whose origin the gizmo shows
};
