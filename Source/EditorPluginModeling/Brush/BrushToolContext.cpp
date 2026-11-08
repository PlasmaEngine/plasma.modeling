#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <EditorFramework/Document/GameObjectDocument.h>
#include <Foundation/Math/ColorScheme.h>
#include <EditorFramework/DocumentWindow/EngineDocumentWindow.h>
#include <EditorFramework/DocumentWindow/EngineViewWidget.h>
#include <EditorFramework/Gizmos/DrawBoxGizmo.h>
#include <EditorFramework/Gizmos/SnapProvider.h>
#include <EditorPluginModeling/Brush/BrushActions.h>
#include <EditorPluginModeling/Brush/BrushCommands.h>
#include <EditorPluginModeling/Brush/BrushEditTool.h>
#include <EditorPluginModeling/Brush/BrushPicking.h>
#include <EditorPluginModeling/Brush/BrushToolContext.h>
#include <EditorPluginModeling/Brush/EngineBrushOverlays.h>
#include <Foundation/Math/Intersection.h>
#include <Foundation/Utilities/GraphicsUtils.h>
#include <ModelingPlugin/Modeling/BrushGenerators.h>
#include <ModelingPlugin/Modeling/MeshOps.h>
#include <ModelingPlugin/Modeling/MeshTopology.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <ToolsFoundation/Object/DocumentObjectBase.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plBrushToolContext, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

// Ctrl snaps to a vertex only this close to the cursor, so the grid stays reachable between vertices
// (holding V snaps to vertices from further away)
static constexpr float c_fCtrlSnapPixels = 15.0f;

plEvent<const plBrushModeEvent&> plBrushToolContext::s_ModeEvents;
plEvent<const plBrushSelectionEvent&> plBrushToolContext::s_SelectionEvents;

plBrushToolContext::plBrushToolContext()
{
  // overlay layers: the authored cage of a carved brush (dim), selected elements (accent), the selected
  // faces' triangles (light, over the accent), hover (white)
  m_hCandidateLines.ConfigureHandle(this, plEngineGizmoHandleType::CustomLines, plColorLinearUB(130, 135, 150), plGizmoFlags::OnTop | plGizmoFlags::ShowInOrtho);
  m_hSelectionLines.ConfigureHandle(this, plEngineGizmoHandleType::CustomLines, plColorLinearUB(6, 250, 201), plGizmoFlags::OnTop | plGizmoFlags::ShowInOrtho);
  m_hFaceWireLines.ConfigureHandle(this, plEngineGizmoHandleType::CustomLines, plColorLinearUB(220, 226, 236), plGizmoFlags::OnTop | plGizmoFlags::ShowInOrtho);
  m_hHoverLines.ConfigureHandle(this, plEngineGizmoHandleType::CustomLines, plColorLinearUB(235, 240, 250), plGizmoFlags::OnTop | plGizmoFlags::ShowInOrtho);
  m_hMarquee.ConfigureHandle(nullptr, plEngineGizmoHandleType::LineBox, plColor::LightSkyBlue, plGizmoFlags::ShowInOrtho | plGizmoFlags::OnTop);
  m_hClipLines.ConfigureHandle(this, plEngineGizmoHandleType::CustomLines, plColorLinearUB(240, 170, 60), plGizmoFlags::OnTop | plGizmoFlags::ShowInOrtho);

  m_TranslateGizmo.m_GizmoEvents.AddEventHandler(plMakeDelegate(&plBrushToolContext::GizmoEventHandler, this));
  m_RotateGizmo.m_GizmoEvents.AddEventHandler(plMakeDelegate(&plBrushToolContext::GizmoEventHandler, this));
  m_ScaleGizmo.m_GizmoEvents.AddEventHandler(plMakeDelegate(&plBrushToolContext::GizmoEventHandler, this));

  SetVisible(false);
  SetTransformation(plTransform::MakeIdentity());

  plDocumentManager::s_Events.AddEventHandler(plMakeDelegate(&plBrushToolContext::DocumentManagerEventHandler, this));
}

plBrushToolContext::~plBrushToolContext()
{
  plDocumentManager::s_Events.RemoveEventHandler(plMakeDelegate(&plBrushToolContext::DocumentManagerEventHandler, this));

  m_TranslateGizmo.m_GizmoEvents.RemoveEventHandler(plMakeDelegate(&plBrushToolContext::GizmoEventHandler, this));
  m_RotateGizmo.m_GizmoEvents.RemoveEventHandler(plMakeDelegate(&plBrushToolContext::GizmoEventHandler, this));
  m_ScaleGizmo.m_GizmoEvents.RemoveEventHandler(plMakeDelegate(&plBrushToolContext::GizmoEventHandler, this));

  SetDocument(nullptr);
}

void plBrushToolContext::DocumentManagerEventHandler(const plDocumentManager::Event& e)
{
  // A closing scene document swaps its object manager for another one before it deletes its edit tools
  // (and with them this context), so the handlers must come off while the original is still in place.
  if (e.m_Type == plDocumentManager::Event::Type::DocumentClosing && e.m_pDocument == m_pDocument)
  {
    SetDocument(nullptr);
  }
}

void plBrushToolContext::SetDocument(plGameObjectDocument* pDocument)
{
  if (m_pDocument == pDocument)
    return;

  if (m_pDocument != nullptr)
  {
    m_pDocument->GetObjectManager()->m_PropertyEvents.RemoveEventHandler(plMakeDelegate(&plBrushToolContext::PropertyEventHandler, this));
    m_pDocument->GetObjectManager()->m_StructureEvents.RemoveEventHandler(plMakeDelegate(&plBrushToolContext::StructureEventHandler, this));
  }

  m_pDocument = pDocument;

  if (m_pDocument != nullptr)
  {
    m_pDocument->GetObjectManager()->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plBrushToolContext::PropertyEventHandler, this));
    m_pDocument->GetObjectManager()->m_StructureEvents.AddEventHandler(plMakeDelegate(&plBrushToolContext::StructureEventHandler, this));
  }
}

void plBrushToolContext::PropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  if (m_SubMode == plBrushSubMode::Object)
    return;

  // our own preview writes raise these too — during a drag the operation owns the state
  if (m_bInTemporaryCommands || m_ModalOp != ModalOp::None)
    return;

  if (e.m_pObject == nullptr || !m_BrushComponent.IsValid())
    return;

  const plUuid guid = e.m_pObject->GetGuid();

  // the owner object matters as well: moving the brush moves every handle
  if (guid != m_BrushComponent && guid != m_BrushOwnerObject)
    return;

  RefreshFromDocument();
}

void plBrushToolContext::StructureEventHandler(const plDocumentObjectStructureEvent& e)
{
  if (m_SubMode == plBrushSubMode::Object || !m_BrushComponent.IsValid())
    return;

  if (e.m_EventType != plDocumentObjectStructureEvent::Type::AfterObjectRemoved)
    return;

  if (e.m_pObject == nullptr)
    return;

  const plUuid guid = e.m_pObject->GetGuid();

  if (guid == m_BrushComponent || guid == m_BrushOwnerObject)
  {
    // the target is gone (deleted, or its creation undone) — back to draw mode
    SetSubMode(plBrushSubMode::Object);
  }
}

void plBrushToolContext::RefreshFromDocument()
{
  if (!RebuildBaseMesh())
  {
    SetSubMode(plBrushSubMode::Object);
    return;
  }

  RepositionGizmos();
  UpdatePivotGizmo();
  UpdateOverlays();
  NotifySelectionChanged();
}

void plBrushToolContext::OnSetOwner(plEngineDocumentWindow* pOwnerWindow, plEngineViewWidget* pOwnerView)
{
  pOwnerWindow->GetDocument()->AddSyncObject(&m_hCandidateLines);
  pOwnerWindow->GetDocument()->AddSyncObject(&m_hHoverLines);
  pOwnerWindow->GetDocument()->AddSyncObject(&m_hSelectionLines);
  pOwnerWindow->GetDocument()->AddSyncObject(&m_hFaceWireLines);
  pOwnerWindow->GetDocument()->AddSyncObject(&m_hMarquee);
  pOwnerWindow->GetDocument()->AddSyncObject(&m_hClipLines);

  m_TranslateGizmo.SetOwner(pOwnerWindow, nullptr);
  m_RotateGizmo.SetOwner(pOwnerWindow, nullptr);
  m_ScaleGizmo.SetOwner(pOwnerWindow, nullptr);

  m_TranslateGizmo.SetVisible(false);
  m_RotateGizmo.SetVisible(false);
  m_ScaleGizmo.SetVisible(false);
}

void plBrushToolContext::DoFocusLost(bool bCancel)
{
  if (m_ModalOp != ModalOp::None)
  {
    CancelModalOp();
  }

  m_Hover.Reset();

  if (m_SubMode == plBrushSubMode::Object && m_pDrawGizmo != nullptr)
  {
    m_pDrawGizmo->FocusLost(bCancel);
  }

  UpdateOverlays();
}

//////////////////////////////////////////////////////////////////////////
// target brush

const plDocumentObject* plBrushToolContext::GetBrushObject() const
{
  if (!m_BrushComponent.IsValid() || m_pDocument == nullptr)
    return nullptr;

  return m_pDocument->GetObjectManager()->GetObject(m_BrushComponent);
}

bool plBrushToolContext::AcquireTargetBrush()
{
  m_BrushComponent = plUuid();
  m_BrushOwnerObject = plUuid();

  if (m_pDocument == nullptr)
    return false;

  const auto& selection = m_pDocument->GetSelectionManager()->GetSelection();

  for (const plDocumentObject* pObject : selection)
  {
    for (const plDocumentObject* pChild : pObject->GetChildren())
    {
      if (pChild->GetTypeAccessor().GetType()->GetTypeName() == "plBrushComponent")
      {
        m_BrushComponent = pChild->GetGuid();
        m_BrushOwnerObject = pObject->GetGuid();
        return RebuildBaseMesh();
      }
    }
  }

  return false;
}

bool plBrushToolContext::RebuildBaseMesh()
{
  const plDocumentObject* pComponent = GetBrushObject();

  if (pComponent == nullptr)
    return false;

  if (plBrushPicking::BuildMeshFromObject(pComponent, m_BaseMesh).Failed())
    return false;

  const plDocumentObject* pOwner = m_pDocument->GetObjectManager()->GetObject(m_BrushOwnerObject);

  if (pOwner == nullptr)
    return false;

  m_ObjectTransform = m_pDocument->GetGlobalTransform(pOwner);

  // parameter edits between clicks can change the vertex count — stale indices must not survive
  const plUInt32 uiVertexCount = m_BaseMesh.m_Vertices.GetCount();

  {
    plHybridArray<plUInt32, 8> stale;
    for (plUInt32 uiVertex : m_SelectedVertices)
    {
      if (uiVertex >= uiVertexCount)
        stale.PushBack(uiVertex);
    }
    for (plUInt32 uiVertex : stale)
    {
      m_SelectedVertices.Remove(uiVertex);
    }

    plHybridArray<plUInt64, 8> staleEdges;
    for (plUInt64 uiEdge : m_SelectedEdges)
    {
      if (static_cast<plUInt32>(uiEdge >> 32) >= uiVertexCount || static_cast<plUInt32>(uiEdge & 0xFFFFFFFF) >= uiVertexCount)
        staleEdges.PushBack(uiEdge);
    }
    for (plUInt64 uiEdge : staleEdges)
    {
      m_SelectedEdges.Remove(uiEdge);
    }
  }

  // with live cutters the rendered mesh differs from the authored one the handles edit
  m_bCarved = false;
  m_CarvedMesh.Clear();

  if (!pComponent->GetTypeAccessor().GetValue("Subtractive").ConvertTo<bool>() && !m_BaseMesh.m_Faces.IsEmpty())
  {
    plBoundingBox bounds = m_BaseMesh.ComputeBounds();
    bounds.TransformFromOrigin(m_ObjectTransform.GetAsMat4());

    plDynamicArray<plBrushCutterInput> cutters;
    plBrushPicking::GatherCutters(pComponent, bounds, cutters);
    m_bCarved = !cutters.IsEmpty();

    // the selected faces' wireframe follows what is rendered, which for a carved brush is the cut result
    if (m_bCarved && plBrushPicking::BuildEvaluatedMeshFromObject(pComponent, m_CarvedMesh).Failed())
    {
      m_CarvedMesh.Clear();
    }
  }

  // face ids can vanish too, e.g. when undo takes back a separation: keep only ids something still carries
  if (!m_SelectedFaces.IsEmpty())
  {
    plHashSet<plUInt32> existing;
    for (const plBrushFace& face : m_BaseMesh.m_Faces)
      existing.Insert(face.m_uiFaceId);
    for (const plBrushFace& face : m_CarvedMesh.m_Faces)
      existing.Insert(face.m_uiFaceId);

    plHybridArray<plUInt32, 8> staleFaces;
    for (plUInt32 uiId : m_SelectedFaces)
    {
      if (!existing.Contains(uiId))
        staleFaces.PushBack(uiId);
    }
    for (plUInt32 uiId : staleFaces)
    {
      m_SelectedFaces.Remove(uiId);
    }
  }

  m_bSurfacesDirty = true;
  return true;
}

//////////////////////////////////////////////////////////////////////////
// CSG faces

bool plBrushToolContext::UsesSurfaces() const
{
  // during an operation the overlays follow the preview mesh, which has no surfaces computed
  return m_SubMode == plBrushSubMode::Face && m_FaceGranularity == plBrushFaceGranularity::Surface && m_ModalOp == ModalOp::None && !m_bInTemporaryCommands;
}

const plEditableMesh& plBrushToolContext::GetSurfaceMesh() const
{
  UpdateSurfaces();
  return m_SurfaceMesh;
}

void plBrushToolContext::UpdateSurfaces() const
{
  if (!m_bSurfacesDirty)
    return;

  m_bSurfacesDirty = false;

  // what is rendered: the cut result (it already carries the face attributes), else the authored mesh
  // with them applied, so material slots split surfaces the way the viewport shows them
  if (m_bCarved && !m_CarvedMesh.m_Faces.IsEmpty())
  {
    m_SurfaceMesh = m_CarvedMesh;
  }
  else
  {
    m_SurfaceMesh = m_BaseMesh;

    if (const plDocumentObject* pBrush = GetBrushObject())
    {
      plBrushFaceAttributeMap attributes;
      plBrushPicking::ReadFaceAttributes(pBrush, attributes);
      plBrushFaceAttributeCodec::Apply(attributes, m_SurfaceMesh);
    }
  }

  const plUInt32 uiSurfaceCount = plMeshOps::ComputeSurfaces(m_SurfaceMesh, m_SurfaceOfFace);

  // each surface's handle sits on its largest piece: the centre of all pieces can lie in a hole
  m_SurfaceRepresentative.Clear();
  m_SurfaceRepresentative.SetCount(uiSurfaceCount, plInvalidIndex);

  plDynamicArray<float> largestArea;
  largestArea.SetCount(uiSurfaceCount, -1.0f);

  for (plUInt32 f = 0; f < m_SurfaceMesh.m_Faces.GetCount(); ++f)
  {
    const plUInt32 uiSurface = m_SurfaceOfFace[f];

    if (uiSurface == plInvalidIndex)
      continue;

    const float fArea = m_SurfaceMesh.ComputeFaceArea(f);

    if (fArea > largestArea[uiSurface])
    {
      largestArea[uiSurface] = fArea;
      m_SurfaceRepresentative[uiSurface] = f;
    }
  }
}

void plBrushToolContext::GatherSurfaceIds(plUInt32 uiSurface, plHashSet<plUInt32>& out_ids) const
{
  UpdateSurfaces();

  for (plUInt32 f = 0; f < m_SurfaceMesh.m_Faces.GetCount(); ++f)
  {
    if (m_SurfaceOfFace[f] == uiSurface)
      out_ids.Insert(m_SurfaceMesh.m_Faces[f].m_uiFaceId);
  }
}

void plBrushToolContext::SelectFaceIds(const plHashSet<plUInt32>& ids, bool bAdditive, bool bSubtract)
{
  bool bAllSelected = !ids.IsEmpty();

  for (plUInt32 uiId : ids)
    bAllSelected &= m_SelectedFaces.Contains(uiId);

  if (!bAdditive && !bSubtract)
    m_SelectedFaces.Clear();

  const bool bRemove = bSubtract || (bAdditive && bAllSelected);

  for (plUInt32 uiId : ids)
  {
    bRemove ? (void)m_SelectedFaces.Remove(uiId) : (void)m_SelectedFaces.Insert(uiId);
  }
}

void plBrushToolContext::GetPolygonIds(plArrayPtr<const plUInt32> faceIndices, plHashSet<plUInt32>& out_ids)
{
  plDynamicArray<plUInt32> ids;
  const plStatus result = plBrushCommands::SeparateFaceIds(m_pDocument, GetBrushObject(), faceIndices, ids);

  if (result.Failed())
  {
    if (GetOwnerWindow())
      GetOwnerWindow()->ShowTemporaryStatusBarMsg(plFmt("{}", result.m_sMessage), plTime::MakeFromSeconds(6));
    return;
  }

  for (plUInt32 uiId : ids)
    out_ids.Insert(uiId);

  // a separation rewrote the mesh; the document events already refreshed it unless a drag is running
  RebuildBaseMesh();
}

void plBrushToolContext::SetFaceGranularity(plBrushFaceGranularity::Enum granularity)
{
  if (m_FaceGranularity == granularity)
    return;

  m_FaceGranularity = granularity;

  // the ids stay selected: a CSG face is the set of its pieces' ids, so both views show the same faces
  UpdateOverlays();
  NotifySelectionChanged();

  if (GetOwnerWindow())
    UpdateStatusBarText(GetOwnerWindow());

  plBrushModeEvent e;
  e.m_pContext = this;
  e.m_pDocument = m_pDocument;
  e.m_Mode = m_SubMode;
  s_ModeEvents.Broadcast(e);
}

void plBrushToolContext::NotifySelectionChanged()
{
  // info line: what the selection measures
  m_sInfoText.Clear();

  if (m_ModalOp == ModalOp::None && m_SubMode == plBrushSubMode::Edge && m_SelectedEdges.GetCount() == 1)
  {
    for (plUInt64 uiEdge : m_SelectedEdges)
    {
      const plUInt32 v0 = static_cast<plUInt32>(uiEdge >> 32);
      const plUInt32 v1 = static_cast<plUInt32>(uiEdge & 0xFFFFFFFF);

      if (v0 < m_BaseMesh.m_Vertices.GetCount() && v1 < m_BaseMesh.m_Vertices.GetCount())
      {
        const float fLength = (m_ObjectTransform.TransformPosition(m_BaseMesh.m_Vertices[v0]) - m_ObjectTransform.TransformPosition(m_BaseMesh.m_Vertices[v1])).GetLength();
        m_sInfoText.SetFormat("Edge {} m", plArgF(fLength, 3));
      }
    }
  }
  else if (m_ModalOp == ModalOp::None && m_SubMode == plBrushSubMode::Face && !m_SelectedFaces.IsEmpty())
  {
    // how many faces of the current kind are selected, and their area as rendered or authored
    const bool bSurfaces = UsesSurfaces();
    const plEditableMesh& faceMesh = bSurfaces ? GetSurfaceMesh() : m_BaseMesh;

    float fArea = 0.0f;
    plHashSet<plUInt32> counted;

    for (plUInt32 uiFace = 0; uiFace < faceMesh.m_Faces.GetCount(); ++uiFace)
    {
      if (!m_SelectedFaces.Contains(faceMesh.m_Faces[uiFace].m_uiFaceId))
        continue;

      fArea += faceMesh.ComputeFaceArea(uiFace);
      counted.Insert(bSurfaces ? m_SurfaceOfFace[uiFace] : uiFace);
    }

    const float fScale = m_ObjectTransform.m_vScale.x * m_ObjectTransform.m_vScale.y;
    m_sInfoText.SetFormat("{} {} face{}  {} m\xC2\xB2", counted.GetCount(), bSurfaces ? "CSG" : "mesh", counted.GetCount() == 1 ? "" : "s", plArgF(fArea * fScale, 3));
  }

  if (m_bCarved && m_SubMode != plBrushSubMode::Object && m_sInfoText.IsEmpty())
  {
    m_sInfoText = "Editing the uncut shape: live cutters apply on top";
  }

  plBrushSelectionEvent e;
  e.m_pContext = this;
  e.m_pDocument = m_pDocument;
  s_SelectionEvents.Broadcast(e);
}

//////////////////////////////////////////////////////////////////////////
// picking & selection

plUInt64 plBrushToolContext::MakeEdgeKey(plUInt32 uiV0, plUInt32 uiV1)
{
  const plUInt32 uiLow = plMath::Min(uiV0, uiV1);
  const plUInt32 uiHigh = plMath::Max(uiV0, uiV1);
  return (static_cast<plUInt64>(uiHigh) << 32) | uiLow;
}

bool plBrushToolContext::PickAt(const plVec2I32& vViewPos, HoverHit& out_hit)
{
  out_hit.Reset();

  if (GetOwnerView() == nullptr)
    return false;

  // the GPU pick supplies the surface point (one frame latent — fine for hover); everything
  // sub-object is resolved CPU-side against the rebuilt mesh
  const plObjectPickingResult& res = GetOwnerView()->PickObject(static_cast<plUInt16>(vViewPos.x), static_cast<plUInt16>(vViewPos.y));

  if (!res.m_PickedComponent.IsValid() || res.m_PickedComponent != m_BrushComponent)
    return false;

  if (!RebuildBaseMesh())
    return false;

  const plVec3 vLocalPos = m_ObjectTransform.GetInverse().TransformPosition(res.m_vPickedPosition);

  if (m_SubMode == plBrushSubMode::Face)
  {
    // the click lands on the rendered surface, which on a carved brush includes cut faces the authored
    // mesh does not have: resolve it there first
    const plEditableMesh& surface = GetSurfaceMesh();
    const plInt32 iSurfaceFace = plBrushPicking::PickFaceIndexAtLocalPos(surface, vLocalPos);

    if (iSurfaceFace < 0)
      return false;

    if (UsesSurfaces())
    {
      out_hit.m_iFace = iSurfaceFace; // an index into the surface mesh
      return true;
    }

    // mesh faces: a cut face has no polygon of its own to select
    const plUInt32 uiSurfaceId = surface.m_Faces[iSurfaceFace].m_uiFaceId;
    bool bAuthored = false;

    for (const plBrushFace& face : m_BaseMesh.m_Faces)
      bAuthored |= face.m_uiFaceId == uiSurfaceId;

    if (!bAuthored)
    {
      if (GetOwnerWindow())
        GetOwnerWindow()->ShowTemporaryStatusBarMsg("That face is cut by a cutter and has no polygon of its own - switch Face mode to CSG faces to select it.", plTime::MakeFromSeconds(6));

      return false;
    }
  }

  const plInt32 iFace = plBrushPicking::PickFaceIndexAtLocalPos(m_BaseMesh, vLocalPos);

  if (iFace < 0)
    return false;

  out_hit.m_iFace = iFace;

  const plBrushFace& face = m_BaseMesh.m_Faces[iFace];

  if (m_SubMode == plBrushSubMode::Vertex)
  {
    float fBestDist = plMath::MaxValue<float>();

    for (plUInt32 uiCorner : face.m_Corners)
    {
      const float fDist = (m_BaseMesh.m_Vertices[uiCorner] - vLocalPos).GetLengthSquared();

      if (fDist < fBestDist)
      {
        fBestDist = fDist;
        out_hit.m_iVertex = static_cast<plInt32>(uiCorner);
      }
    }
  }
  else if (m_SubMode == plBrushSubMode::Edge)
  {
    float fBestDist = plMath::MaxValue<float>();

    for (plUInt32 i = 0; i < face.m_Corners.GetCount(); ++i)
    {
      const plUInt32 uiV0 = face.m_Corners[i];
      const plUInt32 uiV1 = face.m_Corners[(i + 1) % face.m_Corners.GetCount()];

      const plVec3& vStart = m_BaseMesh.m_Vertices[uiV0];
      const plVec3 vEdge = m_BaseMesh.m_Vertices[uiV1] - vStart;

      float fT = 0.0f;
      const float fEdgeLenSq = vEdge.GetLengthSquared();

      if (fEdgeLenSq > 0.0f)
      {
        fT = plMath::Clamp((vLocalPos - vStart).Dot(vEdge) / fEdgeLenSq, 0.0f, 1.0f);
      }

      const float fDist = (vStart + vEdge * fT - vLocalPos).GetLengthSquared();

      if (fDist < fBestDist)
      {
        fBestDist = fDist;
        out_hit.m_iEdgeV0 = static_cast<plInt32>(uiV0);
        out_hit.m_iEdgeV1 = static_cast<plInt32>(uiV1);
      }
    }
  }

  return true;
}

void plBrushToolContext::ApplyClickSelection(const HoverHit& hit, bool bAdditive)
{
  if (!hit.IsValid())
  {
    if (!bAdditive)
    {
      m_SelectedVertices.Clear();
      m_SelectedEdges.Clear();
      m_SelectedFaces.Clear();
    }

    return;
  }

  switch (m_SubMode)
  {
    case plBrushSubMode::Vertex:
    {
      const plUInt32 uiVertex = static_cast<plUInt32>(hit.m_iVertex);

      if (bAdditive)
      {
        if (!m_SelectedVertices.Remove(uiVertex))
          m_SelectedVertices.Insert(uiVertex);
      }
      else
      {
        m_SelectedVertices.Clear();
        m_SelectedVertices.Insert(uiVertex);
      }

      break;
    }

    case plBrushSubMode::Edge:
    {
      const plUInt64 uiKey = MakeEdgeKey(static_cast<plUInt32>(hit.m_iEdgeV0), static_cast<plUInt32>(hit.m_iEdgeV1));

      if (bAdditive)
      {
        if (!m_SelectedEdges.Remove(uiKey))
          m_SelectedEdges.Insert(uiKey);
      }
      else
      {
        m_SelectedEdges.Clear();
        m_SelectedEdges.Insert(uiKey);
      }

      break;
    }

    case plBrushSubMode::Face:
    {
      plHashSet<plUInt32> ids;

      if (UsesSurfaces())
      {
        // a CSG face: every piece of the surface under the cursor
        GatherSurfaceIds(m_SurfaceOfFace[hit.m_iFace], ids);
      }
      else
      {
        const plUInt32 uiFace = static_cast<plUInt32>(hit.m_iFace);
        GetPolygonIds(plArrayPtr<const plUInt32>(&uiFace, 1), ids);
      }

      SelectFaceIds(ids, bAdditive, false);
      break;
    }

    default:
      break;
  }
}

void plBrushToolContext::GatherSelectedVertices(plHashSet<plUInt32>& out_vertices) const
{
  out_vertices.Clear();

  for (plUInt32 uiVertex : m_SelectedVertices)
  {
    out_vertices.Insert(uiVertex);
  }

  for (plUInt64 uiEdge : m_SelectedEdges)
  {
    out_vertices.Insert(static_cast<plUInt32>(uiEdge >> 32));
    out_vertices.Insert(static_cast<plUInt32>(uiEdge & 0xFFFFFFFF));
  }

  if (!m_SelectedFaces.IsEmpty())
  {
    for (const plBrushFace& face : m_BaseMesh.m_Faces)
    {
      if (!m_SelectedFaces.Contains(face.m_uiFaceId))
        continue;

      for (plUInt32 uiCorner : face.m_Corners)
      {
        out_vertices.Insert(uiCorner);
      }
    }
  }
}

//////////////////////////////////////////////////////////////////////////
// sub-object modes

void plBrushToolContext::SetSubMode(plBrushSubMode::Enum mode)
{
  if (m_SubMode == mode)
    return;

  if (m_ModalOp != ModalOp::None)
  {
    CancelModalOp();
  }

  // the pivot gizmo belongs to the brush the old mode worked on
  SetPivotEdit(false).IgnoreResult();

  if (mode != plBrushSubMode::Object)
  {
    if (!AcquireTargetBrush())
    {
      if (GetOwnerWindow())
      {
        GetOwnerWindow()->SetPermanentStatusBarMsg("Brush: select a brush object first, then press 1/2/3 for vertex/edge/face mode.");
      }

      return;
    }

    m_LastSubMode = mode;

    // sub-object hotkeys (G/R/S, 1/2/3) must reach this context instead of firing document shortcuts
    SetShortcutsDisabled(true);
  }
  else
  {
    SetShortcutsDisabled(false);
    m_Hover.Reset();
    HideGizmos();
  }

  m_SubMode = mode;

  // a mode owns its selection: leftovers from another mode would silently join every G/gizmo
  // operation (the op works on the union of all sets)
  m_SelectedVertices.Clear();
  m_SelectedEdges.Clear();
  m_SelectedFaces.Clear();
  m_Hover.Reset();

  // with the selection gone an active gizmo has nothing to anchor to
  RepositionGizmos();

  UpdateOverlays();

  if (GetOwnerWindow())
  {
    UpdateStatusBarText(GetOwnerWindow());
  }

  plBrushModeEvent e;
  e.m_pContext = this;
  e.m_pDocument = m_pDocument;
  e.m_Mode = m_SubMode;
  s_ModeEvents.Broadcast(e);

  NotifySelectionChanged();
}

//////////////////////////////////////////////////////////////////////////
// modal operations

void plBrushToolContext::ApplyWorldTransformToSelection(const plVec3& vTranslate, const plQuat& qRotation, const plVec3& vScale)
{
  const plTransform invTransform = m_ObjectTransform.GetInverse();

  m_PreviewMesh = m_BaseMesh;

  for (plUInt32 uiVertex : m_ModalVertices)
  {
    const plVec3 vWorld = m_ObjectTransform.TransformPosition(m_BaseMesh.m_Vertices[uiVertex]);

    plVec3 vFromPivot = vWorld - m_vPivot;
    vFromPivot = qRotation * vFromPivot.CompMul(vScale);

    m_PreviewMesh.m_Vertices[uiVertex] = invTransform.TransformPosition(m_vPivot + vFromPivot + vTranslate);
  }
}

//////////////////////////////////////////////////////////////////////////
// gizmo workflow (W / E / R)

plGizmo* plBrushToolContext::GetActiveGizmoPtr()
{
  if (m_bPivotEdit)
    return &m_TranslateGizmo;

  switch (m_ActiveGizmo)
  {
    case ActiveGizmo::Translate:
      return &m_TranslateGizmo;
    case ActiveGizmo::Rotate:
      return &m_RotateGizmo;
    case ActiveGizmo::Scale:
      return &m_ScaleGizmo;
    default:
      return nullptr;
  }
}

plResult plBrushToolContext::ComputeSelectionPivot(plVec3& out_vPivot)
{
  plHashSet<plUInt32> vertices;
  GatherSelectedVertices(vertices);

  if (vertices.IsEmpty())
    return PL_FAILURE;

  plVec3 vPivot = plVec3::MakeZero();

  for (plUInt32 uiVertex : vertices)
  {
    vPivot += m_ObjectTransform.TransformPosition(m_BaseMesh.m_Vertices[uiVertex]);
  }

  out_vPivot = vPivot / static_cast<float>(vertices.GetCount());
  return PL_SUCCESS;
}

void plBrushToolContext::ShowGizmo(ActiveGizmo::Enum which)
{
  if (!RebuildBaseMesh())
    return;

  // the translate gizmo cannot sit on the pivot and on the selection at once
  SetPivotEdit(false).IgnoreResult();

  plVec3 vPivot;

  if (ComputeSelectionPivot(vPivot).Failed())
  {
    if (GetOwnerWindow())
    {
      GetOwnerWindow()->SetPermanentStatusBarMsg("Brush: nothing selected - click a vertex/edge/face first, then W/E/R for a gizmo.");
    }

    return;
  }

  m_ActiveGizmo = which;

  // world-space orientation at the selection pivot
  const plTransform transform(vPivot);

  m_TranslateGizmo.SetVisible(which == ActiveGizmo::Translate);
  m_RotateGizmo.SetVisible(which == ActiveGizmo::Rotate);
  m_ScaleGizmo.SetVisible(which == ActiveGizmo::Scale);

  if (plGizmo* pGizmo = GetActiveGizmoPtr())
  {
    pGizmo->SetTransformation(transform);
  }

  if (GetOwnerWindow())
  {
    UpdateStatusBarText(GetOwnerWindow());
  }
}

void plBrushToolContext::HideGizmos()
{
  const bool bEndsPivotEdit = m_bPivotEdit;

  m_ActiveGizmo = ActiveGizmo::None;
  m_bPivotEdit = false;
  m_PivotBrush = plUuid();

  m_TranslateGizmo.SetVisible(false);
  m_RotateGizmo.SetVisible(false);
  m_ScaleGizmo.SetVisible(false);

  HighlightHandle(plUuid());

  if (bEndsPivotEdit)
  {
    if (GetOwnerWindow())
    {
      UpdateStatusBarText(GetOwnerWindow());
    }

    plBrushModeEvent e;
    e.m_pContext = this;
    e.m_pDocument = m_pDocument;
    e.m_Mode = m_SubMode;
    s_ModeEvents.Broadcast(e);
  }
}

void plBrushToolContext::HighlightHandle(const plUuid& handle)
{
  if (handle == m_LastHighlightedHandle || GetOwnerWindow() == nullptr)
    return;

  m_LastHighlightedHandle = handle;

  plViewHighlightMsgToEngine msg;
  msg.m_HighlightObject = handle;
  GetOwnerWindow()->GetEditorEngineConnection()->SendHighlightObjectMessage(&msg);
}

plGizmoHandle* plBrushToolContext::FindActiveGizmoHandle(const plObjectPickingResult& res)
{
  plGizmo* pActive = GetActiveGizmoPtr();

  if (pActive == nullptr || !res.m_PickedOther.IsValid() || GetOwnerWindow() == nullptr)
    return nullptr;

  auto* pSO = GetOwnerWindow()->GetDocument()->FindSyncObject(res.m_PickedOther);

  if (pSO == nullptr || !pSO->GetDynamicRTTI()->IsDerivedFrom<plGizmoHandle>())
    return nullptr;

  plGizmoHandle* pHandle = static_cast<plGizmoHandle*>(pSO);
  return pHandle->GetOwnerGizmo() == pActive ? pHandle : nullptr;
}

bool plBrushToolContext::PressActiveGizmoHandle(const plEditorInputEvent& e, const plObjectPickingResult& res, plEditorInput& out_result)
{
  plGizmoHandle* pHandle = FindActiveGizmoHandle(res);

  if (pHandle == nullptr)
    return false;

  plGizmo* pGizmo = pHandle->GetOwnerGizmo();
  plEngineViewWidget* pView = GetOwnerView();
  const plVec2 vViewport = pView->GetViewportSize();
  pGizmo->ConfigureInteraction(pHandle, &pView->m_pViewConfig->m_Camera, res.m_vPickedPosition, plVec2I32(plMath::RoundToInt(vViewport.x), plMath::RoundToInt(vViewport.y)));
  out_result = pGizmo->MousePressEvent(e);
  return true;
}

//////////////////////////////////////////////////////////////////////////
// pivot editing

const plDocumentObject* plBrushToolContext::FindPivotBrush() const
{
  if (m_pDocument == nullptr)
    return nullptr;

  // while editing sub-objects the pivot belongs to the edited brush
  if (m_SubMode != plBrushSubMode::Object)
    return GetBrushObject();

  plDynamicArray<const plDocumentObject*> brushes;
  plBrushCommands::GatherSelectedBrushes(m_pDocument, brushes);
  return brushes.IsEmpty() ? nullptr : brushes[0];
}

plStatus plBrushToolContext::SetPivotEdit(bool bEnable)
{
  if (bEnable == m_bPivotEdit)
    return plStatus(PL_SUCCESS);

  if (!bEnable)
  {
    HideGizmos(); // ends the pivot session
    return plStatus(PL_SUCCESS);
  }

  if (m_ModalOp != ModalOp::None || m_bClipActive)
    return plStatus("Edit Pivot: finish the current operation first.");

  if (FindPivotBrush() == nullptr)
    return plStatus("Edit Pivot: select a brush first.");

  HideGizmos();
  m_bPivotEdit = true;
  UpdatePivotGizmo();

  if (GetOwnerWindow())
  {
    UpdateStatusBarText(GetOwnerWindow());
  }

  plBrushModeEvent e;
  e.m_pContext = this;
  e.m_pDocument = m_pDocument;
  e.m_Mode = m_SubMode;
  s_ModeEvents.Broadcast(e);

  return plStatus(PL_SUCCESS);
}

void plBrushToolContext::UpdatePivotGizmo()
{
  // a drag owns the gizmo's position until it ends
  if (!m_bPivotEdit || m_bPivotDragging)
    return;

  const plDocumentObject* pBrush = FindPivotBrush();

  if (pBrush == nullptr)
  {
    m_PivotBrush = plUuid();
    m_TranslateGizmo.SetVisible(false);
    return;
  }

  m_PivotBrush = pBrush->GetGuid();
  m_TranslateGizmo.SetTransformation(plTransform(plBrushPicking::GetComponentGlobalTransform(pBrush).m_vPosition));
  m_TranslateGizmo.SetVisible(true);
}

void plBrushToolContext::PivotGizmoEventHandler(const plGizmoEvent& e)
{
  switch (e.m_Type)
  {
    case plGizmoEvent::Type::BeginInteractions:
    {
      m_bPivotDragging = true;
      m_GizmoStartTransform = m_TranslateGizmo.GetTransformation();
      m_vPivot = m_GizmoStartTransform.m_vPosition;

      // the pivot may snap to any vertex, its own brush's included
      m_ModalVertices.Clear();
      GatherSnapVertices(false);
      break;
    }

    case plGizmoEvent::Type::Interaction:
    {
      m_szSnapInfo = nullptr;

      if (IsSnapHeld())
      {
        m_TranslateGizmo.SetTransformation(plTransform(m_vPivot + SnapGizmoTranslation(m_TranslateGizmo.GetTranslationResult())));
      }
      else if (plBrushToolSettings::GetSnapToGrid())
      {
        m_TranslateGizmo.SetTransformation(plTransform(SnapToGrid(m_vPivot + m_TranslateGizmo.GetTranslationResult())));
        m_szSnapInfo = "grid";
      }

      const plVec3 vPos = m_TranslateGizmo.GetTransformation().m_vPosition;
      m_sInfoText.SetFormat("Pivot {} {} {}{}{}", plArgF(vPos.x, 3), plArgF(vPos.y, 3), plArgF(vPos.z, 3), m_szSnapInfo ? "   snap: " : "", m_szSnapInfo ? m_szSnapInfo : "");
      break;
    }

    case plGizmoEvent::Type::EndInteractions:
    {
      m_bPivotDragging = false;
      m_SnapVertices.Clear();
      m_sInfoText.Clear();

      const plDocumentObject* pBrush = m_pDocument ? m_pDocument->GetObjectManager()->GetObject(m_PivotBrush) : nullptr;

      if (pBrush != nullptr)
      {
        const plStatus result = plBrushCommands::SetPivot(m_pDocument, pBrush, m_TranslateGizmo.GetTransformation().m_vPosition);

        if (result.Failed() && GetOwnerWindow())
        {
          GetOwnerWindow()->ShowTemporaryStatusBarMsg(plFmt("{}", result.m_sMessage), plTime::MakeFromSeconds(6));
        }
      }

      UpdatePivotGizmo();
      break;
    }

    case plGizmoEvent::Type::CancelInteractions:
    {
      m_bPivotDragging = false;
      m_SnapVertices.Clear();
      m_sInfoText.Clear();
      m_TranslateGizmo.SetTransformation(m_GizmoStartTransform);
      break;
    }

    default:
      break;
  }
}

plStatus plBrushToolContext::MovePivotToSelection()
{
  plVec3 vPivot;

  if (m_SubMode == plBrushSubMode::Object || !RebuildBaseMesh() || ComputeSelectionPivot(vPivot).Failed())
    return plStatus("Pivot To Selection: select vertices, edges or faces of a brush first.");

  const plStatus result = plBrushCommands::SetPivot(m_pDocument, GetBrushObject(), vPivot);

  UpdatePivotGizmo();
  return result;
}

//////////////////////////////////////////////////////////////////////////
// snapping

bool plBrushToolContext::IsSnapHeld()
{
  // the platform's key state: it also answers right while the Ctrl key event itself is delivered
  return (plEditorPlatform::GetKeyboardModifiers() & plEditorPlatformEvent::Control) != 0;
}

plVec3 plBrushToolContext::SnapToGrid(const plVec3& vWorld)
{
  // Ctrl asks for the grid even when the editor's snapping is switched off
  return plBrushToolSettings::SnapToGrid(vWorld);
}

plVec3 plBrushToolContext::AlignMoveToGrid(const plVec3& vDelta, const plVec3& vAxis) const
{
  if (!plBrushToolSettings::GetSnapToGrid() || m_ModalVertices.IsEmpty())
    return vDelta;

  plBoundingBox bounds = plBoundingBox::MakeInvalid();

  for (plUInt32 v : m_ModalVertices)
  {
    if (v < m_BaseMesh.m_Vertices.GetCount())
      bounds.ExpandToInclude(m_ObjectTransform.TransformPosition(m_BaseMesh.m_Vertices[v]));
  }

  if (!bounds.IsValid())
    return vDelta;

  const plVec3 vAligned = SnapToGrid(bounds.m_vMin + vDelta) - bounds.m_vMin;
  return vAxis.IsZero() ? vAligned : vAxis * vAxis.Dot(vAligned);
}

void plBrushToolContext::GatherSnapVertices(bool bExcludeMovingVertices)
{
  m_SnapVertices.Clear();

  if (m_pDocument == nullptr)
    return;

  plDynamicArray<const plDocumentObject*> brushes;
  plBrushPicking::FindAllBrushComponents(m_pDocument->GetObjectManager()->GetRootObject(), brushes);

  // the moving vertices themselves are no target; their mesh is the cached base mesh
  const plDocumentObject* pMovingBrush = bExcludeMovingVertices ? GetBrushObject() : nullptr;

  plEditableMesh mesh;

  for (const plDocumentObject* pBrush : brushes)
  {
    if (pBrush == pMovingBrush)
    {
      for (plUInt32 v = 0; v < m_BaseMesh.m_Vertices.GetCount(); ++v)
      {
        if (!m_ModalVertices.Contains(v))
          m_SnapVertices.PushBack(m_ObjectTransform.TransformPosition(m_BaseMesh.m_Vertices[v]));
      }
      continue;
    }

    if (plBrushPicking::BuildMeshFromObject(pBrush, mesh).Failed())
      continue;

    const plTransform t = plBrushPicking::GetComponentGlobalTransform(pBrush);

    for (const plVec3& v : mesh.m_Vertices)
    {
      m_SnapVertices.PushBack(t.TransformPosition(v));
    }
  }
}

plVec3 plBrushToolContext::SnapGizmoTranslation(const plVec3& vTranslate)
{
  plVec3 vTarget;

  plVec2I32 vCursor;
  if (GetCursorInView(vCursor) && FindSnapVertex(vCursor, vTarget, c_fCtrlSnapPixels))
  {
    m_szSnapInfo = "vertex";
  }
  else
  {
    vTarget = SnapToGrid(m_vPivot + vTranslate);
    m_szSnapInfo = "grid";
  }

  plVec3 vDelta = vTarget - m_vPivot;

  // keep the handle's constraint: the gizmo is world-aligned, so its axes and planes are the world's
  if (m_TranslateGizmo.GetTranslateMode() == plTranslateGizmo::TranslateMode::Axis)
  {
    // the dragged axis is the one the gizmo has moved along
    const plVec3 vAbs(plMath::Abs(vTranslate.x), plMath::Abs(vTranslate.y), plMath::Abs(vTranslate.z));

    if (vAbs.IsZero())
      return vTranslate;

    const plVec3 vAxis = (vAbs.x >= vAbs.y && vAbs.x >= vAbs.z) ? plVec3(1, 0, 0) : (vAbs.y >= vAbs.z) ? plVec3(0, 1, 0) : plVec3(0, 0, 1);
    vDelta = vAxis * vAxis.Dot(vDelta);
  }
  else if (m_TranslateGizmo.GetTranslateMode() == plTranslateGizmo::TranslateMode::Plane)
  {
    plVec3 vNormal(0, 0, 1);

    switch (m_TranslateGizmo.GetLastPlaneInteraction())
    {
      case plTranslateGizmo::PlaneInteraction::PlaneX:
        vNormal.Set(1, 0, 0);
        break;
      case plTranslateGizmo::PlaneInteraction::PlaneY:
        vNormal.Set(0, 1, 0);
        break;
      default:
        break;
    }

    vDelta -= vNormal * vNormal.Dot(vDelta);
  }

  return vDelta;
}

void plBrushToolContext::RepositionGizmos()
{
  if (m_ActiveGizmo == ActiveGizmo::None)
    return;

  plVec3 vPivot;

  if (ComputeSelectionPivot(vPivot).Failed())
  {
    HideGizmos();
    return;
  }

  if (plGizmo* pGizmo = GetActiveGizmoPtr())
  {
    pGizmo->SetTransformation(plTransform(vPivot));
  }
}

void plBrushToolContext::GizmoEventHandler(const plGizmoEvent& e)
{
  if (m_bPivotEdit && e.m_pGizmo == &m_TranslateGizmo)
  {
    PivotGizmoEventHandler(e);
    return;
  }

  switch (e.m_Type)
  {
    case plGizmoEvent::Type::BeginInteractions:
    {
      if (!RebuildBaseMesh())
        return;

      GatherSelectedVertices(m_ModalVertices);

      if (m_ModalVertices.IsEmpty())
        return;

      m_PreviewMesh = m_BaseMesh;

      if (plGizmo* pGizmo = GetActiveGizmoPtr())
      {
        m_GizmoStartTransform = pGizmo->GetTransformation();
        m_vPivot = m_GizmoStartTransform.m_vPosition;
      }

      if (m_ActiveGizmo == ActiveGizmo::Translate)
      {
        GatherSnapVertices(true);
      }

      m_pDocument->GetCommandHistory()->BeginTemporaryCommands("Edit Brush", true);
      m_bInTemporaryCommands = true;
      break;
    }

    case plGizmoEvent::Type::Interaction:
    {
      if (!m_bInTemporaryCommands)
        return;

      plVec3 vTranslate = plVec3::MakeZero();
      plQuat qRotation = plQuat::MakeIdentity();
      plVec3 vScale = plVec3(1, 1, 1);

      switch (m_ActiveGizmo)
      {
        case ActiveGizmo::Translate:
          vTranslate = m_TranslateGizmo.GetTranslationResult();

          // Ctrl: the selection's pivot lands on a vertex or the grid, and the gizmo goes with it
          m_szSnapInfo = nullptr;
          if (IsSnapHeld())
          {
            vTranslate = SnapGizmoTranslation(vTranslate);
            m_TranslateGizmo.SetTransformation(plTransform(m_vPivot + vTranslate));
          }
          else if (plBrushToolSettings::GetSnapToGrid())
          {
            vTranslate = AlignMoveToGrid(vTranslate, plVec3::MakeZero());
            m_TranslateGizmo.SetTransformation(plTransform(m_vPivot + vTranslate));
            m_szSnapInfo = "grid";
          }

          m_sInfoText.SetFormat("Move {} {} {} m{}{}", plArgF(vTranslate.x, 3), plArgF(vTranslate.y, 3), plArgF(vTranslate.z, 3), m_szSnapInfo ? "   snap: " : "", m_szSnapInfo ? m_szSnapInfo : "");
          break;
        case ActiveGizmo::Rotate:
          qRotation = m_RotateGizmo.GetRotationResult();
          break;
        case ActiveGizmo::Scale:
          vScale = m_ScaleGizmo.GetScalingResult();
          break;
        default:
          return;
      }

      ApplyWorldTransformToSelection(vTranslate, qRotation, vScale);
      WritePreview(m_PreviewMesh, false);
      UpdateOverlays();
      break;
    }

    case plGizmoEvent::Type::EndInteractions:
    {
      if (m_bInTemporaryCommands)
      {
        m_pDocument->GetCommandHistory()->FinishTemporaryCommands();
        m_bInTemporaryCommands = false;
      }

      m_BaseMesh = m_PreviewMesh;
      m_SnapVertices.Clear();
      RepositionGizmos();
      UpdateOverlays();
      NotifySelectionChanged(); // listeners skipped the preview writes
      break;
    }

    case plGizmoEvent::Type::CancelInteractions:
    {
      if (m_bInTemporaryCommands)
      {
        m_pDocument->GetCommandHistory()->CancelTemporaryCommands();
        m_bInTemporaryCommands = false;
      }

      m_SnapVertices.Clear();
      m_sInfoText.Clear();

      if (plGizmo* pGizmo = GetActiveGizmoPtr())
      {
        pGizmo->SetTransformation(m_GizmoStartTransform);
      }

      UpdateOverlays();
      break;
    }

    default:
      break;
  }
}

plVec3 plBrushToolContext::GetAxisVector() const
{
  switch (m_AxisLock)
  {
    case AxisLock::X:
      return plVec3(1, 0, 0);
    case AxisLock::Y:
      return plVec3(0, 1, 0);
    case AxisLock::Z:
      return plVec3(0, 0, 1);
    default:
      return plVec3::MakeZero();
  }
}

bool plBrushToolContext::GetCursorInView(plVec2I32& out_vViewPos) const
{
  const plEngineViewWidget* pView = GetOwnerView();
  if (pView == nullptr)
    return false;

  // the cursor and the view's slot are in physical pixels; positions in the view are logical units
  const plVec2I32 vCursor = plEditorPlatform::GetCursorScreenPosition();
  const plRectI32 slot = pView->GetEngineShellScreenRect();
  const float fPixelRatio = plMath::Max(pView->GetViewportPixelRatio(), 0.01f);

  out_vViewPos.x = plMath::RoundToInt((vCursor.x - slot.x) / fPixelRatio);
  out_vViewPos.y = plMath::RoundToInt((vCursor.y - slot.y) / fPixelRatio);
  return true;
}

plResult plBrushToolContext::PickOnWorkPlane(const plVec2I32& vViewPos, plVec3& out_vPoint) const
{
  if (GetOwnerView() == nullptr)
    return PL_FAILURE;

  const plCamera& camera = GetOwnerView()->m_pViewConfig->m_Camera;
  const plVec3 vCamFwd = camera.GetDirForwards();

  plVec3 vNormal = vCamFwd;

  if (m_ModalOp == ModalOp::Rotate)
  {
    // rotation reads angles on the rotation plane itself
    const plVec3 vAxis = GetAxisVector();

    if (!vAxis.IsZero())
    {
      vNormal = vAxis;
    }
  }
  else if (m_AxisLock != AxisLock::None || (m_ModalOp == ModalOp::Extrude && !m_vOpAxis.IsZero()))
  {
    // translation along an axis: pick on the plane that contains the axis and faces the camera most
    const plVec3 vAxis = (m_ModalOp == ModalOp::Extrude) ? m_vOpAxis : GetAxisVector();
    plVec3 vSide = vAxis.CrossRH(vCamFwd);

    if (vSide.GetLengthSquared() > 0.0001f)
    {
      vNormal = vAxis.CrossRH(vSide);
      vNormal.Normalize();
    }
  }

  const plPlane plane = plPlane::MakeFromNormalAndPoint(vNormal, m_vPivot);

  return GetOwnerView()->PickPlane(static_cast<plUInt16>(vViewPos.x), static_cast<plUInt16>(vViewPos.y), plane, out_vPoint);
}

void plBrushToolContext::BeginModalOp(ModalOp::Enum op)
{
  if (!HasViewForModalOp() || !RebuildBaseMesh())
    return;

  GatherSelectedVertices(m_ModalVertices);

  if (m_ModalVertices.IsEmpty())
  {
    if (GetOwnerWindow())
    {
      GetOwnerWindow()->SetPermanentStatusBarMsg("Brush: nothing selected - click a vertex/edge/face first.");
    }

    return;
  }

  m_ModalOp = op;
  m_AxisLock = AxisLock::None;
  m_sNumericInput.Clear();
  m_PreviewMesh = m_BaseMesh;

  // world-space pivot: average of the operated vertices
  plVec3 vPivot = plVec3::MakeZero();

  for (plUInt32 uiVertex : m_ModalVertices)
  {
    vPivot += m_ObjectTransform.TransformPosition(m_BaseMesh.m_Vertices[uiVertex]);
  }

  m_vPivot = vPivot / static_cast<float>(m_ModalVertices.GetCount());

  // vertex snapping (Ctrl or V) targets every other vertex of every brush, gathered once per move
  m_SnapVertices.Clear();

  if (op == ModalOp::Move)
  {
    GatherSnapVertices(true);
  }

  GetCursorInView(m_vLastMouse);

  if (PickOnWorkPlane(m_vLastMouse, m_vStartPoint).Failed())
  {
    m_vStartPoint = m_vPivot;
  }

  m_vCurrentPoint = m_vStartPoint;

  MakeActiveInputContext(true);

  m_pDocument->GetCommandHistory()->BeginTemporaryCommands("Edit Brush", true);
  m_bInTemporaryCommands = true;

  WritePreview(m_PreviewMesh, false);

  if (GetOwnerWindow())
  {
    UpdateStatusBarText(GetOwnerWindow());
  }
}

void plBrushToolContext::UpdateModalPreview()
{
  if (m_ModalOp == ModalOp::None)
    return;

  const plVec2I32 viewPos = m_vLastMouse;

  double fNumericValue = 0.0;
  const bool bNumeric = !m_sNumericInput.IsEmpty() && plConversionUtils::StringToFloat(m_sNumericInput, fNumericValue).Succeeded();

  if (m_ModalOp >= ModalOp::Extrude)
  {
    float fAmount = m_fOpAmount;

    if (bNumeric)
    {
      fAmount = static_cast<float>(fNumericValue);
    }
    else if (PickOnWorkPlane(viewPos, m_vCurrentPoint).Succeeded())
    {
      switch (m_ModalOp)
      {
        case ModalOp::Extrude:
        {
          fAmount = m_vOpAxis.Dot(m_vCurrentPoint - m_vStartPoint);

          if (plBrushToolSettings::GetSnapToGrid())
            fAmount = plMath::RoundToMultiple(fAmount, plBrushToolSettings::GetGridStep());
          break;
        }

        case ModalOp::Inset:
          fAmount = plMath::Max(0.0f, (m_vStartPoint - m_vPivot).GetLength() - (m_vCurrentPoint - m_vPivot).GetLength());
          break;

        case ModalOp::Bevel:
          fAmount = plMath::Max(0.0f, (m_vCurrentPoint - m_vPivot).GetLength() - (m_vStartPoint - m_vPivot).GetLength());
          break;

        case ModalOp::LoopCut:
        {
          // slide: the cursor's position along the picked edge
          const plVec3 v0 = m_ObjectTransform.TransformPosition(m_BaseMesh.m_Vertices[m_uiLoopEdgeV0]);
          const plVec3 v1 = m_ObjectTransform.TransformPosition(m_BaseMesh.m_Vertices[m_uiLoopEdgeV1]);
          const plVec3 vEdge = v1 - v0;
          fAmount = plMath::Clamp((m_vCurrentPoint - v0).Dot(vEdge) / plMath::Max(vEdge.GetLengthSquared(), 0.000001f), 0.02f, 0.98f);
          break;
        }

        default:
          break;
      }
    }

    UpdateMeshModalPreview(fAmount);
    return;
  }

  const plVec3 vAxis = GetAxisVector();

  plVec3 vDelta = plVec3::MakeZero();
  plQuat qRotation = plQuat::MakeIdentity();
  float fScale = 1.0f;
  float fAngleDeg = 0.0f;

  if (m_ModalOp == ModalOp::Move)
  {
    // Ctrl: onto a vertex near the cursor, else onto the grid; V: onto vertices from further away
    const bool bCtrlSnap = IsSnapHeld();
    m_szSnapInfo = nullptr;

    if (bNumeric && !vAxis.IsZero())
    {
      vDelta = vAxis * static_cast<float>(fNumericValue);
    }
    else if (plVec3 vSnapTarget; (m_bVertexSnap || bCtrlSnap) && FindSnapVertex(viewPos, vSnapTarget, m_bVertexSnap ? 30.0f : c_fCtrlSnapPixels))
    {
      // the pivot lands exactly on another vertex
      vDelta = vAxis.IsZero() ? (vSnapTarget - m_vPivot) : vAxis * vAxis.Dot(vSnapTarget - m_vPivot);
      m_szSnapInfo = "vertex";
    }
    else if (PickOnWorkPlane(viewPos, m_vCurrentPoint).Succeeded())
    {
      if (vAxis.IsZero())
      {
        vDelta = m_vCurrentPoint - m_vStartPoint;
      }
      else
      {
        vDelta = vAxis * vAxis.Dot(m_vCurrentPoint - m_vStartPoint);
      }

      if (bCtrlSnap)
      {
        // the pivot lands on a grid point, not just a grid step away from where it was
        vDelta = SnapToGrid(m_vPivot + vDelta) - m_vPivot;

        if (!vAxis.IsZero())
          vDelta = vAxis * vAxis.Dot(vDelta);

        m_szSnapInfo = "grid";
      }
      else if (plBrushToolSettings::GetSnapToGrid())
      {
        vDelta = AlignMoveToGrid(vDelta, vAxis);
        m_szSnapInfo = "grid";
      }
    }

    m_sInfoText.SetFormat("Move {} {} {} m{}{}", plArgF(vDelta.x, 3), plArgF(vDelta.y, 3), plArgF(vDelta.z, 3), m_szSnapInfo ? "   snap: " : "", m_szSnapInfo ? m_szSnapInfo : "");
  }
  else if (m_ModalOp == ModalOp::Rotate)
  {
    const plCamera& camera = GetOwnerView()->m_pViewConfig->m_Camera;
    const plVec3 vRotAxis = vAxis.IsZero() ? camera.GetDirForwards() : vAxis;

    if (bNumeric)
    {
      fAngleDeg = static_cast<float>(fNumericValue);
    }
    else if (PickOnWorkPlane(viewPos, m_vCurrentPoint).Succeeded())
    {
      const plVec3 v0 = m_vStartPoint - m_vPivot;
      const plVec3 v1 = m_vCurrentPoint - m_vPivot;

      if (v0.GetLengthSquared() > 0.000001f && v1.GetLengthSquared() > 0.000001f)
      {
        fAngleDeg = plMath::ATan2(vRotAxis.Dot(v0.CrossRH(v1)), v0.Dot(v1)).GetDegree();
      }
    }

    qRotation = plQuat::MakeFromAxisAndAngle(vRotAxis.GetNormalized(), plAngle::MakeFromDegree(fAngleDeg));
  }
  else if (m_ModalOp == ModalOp::Scale)
  {
    if (bNumeric)
    {
      fScale = static_cast<float>(fNumericValue);
    }
    else if (PickOnWorkPlane(viewPos, m_vCurrentPoint).Succeeded())
    {
      const float fStartDist = (m_vStartPoint - m_vPivot).GetLength();
      const float fCurrentDist = (m_vCurrentPoint - m_vPivot).GetLength();

      fScale = (fStartDist > 0.0001f) ? (fCurrentDist / fStartDist) : 1.0f;
    }
  }

  plVec3 vScaleVec(1, 1, 1);

  if (m_ModalOp == ModalOp::Scale)
  {
    vScaleVec = vAxis.IsZero() ? plVec3(fScale) : plVec3(1, 1, 1) + vAxis * (fScale - 1.0f);
  }

  ApplyWorldTransformToSelection(vDelta, qRotation, vScaleVec);

  WritePreview(m_PreviewMesh, false);
  UpdateOverlays();

  if (GetOwnerWindow())
  {
    UpdateStatusBarText(GetOwnerWindow());
  }
}

void plBrushToolContext::WritePreview(const plEditableMesh& mesh, bool bFinal)
{
  PL_IGNORE_UNUSED(bFinal);

  if (!m_bInTemporaryCommands)
    return;

  plDataBuffer blob;
  mesh.ToDataBuffer(blob);

  auto* pHistory = m_pDocument->GetCommandHistory();

  pHistory->StartTransaction("Edit Brush");

  plStatus result(PL_SUCCESS);
  {
    plSetObjectPropertyCommand cmd;
    cmd.m_Object = m_BrushComponent;
    cmd.m_sProperty = "Shape";
    cmd.m_NewValue = static_cast<plInt64>(plBrushShape::Editable);
    result = pHistory->AddCommand(cmd);
  }
  if (result.Succeeded())
  {
    plSetObjectPropertyCommand cmd;
    cmd.m_Object = m_BrushComponent;
    cmd.m_sProperty = "StoredMesh";
    cmd.m_NewValue = blob;
    result = pHistory->AddCommand(cmd);
  }

  if (result.Failed())
  {
    // the brush went away under the drag (deleted, undone): the preview stops, the document stays usable
    pHistory->CancelTransaction();
    plLog::Error("Brush preview failed: {}", result.m_sMessage);
    return;
  }

  pHistory->FinishTransaction();
}

void plBrushToolContext::CommitModalOp()
{
  if (m_ModalOp == ModalOp::None)
    return;

  if (m_bInTemporaryCommands)
  {
    m_pDocument->GetCommandHistory()->FinishTemporaryCommands();
    m_bInTemporaryCommands = false;
  }

  // polygon operations renumber vertices; face ids stay valid
  if (m_ModalOp >= ModalOp::Extrude)
  {
    m_SelectedVertices.Clear();
    m_SelectedEdges.Clear();
  }

  m_BaseMesh = m_PreviewMesh;

  EndModalCommon();
}

void plBrushToolContext::CancelModalOp()
{
  if (m_ModalOp == ModalOp::None)
    return;

  if (m_bInTemporaryCommands)
  {
    m_pDocument->GetCommandHistory()->CancelTemporaryCommands();
    m_bInTemporaryCommands = false;
  }

  EndModalCommon();
}

void plBrushToolContext::EndModalCommon()
{
  m_ModalOp = ModalOp::None;
  m_AxisLock = AxisLock::None;
  m_sNumericInput.Clear();
  m_vOpAxis.SetZero();
  m_bVertexSnap = false;
  m_SnapVertices.Clear();

  MakeActiveInputContext(false);

  RepositionGizmos();
  UpdateOverlays();
  NotifySelectionChanged();

  if (GetOwnerWindow())
  {
    UpdateStatusBarText(GetOwnerWindow());
  }
}

//////////////////////////////////////////////////////////////////////////
// polygon operations

plStatus plBrushToolContext::RunModalTransform(bool bScale)
{
  if (m_SubMode == plBrushSubMode::Object)
    return plStatus("Grab and scale work on vertices, edges or faces: press 1, 2 or 3 first.");

  if (m_ModalOp != ModalOp::None)
    return plStatus(PL_SUCCESS);

  BeginModalOp(bScale ? ModalOp::Scale : ModalOp::Move);
  return plStatus(PL_SUCCESS);
}

plStatus plBrushToolContext::RunMeshOp(plBrushMeshOp::Enum op)
{
  if (m_SubMode == plBrushSubMode::Object || GetBrushObject() == nullptr)
    return plStatus("Pick vertex, edge or face mode on a brush first (1, 2, 3).");

  if (m_ModalOp != ModalOp::None)
    return plStatus("Finish the running operation first (Enter or Esc).");

  switch (op)
  {
    case plBrushMeshOp::Extrude:
    case plBrushMeshOp::Inset:
      if (m_SubMode != plBrushSubMode::Face || m_SelectedFaces.IsEmpty())
        return plStatus(plFmt("{}: select faces first (face mode, 3).", op == plBrushMeshOp::Extrude ? "Extrude" : "Inset"));
      BeginMeshModalOp(op == plBrushMeshOp::Extrude ? ModalOp::Extrude : ModalOp::Inset);
      return plStatus(PL_SUCCESS);

    case plBrushMeshOp::Bevel:
      if (m_SubMode != plBrushSubMode::Edge || m_SelectedEdges.IsEmpty())
        return plStatus("Bevel: select edges first (edge mode, 2).");
      BeginMeshModalOp(ModalOp::Bevel);
      return plStatus(PL_SUCCESS);

    case plBrushMeshOp::LoopCut:
      if (m_SubMode != plBrushSubMode::Edge || m_SelectedEdges.GetCount() != 1)
        return plStatus("Loop cut: select one edge the loop should cross (edge mode, 2).");
      BeginMeshModalOp(ModalOp::LoopCut);
      return plStatus(PL_SUCCESS);

    default:
      return ApplyImmediateMeshOp(op);
  }
}

bool plBrushToolContext::HasViewForModalOp() const
{
  if (GetOwnerView() != nullptr)
    return true;

  // modal operations follow the mouse in a view; started from a menu before any view was used, there is none
  if (GetOwnerWindow())
  {
    GetOwnerWindow()->ShowTemporaryStatusBarMsg("Brush: move the mouse over a viewport first.");
  }

  return false;
}

void plBrushToolContext::BeginMeshModalOp(ModalOp::Enum op)
{
  if (!HasViewForModalOp() || !RebuildBaseMesh())
    return;

  m_ModalOp = op;
  m_AxisLock = AxisLock::None;
  m_sNumericInput.Clear();
  m_fOpAmount = (op == ModalOp::LoopCut) ? 0.5f : 0.0f;
  m_uiOpSegments = 1;
  m_PreviewMesh = m_BaseMesh;
  m_vOpAxis.SetZero();

  // pivot and axis from the selection
  plVec3 vPivot = plVec3::MakeZero();
  plVec3 vNormalSum = plVec3::MakeZero();
  plUInt32 uiCount = 0;

  if (op == ModalOp::Extrude || op == ModalOp::Inset)
  {
    for (plUInt32 f = 0; f < m_BaseMesh.m_Faces.GetCount(); ++f)
    {
      if (!m_SelectedFaces.Contains(m_BaseMesh.m_Faces[f].m_uiFaceId))
        continue;

      vPivot += m_ObjectTransform.TransformPosition(m_BaseMesh.ComputeFaceCenter(f));
      vNormalSum += m_ObjectTransform.TransformDirection(m_BaseMesh.ComputeFaceNormal(f));
      ++uiCount;
    }
  }
  else
  {
    for (plUInt64 uiEdge : m_SelectedEdges)
    {
      const plUInt32 v0 = static_cast<plUInt32>(uiEdge >> 32);
      const plUInt32 v1 = static_cast<plUInt32>(uiEdge & 0xFFFFFFFF);
      vPivot += m_ObjectTransform.TransformPosition(plMath::Lerp(m_BaseMesh.m_Vertices[v0], m_BaseMesh.m_Vertices[v1], 0.5f));
      ++uiCount;

      m_uiLoopEdgeV0 = v0;
      m_uiLoopEdgeV1 = v1;
    }
  }

  m_vPivot = vPivot / static_cast<float>(plMath::Max(uiCount, 1u));
  m_vOpAxis = vNormalSum.GetNormalized();

  GetCursorInView(m_vLastMouse);

  if (PickOnWorkPlane(m_vLastMouse, m_vStartPoint).Failed())
  {
    m_vStartPoint = m_vPivot;
  }

  m_vCurrentPoint = m_vStartPoint;

  MakeActiveInputContext(true);

  m_pDocument->GetCommandHistory()->BeginTemporaryCommands("Edit Brush", true);
  m_bInTemporaryCommands = true;

  UpdateMeshModalPreview(m_fOpAmount);
}

void plBrushToolContext::UpdateMeshModalPreview(float fAmount)
{
  m_fOpAmount = fAmount;
  m_PreviewMesh = m_BaseMesh;

  // mesh space is the brush's local space; amounts are typed and dragged in world units
  const float fScale = plMath::Max(plMath::Abs(m_ObjectTransform.m_vScale.x), 0.0001f);
  plStatus result(PL_SUCCESS);
  const char* szOp = "";

  switch (m_ModalOp)
  {
    case ModalOp::Extrude:
      szOp = "Extrude";
      if (plMath::Abs(fAmount) > 0.00001f)
        result = plMeshOps::Extrude(m_PreviewMesh, m_SelectedFaces, fAmount / fScale, true);
      break;

    case ModalOp::Inset:
      szOp = "Inset";
      if (fAmount > 0.00001f)
        result = plMeshOps::Inset(m_PreviewMesh, m_SelectedFaces, fAmount / fScale);
      break;

    case ModalOp::Bevel:
    {
      szOp = "Bevel";
      plHybridArray<plUInt64, 8> edges;
      for (plUInt64 uiEdge : m_SelectedEdges)
        edges.PushBack(plMeshTopology::MakeKey(static_cast<plUInt32>(uiEdge >> 32), static_cast<plUInt32>(uiEdge & 0xFFFFFFFF)));

      if (fAmount > 0.00001f)
        result = plMeshOps::Bevel(m_PreviewMesh, edges, fAmount / fScale, m_uiOpSegments);
      break;
    }

    case ModalOp::LoopCut:
      szOp = "Loop cut";
      result = plMeshOps::LoopCut(m_PreviewMesh, m_uiLoopEdgeV0, m_uiLoopEdgeV1, m_uiOpSegments, fAmount);
      break;

    default:
      break;
  }

  if (result.Failed())
  {
    m_PreviewMesh = m_BaseMesh;
    m_sInfoText = result.m_sMessage;
  }
  else if (m_ModalOp == ModalOp::LoopCut && m_uiOpSegments == 1)
  {
    m_sInfoText.SetFormat("{}: 1 cut at {}% - wheel or +/- for more cuts", szOp, plArgF(fAmount * 100.0f, 0));
  }
  else if (m_ModalOp == ModalOp::LoopCut)
  {
    m_sInfoText.SetFormat("{}: {} even cuts - wheel or +/- to change", szOp, m_uiOpSegments);
  }
  else if (m_ModalOp == ModalOp::Bevel)
  {
    m_sInfoText.SetFormat("{} {} m, {} segment(s) - wheel or +/- for segments", szOp, plArgF(fAmount, 3), m_uiOpSegments);
  }
  else
  {
    m_sInfoText.SetFormat("{} {} m", szOp, plArgF(fAmount, 3));
  }

  WritePreview(m_PreviewMesh, false);
  UpdateOverlays();

  if (GetOwnerWindow())
  {
    UpdateStatusBarText(GetOwnerWindow());
  }
}

plStatus plBrushToolContext::ApplyImmediateMeshOp(plBrushMeshOp::Enum op)
{
  const plDocumentObject* pBrush = GetBrushObject();

  if (!RebuildBaseMesh())
    return plStatus("The brush can no longer be edited.");

  plStatus result(PL_SUCCESS);

  const plHashSet<plUInt32> faces = m_SelectedFaces;
  const plHashSet<plUInt32> vertices = m_SelectedVertices;
  const plHashSet<plUInt64> edges = m_SelectedEdges;

  auto firstEdge = [&](plUInt32& out_v0, plUInt32& out_v1)
  {
    for (plUInt64 uiEdge : edges)
    {
      out_v0 = static_cast<plUInt32>(uiEdge >> 32);
      out_v1 = static_cast<plUInt32>(uiEdge & 0xFFFFFFFF);
      return true;
    }
    return false;
  };

  switch (op)
  {
    case plBrushMeshOp::Bridge:
    {
      if (faces.GetCount() != 2)
        return plStatus("Bridge: select exactly two faces with the same number of corners.");

      plHybridArray<plUInt32, 2> ids;
      for (plUInt32 id : faces)
        ids.PushBack(id);

      result = plBrushCommands::EditMesh(m_pDocument, pBrush, "Bridge", [&](plEditableMesh& mesh)
        { return plMeshOps::Bridge(mesh, ids[0], ids[1]); });
      break;
    }

    case plBrushMeshOp::Weld:
      result = plBrushCommands::EditMesh(m_pDocument, pBrush, "Weld", [&](plEditableMesh& mesh)
        {
          plHashSet<plUInt32> weld = vertices;
          for (plUInt64 uiEdge : edges)
          {
            weld.Insert(static_cast<plUInt32>(uiEdge >> 32));
            weld.Insert(static_cast<plUInt32>(uiEdge & 0xFFFFFFFF));
          }
          return plMeshOps::Weld(mesh, weld);
        });
      break;

    case plBrushMeshOp::Flip:
      result = plBrushCommands::EditMesh(m_pDocument, pBrush, "Flip Faces", [&](plEditableMesh& mesh)
        { return plMeshOps::Flip(mesh, faces); });
      break;

    case plBrushMeshOp::Subdivide:
      result = plBrushCommands::EditMesh(m_pDocument, pBrush, "Subdivide", [&](plEditableMesh& mesh)
        { return plMeshOps::Subdivide(mesh, faces); });
      break;

    case plBrushMeshOp::Triangulate:
      result = plBrushCommands::EditMesh(m_pDocument, pBrush, "Triangulate", [&](plEditableMesh& mesh)
        { return plMeshOps::Triangulate(mesh, faces); });
      break;

    case plBrushMeshOp::Flatten:
      result = plBrushCommands::EditMesh(m_pDocument, pBrush, "Flatten", [&](plEditableMesh& mesh)
        { return plMeshOps::Flatten(mesh, faces); });
      break;

    case plBrushMeshOp::Detach:
      result = plBrushCommands::Detach(m_pDocument, pBrush, faces);
      break;

    case plBrushMeshOp::Delete:
      result = plBrushCommands::EditMesh(m_pDocument, pBrush, "Delete", [&](plEditableMesh& mesh)
        {
          if (m_SubMode == plBrushSubMode::Face)
            return plMeshOps::DeleteFaces(mesh, faces);

          // an edge takes both of its faces with it
          plHashSet<plUInt32> verts = vertices;
          if (m_SubMode == plBrushSubMode::Edge)
          {
            plHashSet<plUInt32> faceIds;
            for (plUInt64 uiEdge : edges)
            {
              const plUInt32 a = static_cast<plUInt32>(uiEdge >> 32);
              const plUInt32 b = static_cast<plUInt32>(uiEdge & 0xFFFFFFFF);

              for (const plBrushFace& face : mesh.m_Faces)
              {
                if (face.m_Corners.Contains(a) && face.m_Corners.Contains(b))
                  faceIds.Insert(face.m_uiFaceId);
              }
            }
            return plMeshOps::DeleteFaces(mesh, faceIds);
          }

          return plMeshOps::DeleteVertices(mesh, verts);
        });
      break;

    case plBrushMeshOp::FillHole:
    {
      plUInt32 v0 = 0, v1 = 0;
      if (!firstEdge(v0, v1))
        return plStatus("Fill hole: select an edge on the rim of the opening.");

      result = plBrushCommands::EditMesh(m_pDocument, pBrush, "Fill Hole", [&](plEditableMesh& mesh)
        { return plMeshOps::FillHole(mesh, v0, v1); });
      break;
    }

    case plBrushMeshOp::Connect:
    {
      if (vertices.GetCount() != 2)
        return plStatus("Connect: select exactly two vertices of one face.");

      plHybridArray<plUInt32, 2> v;
      for (plUInt32 i : vertices)
        v.PushBack(i);

      result = plBrushCommands::EditMesh(m_pDocument, pBrush, "Connect", [&](plEditableMesh& mesh)
        { return plMeshOps::Connect(mesh, v[0], v[1]); });
      break;
    }

    default:
      return plStatus("This operation needs a drag: start it from the keyboard or the panel.");
  }

  if (result.Succeeded())
  {
    // vertex indices changed; face ids did not
    m_SelectedVertices.Clear();
    m_SelectedEdges.Clear();

    // a delete leaves no faces to keep selected
    if (op == plBrushMeshOp::Delete || op == plBrushMeshOp::Detach)
      m_SelectedFaces.Clear();

    RefreshFromDocument();
  }

  return result;
}

//////////////////////////////////////////////////////////////////////////
// selection operations

void plBrushToolContext::ApplySelectionOp(plBrushSelectionOp::Enum op)
{
  if (m_SubMode == plBrushSubMode::Object || !RebuildBaseMesh())
    return;

  const plEditableMesh& mesh = m_BaseMesh;

  plMeshTopology topology;
  topology.Build(mesh);

  switch (op)
  {
    case plBrushSelectionOp::All:
    case plBrushSelectionOp::Invert:
    {
      const bool bInvert = op == plBrushSelectionOp::Invert;

      if (m_SubMode == plBrushSubMode::Vertex)
      {
        plHashSet<plUInt32> result;
        for (plUInt32 v = 0; v < mesh.m_Vertices.GetCount(); ++v)
          if (!bInvert || !m_SelectedVertices.Contains(v))
            result.Insert(v);
        m_SelectedVertices = result;
      }
      else if (m_SubMode == plBrushSubMode::Edge)
      {
        plDynamicArray<plUInt32> pairs;
        BuildEdgeList(mesh, pairs);
        plHashSet<plUInt64> result;
        for (plUInt32 i = 0; i + 1 < pairs.GetCount(); i += 2)
        {
          const plUInt64 key = MakeEdgeKey(pairs[i], pairs[i + 1]);
          if (!bInvert || !m_SelectedEdges.Contains(key))
            result.Insert(key);
        }
        m_SelectedEdges = result;
      }
      else
      {
        // CSG faces include the cut faces only the rendered mesh has
        const plEditableMesh& faceMesh = UsesSurfaces() ? GetSurfaceMesh() : mesh;

        plHashSet<plUInt32> result;
        for (const plBrushFace& face : faceMesh.m_Faces)
          if ((face.m_uiFlags & plBrushFaceFlags::Internal) == 0 && (!bInvert || !m_SelectedFaces.Contains(face.m_uiFaceId)))
            result.Insert(face.m_uiFaceId);
        m_SelectedFaces = result;
      }
      break;
    }

    case plBrushSelectionOp::None:
      m_SelectedVertices.Clear();
      m_SelectedEdges.Clear();
      m_SelectedFaces.Clear();
      break;

    case plBrushSelectionOp::Grow:
    case plBrushSelectionOp::Shrink:
    {
      const bool bGrow = op == plBrushSelectionOp::Grow;

      if (m_SubMode == plBrushSubMode::Face)
      {
        // faces sharing a vertex with the selection (grow), or selected faces with an unselected neighbour (shrink)
        plHashSet<plUInt32> selectedVerts;
        plHashSet<plUInt32> unselectedVerts;
        for (const plBrushFace& face : mesh.m_Faces)
          for (plUInt32 c : face.m_Corners)
            (m_SelectedFaces.Contains(face.m_uiFaceId) ? selectedVerts : unselectedVerts).Insert(c);

        plHashSet<plUInt32> result;
        for (const plBrushFace& face : mesh.m_Faces)
        {
          bool bTouchesSelected = false, bTouchesUnselected = false;
          for (plUInt32 c : face.m_Corners)
          {
            bTouchesSelected |= selectedVerts.Contains(c);
            bTouchesUnselected |= unselectedVerts.Contains(c);
          }

          const bool bSelected = m_SelectedFaces.Contains(face.m_uiFaceId);
          if (bGrow ? (bSelected || bTouchesSelected) : (bSelected && !bTouchesUnselected))
            result.Insert(face.m_uiFaceId);
        }
        m_SelectedFaces = result;
      }
      else
      {
        // vertices (edges through their end points) connected by an edge
        plHashSet<plUInt32> verts;
        GatherSelectedVertices(verts);

        plHashSet<plUInt32> result;
        for (plUInt32 h = 0; h < topology.GetHalfEdgeCount(); ++h)
        {
          const auto& he = topology.GetHalfEdge(h);
          const bool bFrom = verts.Contains(he.m_uiFrom);
          const bool bTo = verts.Contains(he.m_uiTo);

          if (bGrow && (bFrom || bTo))
          {
            result.Insert(he.m_uiFrom);
            result.Insert(he.m_uiTo);
          }
        }

        if (!bGrow)
        {
          result = verts;
          for (plUInt32 h = 0; h < topology.GetHalfEdgeCount(); ++h)
          {
            const auto& he = topology.GetHalfEdge(h);
            if (verts.Contains(he.m_uiFrom) && !verts.Contains(he.m_uiTo))
              result.Remove(he.m_uiFrom);
            if (verts.Contains(he.m_uiTo) && !verts.Contains(he.m_uiFrom))
              result.Remove(he.m_uiTo);
          }
        }

        if (m_SubMode == plBrushSubMode::Vertex)
        {
          m_SelectedVertices = result;
        }
        else
        {
          m_SelectedEdges.Clear();
          for (plUInt32 h = 0; h < topology.GetHalfEdgeCount(); ++h)
          {
            const auto& he = topology.GetHalfEdge(h);
            if (result.Contains(he.m_uiFrom) && result.Contains(he.m_uiTo))
              m_SelectedEdges.Insert(MakeEdgeKey(he.m_uiFrom, he.m_uiTo));
          }
        }
      }
      break;
    }

    case plBrushSelectionOp::Loop:
    case plBrushSelectionOp::Ring:
    {
      if (m_SubMode != plBrushSubMode::Edge)
        break;

      plHashSet<plUInt64> result = m_SelectedEdges;

      for (plUInt64 uiEdge : m_SelectedEdges)
      {
        const plUInt32 h = topology.FindEdge(static_cast<plUInt32>(uiEdge >> 32), static_cast<plUInt32>(uiEdge & 0xFFFFFFFF));
        if (h == plMeshTopology::c_uiInvalid)
          continue;

        if (op == plBrushSelectionOp::Loop)
        {
          plDynamicArray<plUInt32> loop;
          topology.GetEdgeLoop(h, loop);
          for (plUInt32 l : loop)
            result.Insert(MakeEdgeKey(topology.GetHalfEdge(l).m_uiFrom, topology.GetHalfEdge(l).m_uiTo));
        }
        else
        {
          plDynamicArray<plMeshTopology::RingEdge> ring;
          plDynamicArray<plUInt32> quads;
          bool bClosed = false;
          topology.GetEdgeRing(h, ring, quads, bClosed);
          for (const auto& r : ring)
            result.Insert(MakeEdgeKey(r.m_uiA, r.m_uiB));
        }
      }

      m_SelectedEdges = result;
      break;
    }

    case plBrushSelectionOp::SameMaterial:
    {
      if (m_SubMode != plBrushSubMode::Face)
        break;

      // slots as rendered: authored mesh plus face overrides (CSG faces: the cut result, which has them)
      plEditableMesh withAttributes = UsesSurfaces() ? GetSurfaceMesh() : mesh;

      if (!UsesSurfaces())
      {
        plBrushFaceAttributeMap attributes;
        plBrushPicking::ReadFaceAttributes(GetBrushObject(), attributes);
        plBrushFaceAttributeCodec::Apply(attributes, withAttributes);
      }

      plHashSet<plUInt32> usedSlots;
      for (const plBrushFace& face : withAttributes.m_Faces)
        if (m_SelectedFaces.Contains(face.m_uiFaceId))
          usedSlots.Insert(face.m_uiMaterialSlot);

      for (const plBrushFace& face : withAttributes.m_Faces)
        if (usedSlots.Contains(face.m_uiMaterialSlot))
          m_SelectedFaces.Insert(face.m_uiFaceId);
      break;
    }
  }

  RepositionGizmos();
  UpdateOverlays();
  NotifySelectionChanged();

  if (GetOwnerWindow())
    UpdateStatusBarText(GetOwnerWindow());
}

void plBrushToolContext::SetSelection(const plHashSet<plUInt32>& vertices, const plHashSet<plUInt64>& edges, const plHashSet<plUInt32>& faces)
{
  if (m_SubMode == plBrushSubMode::Object || !RebuildBaseMesh())
    return;

  m_SelectedVertices.Clear();
  m_SelectedEdges.Clear();
  m_SelectedFaces = faces;

  for (plUInt32 v : vertices)
  {
    if (v < m_BaseMesh.m_Vertices.GetCount())
      m_SelectedVertices.Insert(v);
  }

  for (plUInt64 e : edges)
  {
    const plUInt32 v0 = static_cast<plUInt32>(e >> 32);
    const plUInt32 v1 = static_cast<plUInt32>(e & 0xFFFFFFFF);

    if (v0 < m_BaseMesh.m_Vertices.GetCount() && v1 < m_BaseMesh.m_Vertices.GetCount())
      m_SelectedEdges.Insert(MakeEdgeKey(v0, v1));
  }

  RepositionGizmos();
  UpdateOverlays();
  NotifySelectionChanged();

  if (GetOwnerWindow())
    UpdateStatusBarText(GetOwnerWindow());
}

//////////////////////////////////////////////////////////////////////////
// projection, marquee, snapping

bool plBrushToolContext::ProjectToView(const plVec3& vWorld, plVec2& out_vScreen) const
{
  plEngineViewWidget* pView = GetOwnerView();
  if (pView == nullptr)
    return false;

  // logical units, as the positions of input events
  const plCamera& camera = pView->m_pViewConfig->m_Camera;
  const plVec2 vViewport = pView->GetViewportSize();
  const float fWidth = plMath::Max(vViewport.x, 1.0f);
  const float fHeight = plMath::Max(vViewport.y, 1.0f);

  plMat4 mProj;
  camera.GetProjectionMatrix(fWidth / fHeight, mProj);
  const plMat4 mViewProj = mProj * camera.GetViewMatrix();

  plVec3 vScreen;
  if (plGraphicsUtils::ConvertWorldPosToScreenPos(mViewProj, 0, 0, static_cast<plUInt32>(fWidth), static_cast<plUInt32>(fHeight), vWorld, vScreen).Failed())
    return false;

  // behind the camera
  if (vScreen.z < 0.0f)
    return false;

  out_vScreen.Set(vScreen.x, fHeight - vScreen.y);
  return true;
}

bool plBrushToolContext::IsElementFacingCamera(const plEditableMesh& mesh, plUInt32 uiElement) const
{
  if (GetOwnerView() == nullptr)
    return true;

  const plCamera& camera = GetOwnerView()->m_pViewConfig->m_Camera;
  const plVec3 vCamPos = camera.GetPosition();
  const bool bOrtho = camera.IsOrthographic();

  auto faceFaces = [&](plUInt32 uiFace)
  {
    const plVec3 vNormal = m_ObjectTransform.TransformDirection(mesh.ComputeFaceNormal(uiFace));
    const plVec3 vToCamera = bOrtho ? -camera.GetDirForwards() : vCamPos - m_ObjectTransform.TransformPosition(mesh.ComputeFaceCenter(uiFace));
    return vNormal.Dot(vToCamera) > 0.0f;
  };

  switch (m_SubMode)
  {
    case plBrushSubMode::Face:
      return faceFaces(uiElement);

    case plBrushSubMode::Vertex:
    case plBrushSubMode::Edge:
    {
      // visible when any face touching it faces the camera
      const plUInt32 v0 = (m_SubMode == plBrushSubMode::Vertex) ? uiElement : m_CurrentEdges[uiElement * 2];
      const plUInt32 v1 = (m_SubMode == plBrushSubMode::Vertex) ? uiElement : m_CurrentEdges[uiElement * 2 + 1];

      for (plUInt32 f = 0; f < mesh.m_Faces.GetCount(); ++f)
      {
        const auto& corners = mesh.m_Faces[f].m_Corners;
        if (corners.Contains(v0) && corners.Contains(v1) && faceFaces(f))
          return true;
      }
      return false;
    }

    default:
      return true;
  }
}

bool plBrushToolContext::FindSnapVertex(const plVec2I32& vViewPos, plVec3& out_vWorld, float fMaxPixels) const
{
  float fBest = fMaxPixels * fMaxPixels;
  bool bFound = false;
  const plVec2 vCursor(static_cast<float>(vViewPos.x), static_cast<float>(vViewPos.y));

  for (const plVec3& v : m_SnapVertices)
  {
    plVec2 vScreen;
    if (!ProjectToView(v, vScreen))
      continue;

    const float fDist = (vScreen - vCursor).GetLengthSquared();
    if (fDist < fBest)
    {
      fBest = fDist;
      out_vWorld = v;
      bFound = true;
    }
  }

  return bFound;
}

void plBrushToolContext::UpdateMarquee(const plVec2I32& vCurrentPos)
{
  plEngineViewWidget* pView = GetOwnerView();
  const plCamera& camera = pView->m_pViewConfig->m_Camera;
  const plVec2 vViewport = pView->GetViewportSize().CompMax(plVec2(1.0f));

  plMat4 mProj;
  camera.GetProjectionMatrix(vViewport.x / vViewport.y, mProj);
  plMat4 mInvViewProj = mProj * camera.GetViewMatrix();
  mInvViewProj.Invert(0.0f).IgnoreResult();

  // the same near-plane rectangle the scene's selection marquee draws
  const plVec3 vScreen0(static_cast<float>(vCurrentPos.x), vViewport.y - vCurrentPos.y, 0.01f);
  const plVec3 vScreen1(static_cast<float>(m_vPressPos.x), vViewport.y - m_vPressPos.y, 0.01f);

  plVec3 vNear0, vNear1, vDir0, vDir1;
  plGraphicsUtils::ConvertScreenPosToWorldPos(mInvViewProj, 0, 0, static_cast<plUInt32>(vViewport.x), static_cast<plUInt32>(vViewport.y), vScreen0, vNear0, &vDir0).IgnoreResult();
  plGraphicsUtils::ConvertScreenPosToWorldPos(mInvViewProj, 0, 0, static_cast<plUInt32>(vViewport.x), static_cast<plUInt32>(vViewport.y), vScreen1, vNear1, &vDir1).IgnoreResult();

  plTransform t = plTransform::MakeIdentity();
  t.m_vPosition = plMath::Lerp(vNear0, vNear1, 0.5f);
  t.m_qRotation = plQuat::MakeFromMat3(camera.GetViewMatrix().GetRotationalPart());

  const plVec3 vBox0 = t.m_qRotation * vNear0;
  const plVec3 vBox1 = t.m_qRotation * vNear1;

  t.m_qRotation = t.m_qRotation.GetInverse();
  t.m_vScale.Set(plMath::Abs(vBox0.x - vBox1.x), plMath::Abs(vBox0.y - vBox1.y), 0.0f);

  m_hMarquee.SetTransformation(t);
  m_hMarquee.SetVisible(true);
}

void plBrushToolContext::FinishMarquee(const plVec2I32& vCurrentPos, bool bAdditive, bool bSubtract)
{
  m_hMarquee.SetVisible(false);

  const float fMinX = static_cast<float>(plMath::Min(vCurrentPos.x, m_vPressPos.x));
  const float fMaxX = static_cast<float>(plMath::Max(vCurrentPos.x, m_vPressPos.x));
  const float fMinY = static_cast<float>(plMath::Min(vCurrentPos.y, m_vPressPos.y));
  const float fMaxY = static_cast<float>(plMath::Max(vCurrentPos.y, m_vPressPos.y));

  if (!RebuildBaseMesh())
    return;

  if (!bAdditive && !bSubtract)
  {
    m_SelectedVertices.Clear();
    m_SelectedEdges.Clear();
    m_SelectedFaces.Clear();
  }

  const auto isInRect = [&](const plVec3& vLocal)
  {
    plVec2 vScreen;
    return ProjectToView(m_ObjectTransform.TransformPosition(vLocal), vScreen) && vScreen.x >= fMinX && vScreen.x <= fMaxX && vScreen.y >= fMinY && vScreen.y <= fMaxY;
  };

  if (m_SubMode == plBrushSubMode::Face)
  {
    plHashSet<plUInt32> ids;

    if (UsesSurfaces())
    {
      // a CSG face is in when its handle (on its largest piece) is
      UpdateSurfaces();

      for (plUInt32 uiSurface = 0; uiSurface < m_SurfaceRepresentative.GetCount(); ++uiSurface)
      {
        const plUInt32 uiFace = m_SurfaceRepresentative[uiSurface];

        if (isInRect(m_SurfaceMesh.ComputeFaceCenter(uiFace)) && (m_bSelectThrough || IsElementFacingCamera(m_SurfaceMesh, uiFace)))
          GatherSurfaceIds(uiSurface, ids);
      }
    }
    else
    {
      plDynamicArray<plUInt32> polygons;

      for (plUInt32 uiFace = 0; uiFace < m_BaseMesh.m_Faces.GetCount(); ++uiFace)
      {
        if (isInRect(m_BaseMesh.ComputeFaceCenter(uiFace)) && (m_bSelectThrough || IsElementFacingCamera(m_BaseMesh, uiFace)))
          polygons.PushBack(uiFace);
      }

      // removing needs no separation: selected polygons already have ids of their own
      if (bSubtract)
      {
        for (plUInt32 uiFace : polygons)
          ids.Insert(m_BaseMesh.m_Faces[uiFace].m_uiFaceId);
      }
      else
      {
        GetPolygonIds(polygons, ids);
      }
    }

    for (plUInt32 uiId : ids)
    {
      bSubtract ? (void)m_SelectedFaces.Remove(uiId) : (void)m_SelectedFaces.Insert(uiId);
    }

    RepositionGizmos();
    UpdateOverlays();
    NotifySelectionChanged();

    if (GetOwnerWindow())
      UpdateStatusBarText(GetOwnerWindow());

    return;
  }

  const plEditableMesh& mesh = m_BaseMesh;

  if (m_SubMode == plBrushSubMode::Edge)
    BuildEdgeList(mesh, m_CurrentEdges);

  const plUInt32 uiCount = (m_SubMode == plBrushSubMode::Vertex) ? mesh.m_Vertices.GetCount() : (m_SubMode == plBrushSubMode::Edge) ? m_CurrentEdges.GetCount() / 2 : mesh.m_Faces.GetCount();

  for (plUInt32 uiElement = 0; uiElement < uiCount; ++uiElement)
  {
    plVec3 vLocal;
    switch (m_SubMode)
    {
      case plBrushSubMode::Vertex:
        vLocal = mesh.m_Vertices[uiElement];
        break;
      case plBrushSubMode::Edge:
        vLocal = plMath::Lerp(mesh.m_Vertices[m_CurrentEdges[uiElement * 2]], mesh.m_Vertices[m_CurrentEdges[uiElement * 2 + 1]], 0.5f);
        break;
      default:
        vLocal = mesh.ComputeFaceCenter(uiElement);
        break;
    }

    plVec2 vScreen;
    if (!ProjectToView(m_ObjectTransform.TransformPosition(vLocal), vScreen))
      continue;

    if (vScreen.x < fMinX || vScreen.x > fMaxX || vScreen.y < fMinY || vScreen.y > fMaxY)
      continue;

    if (!m_bSelectThrough && !IsElementFacingCamera(mesh, uiElement))
      continue;

    switch (m_SubMode)
    {
      case plBrushSubMode::Vertex:
        bSubtract ? (void)m_SelectedVertices.Remove(uiElement) : (void)m_SelectedVertices.Insert(uiElement);
        break;
      case plBrushSubMode::Edge:
      {
        const plUInt64 uiKey = MakeEdgeKey(m_CurrentEdges[uiElement * 2], m_CurrentEdges[uiElement * 2 + 1]);
        bSubtract ? (void)m_SelectedEdges.Remove(uiKey) : (void)m_SelectedEdges.Insert(uiKey);
        break;
      }
      default:
        bSubtract ? (void)m_SelectedFaces.Remove(mesh.m_Faces[uiElement].m_uiFaceId) : (void)m_SelectedFaces.Insert(mesh.m_Faces[uiElement].m_uiFaceId);
        break;
    }
  }

  RepositionGizmos();
  UpdateOverlays();
  NotifySelectionChanged();

  if (GetOwnerWindow())
    UpdateStatusBarText(GetOwnerWindow());
}

//////////////////////////////////////////////////////////////////////////
// clip tool

plStatus plBrushToolContext::StartClip()
{
  plDynamicArray<const plDocumentObject*> brushes;
  plBrushCommands::GatherSelectedBrushes(m_pDocument, brushes);

  if (brushes.IsEmpty())
    return plStatus("Clip: select the brushes to cut first.");

  if (m_SubMode != plBrushSubMode::Object)
    SetSubMode(plBrushSubMode::Object);

  m_bClipActive = true;
  m_ClipPoints.Clear();
  m_iClipKeep = plBrushCommands::ClipKeep::Front;

  if (GetOwnerWindow())
    UpdateStatusBarText(GetOwnerWindow());

  UpdateClipPreview();
  return plStatus(PL_SUCCESS);
}

void plBrushToolContext::UpdateClipPreview()
{
  plDynamicArray<plVec3> lines;

  // the points picked so far, as small crosses
  for (const plVec3& p : m_ClipPoints)
  {
    for (plVec3 d : {plVec3(0.1f, 0, 0), plVec3(0, 0.1f, 0), plVec3(0, 0, 0.1f)})
    {
      lines.PushBack(p - d);
      lines.PushBack(p + d);
    }
  }

  if (m_ClipPoints.GetCount() == 3)
  {
    plPlane plane;
    if (plane.SetFromPoints(m_ClipPoints[0], m_ClipPoints[1], m_ClipPoints[2]).Succeeded())
    {
      // the plane where it crosses the selected brushes' bounds
      plDynamicArray<const plDocumentObject*> brushes;
      plBrushCommands::GatherSelectedBrushes(m_pDocument, brushes);

      plBoundingBox bounds = plBoundingBox::MakeInvalid();
      plEditableMesh mesh;
      for (const plDocumentObject* pBrush : brushes)
      {
        if (plBrushPicking::BuildMeshFromObject(pBrush, mesh).Succeeded() && !mesh.m_Vertices.IsEmpty())
        {
          plBoundingBox b = mesh.ComputeBounds();
          b.TransformFromOrigin(plBrushPicking::GetComponentGlobalTransform(pBrush).GetAsMat4());
          bounds.ExpandToInclude(b);
        }
      }

      if (bounds.IsValid())
      {
        bounds.Grow(plVec3(0.05f));

        plVec3 corners[8];
        bounds.GetCorners(corners);
        const plUInt32 boxEdges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};

        plHybridArray<plVec3, 12> hits;
        for (const auto& e : boxEdges)
        {
          const float d0 = plane.GetDistanceTo(corners[e[0]]);
          const float d1 = plane.GetDistanceTo(corners[e[1]]);
          if ((d0 < 0.0f) != (d1 < 0.0f))
            hits.PushBack(plMath::Lerp(corners[e[0]], corners[e[1]], d0 / (d0 - d1)));
        }

        if (hits.GetCount() >= 3)
        {
          plVec3 vCenter = plVec3::MakeZero();
          for (const plVec3& h : hits)
            vCenter += h;
          vCenter /= static_cast<float>(hits.GetCount());

          // order around the centre in the plane
          const plVec3 vU = (hits[0] - vCenter).GetNormalized();
          const plVec3 vV = plane.m_vNormal.CrossRH(vU);
          hits.Sort([&](const plVec3& a, const plVec3& b)
            { return plMath::ATan2((a - vCenter).Dot(vV), (a - vCenter).Dot(vU)).GetRadian() < plMath::ATan2((b - vCenter).Dot(vV), (b - vCenter).Dot(vU)).GetRadian(); });

          for (plUInt32 i = 0; i < hits.GetCount(); ++i)
          {
            lines.PushBack(hits[i]);
            lines.PushBack(hits[(i + 1) % hits.GetCount()]);
          }

          // arrows toward the kept side(s)
          const float fArrow = 0.5f;
          if (m_iClipKeep != plBrushCommands::ClipKeep::Back)
          {
            lines.PushBack(vCenter);
            lines.PushBack(vCenter + plane.m_vNormal * fArrow);
          }
          if (m_iClipKeep != plBrushCommands::ClipKeep::Front)
          {
            lines.PushBack(vCenter);
            lines.PushBack(vCenter - plane.m_vNormal * fArrow);
          }
        }
      }
    }
  }

  if (lines.IsEmpty())
  {
    m_hClipLines.SetVisible(false);
    return;
  }

  m_hClipLines.SetLines(lines);
  m_hClipLines.SetTransformation(plTransform::MakeIdentity());
  m_hClipLines.SetVisible(true);
}

void plBrushToolContext::FinishClip(bool bApply)
{
  if (bApply && m_ClipPoints.GetCount() == 3)
  {
    plPlane plane;
    if (plane.SetFromPoints(m_ClipPoints[0], m_ClipPoints[1], m_ClipPoints[2]).Succeeded())
    {
      plDynamicArray<const plDocumentObject*> brushes;
      plBrushCommands::GatherSelectedBrushes(m_pDocument, brushes);

      const plStatus status = plBrushCommands::Clip(m_pDocument, brushes, plane, static_cast<plBrushCommands::ClipKeep::Enum>(m_iClipKeep));

      if (status.Failed() && GetOwnerWindow())
        GetOwnerWindow()->ShowTemporaryStatusBarMsg(plFmt("{}", status.m_sMessage));
    }
  }

  m_bClipActive = false;
  m_ClipPoints.Clear();
  m_hClipLines.SetVisible(false);

  if (GetOwnerWindow())
    UpdateStatusBarText(GetOwnerWindow());
}

//////////////////////////////////////////////////////////////////////////
// keys and the marking menu

bool plBrushToolContext::WantsKey(const plEditorInputEvent& e) const
{
  if (m_ModalOp != ModalOp::None || m_bClipActive)
    return true;

  // Esc ends pivot editing before anything else
  if (m_bPivotEdit && e.m_Key == plEditorPlatformKey::Escape)
    return true;

  plModelingCommand::Enum cmd;
  if (plBrushActions::MatchShortcut(e, cmd))
  {
    if (m_SubMode != plBrushSubMode::Object)
      return true;

    // in object mode only the commands that make sense without a sub-object selection take their keys
    switch (cmd)
    {
      case plModelingCommand::ModeVertex:
      case plModelingCommand::ModeEdge:
      case plModelingCommand::ModeFace:
      case plModelingCommand::MarkingMenu:
      case plModelingCommand::Clip:
      case plModelingCommand::ToggleCutter:
      case plModelingCommand::BakeCuts:
      case plModelingCommand::ToggleGridSnap:
      case plModelingCommand::Hollow:
      case plModelingCommand::Union:
      case plModelingCommand::Decompose:
      case plModelingCommand::Group:
      case plModelingCommand::EditPivot:
      case plModelingCommand::PivotToCenter:
      case plModelingCommand::PivotToBottom:
        return true;
      default:
        return false;
    }
  }

  // the transform gizmos retarget to the sub-object selection
  if (m_SubMode != plBrushSubMode::Object && e.m_uiModifiers == 0)
  {
    if (e.IsLetter('W') || e.IsLetter('E') || e.IsLetter('R') || e.IsLetter('Q'))
      return true;
  }

  return false;
}

void plBrushToolContext::OpenMarkingMenu()
{
  plEngineDocumentWindow* pWindow = GetOwnerWindow();

  if (pWindow == nullptr || m_pDocument == nullptr || pWindow->GetViewWidgets().IsEmpty())
    return;

  // the engine shell shows it in a popup at the cursor
  plModelingEngineUi::ShowShellMarkingMenu(m_pDocument, m_SubMode);
}

//////////////////////////////////////////////////////////////////////////
// input

plEditorInput plBrushToolContext::DoKeyPressEvent(const plEditorInputEvent& e)
{
  if (m_bClipActive)
  {
    switch (e.m_Key)
    {
      case plEditorPlatformKey::Escape:
        FinishClip(false);
        return plEditorInput::WasExclusivelyHandled;

      case plEditorPlatformKey::Enter:
        FinishClip(true);
        return plEditorInput::WasExclusivelyHandled;

      case plEditorPlatformKey::Tab:
        m_iClipKeep = (m_iClipKeep + 1) % 3;
        UpdateClipPreview();
        if (GetOwnerWindow())
          UpdateStatusBarText(GetOwnerWindow());
        return plEditorInput::WasExclusivelyHandled;

      case plEditorPlatformKey::Backspace:
        if (!m_ClipPoints.IsEmpty())
          m_ClipPoints.PopBack();
        UpdateClipPreview();
        if (GetOwnerWindow())
          UpdateStatusBarText(GetOwnerWindow());
        return plEditorInput::WasExclusivelyHandled;

      default:
        return plEditorInput::MayBeHandledByOthers;
    }
  }

  if (m_ModalOp != ModalOp::None)
    return HandleModalKey(e);

  // a box being drawn owns the keyboard until it is done (Esc cancels it)
  if (m_SubMode == plBrushSubMode::Object && m_pDrawGizmo != nullptr && m_pDrawGizmo->GetCurrentMode() != plDrawBoxGizmo::ManipulateMode::None)
  {
    return m_pDrawGizmo->KeyPressEvent(e);
  }

  if (m_bPivotEdit && e.m_Key == plEditorPlatformKey::Escape)
  {
    SetPivotEdit(false).IgnoreResult();
    return plEditorInput::WasExclusivelyHandled;
  }

  // every rebindable modeling command
  plModelingCommand::Enum cmd;
  if (WantsKey(e) && plBrushActions::MatchShortcut(e, cmd))
  {
    plBrushActions::Execute(m_pDocument, cmd);
    return plEditorInput::WasExclusivelyHandled;
  }

  if (m_SubMode == plBrushSubMode::Object)
  {
    return m_pDrawGizmo ? m_pDrawGizmo->KeyPressEvent(e) : plEditorInput::MayBeHandledByOthers;
  }

  // vertex / edge / face mode
  if (e.m_uiModifiers != 0)
    return plEditorInput::MayBeHandledByOthers;

  // the editor's standard gizmo keys, retargeted at the sub-object selection
  if (e.IsLetter('W'))
  {
    ShowGizmo(ActiveGizmo::Translate);
    return plEditorInput::WasExclusivelyHandled;
  }

  if (e.IsLetter('E'))
  {
    ShowGizmo(ActiveGizmo::Rotate);
    return plEditorInput::WasExclusivelyHandled;
  }

  if (e.IsLetter('R'))
  {
    ShowGizmo(ActiveGizmo::Scale);
    return plEditorInput::WasExclusivelyHandled;
  }

  if (e.IsLetter('Q'))
  {
    HideGizmos();
    return plEditorInput::WasExclusivelyHandled;
  }

  return plEditorInput::MayBeHandledByOthers;
}

plEditorInput plBrushToolContext::HandleModalKey(const plEditorInputEvent& e)
{
  const bool bTransformOp = m_ModalOp == ModalOp::Move || m_ModalOp == ModalOp::Rotate || m_ModalOp == ModalOp::Scale;
  const bool bSegmentOp = m_ModalOp == ModalOp::Bevel || m_ModalOp == ModalOp::LoopCut;

  switch (e.m_Key)
  {
    case plEditorPlatformKey::Escape:
      CancelModalOp();
      return plEditorInput::WasExclusivelyHandled;

    case plEditorPlatformKey::Enter:
      CommitModalOp();
      return plEditorInput::WasExclusivelyHandled;

    case plEditorPlatformKey::Control:
      // Ctrl snapping takes effect without waiting for the mouse to move
      UpdateModalPreview();
      return plEditorInput::WasExclusivelyHandled;

    case plEditorPlatformKey::Backspace:
      if (!m_sNumericInput.IsEmpty())
      {
        m_sNumericInput.Shrink(0, 1);
        UpdateModalPreview();
      }
      return plEditorInput::WasExclusivelyHandled;

    default:
      break;
  }

  const plUInt32 uiLetter = e.m_uiModifiers == 0 || e.m_uiModifiers == plEditorInputEvent::Shift ? e.GetLetter() : 0;

  if (uiLetter == 'V')
  {
    if (m_ModalOp == ModalOp::Move)
    {
      m_bVertexSnap = true;
      UpdateModalPreview();
    }
    return plEditorInput::WasExclusivelyHandled;
  }

  if (uiLetter == '+' || uiLetter == '=')
  {
    if (bSegmentOp)
    {
      m_uiOpSegments = plMath::Min<plUInt32>(m_uiOpSegments + 1, 16);
      UpdateModalPreview();
    }
    return plEditorInput::WasExclusivelyHandled;
  }

  if (uiLetter == '-' && bSegmentOp && m_sNumericInput.IsEmpty())
  {
    m_uiOpSegments = plMath::Max<plUInt32>(m_uiOpSegments - 1, 1);
    UpdateModalPreview();
    return plEditorInput::WasExclusivelyHandled;
  }

  if (uiLetter == 'X' || uiLetter == 'Y' || uiLetter == 'Z')
  {
    if (!bTransformOp)
      return plEditorInput::WasExclusivelyHandled;

    const AxisLock::Enum axis = uiLetter == 'X' ? AxisLock::X : uiLetter == 'Y' ? AxisLock::Y : AxisLock::Z;
    m_AxisLock = (m_AxisLock == axis) ? AxisLock::None : axis;
    UpdateModalPreview();
    return plEditorInput::WasExclusivelyHandled;
  }

  // typed numbers set the amount; the keys show their unshifted characters
  if (e.m_uiModifiers == 0 && ((uiLetter >= '0' && uiLetter <= '9') || uiLetter == '.' || uiLetter == '-'))
  {
    m_sNumericInput.Append(uiLetter);
    UpdateModalPreview();
  }

  // a running modal operation swallows all keys — document shortcuts must not fire
  return plEditorInput::WasExclusivelyHandled;
}

plEditorInput plBrushToolContext::DoKeyReleaseEvent(const plEditorInputEvent& e)
{
  if (m_ModalOp != ModalOp::None)
  {
    if (e.IsLetter('V') && m_bVertexSnap)
    {
      m_bVertexSnap = false;
      UpdateModalPreview();
    }
    else if (e.m_Key == plEditorPlatformKey::Control)
    {
      UpdateModalPreview();
    }

    return plEditorInput::WasExclusivelyHandled;
  }

  if (m_SubMode == plBrushSubMode::Object && m_pDrawGizmo != nullptr)
    return m_pDrawGizmo->KeyReleaseEvent(e);

  return plEditorInput::MayBeHandledByOthers;
}

plEditorInput plBrushToolContext::DoMousePressEvent(const plEditorInputEvent& e)
{
  const plVec2I32 vPos = e.GetPosition();

  if (m_ModalOp != ModalOp::None)
  {
    if (e.m_uiButton == plEditorInputEvent::LeftButton)
    {
      CommitModalOp();
    }
    else if (e.m_uiButton == plEditorInputEvent::RightButton)
    {
      CancelModalOp();
    }

    return plEditorInput::WasExclusivelyHandled;
  }

  if (m_bClipActive)
  {
    if (e.HasModifier(plEditorInputEvent::Alt) || e.m_uiButton != plEditorInputEvent::LeftButton)
      return plEditorInput::MayBeHandledByOthers;

    // points land on surfaces; in empty space, on the camera-facing plane through the last point
    const plObjectPickingResult& res = GetOwnerView()->PickObject(static_cast<plUInt16>(vPos.x), static_cast<plUInt16>(vPos.y));
    plVec3 vPoint = res.m_vPickedPosition;

    if (!res.m_PickedObject.IsValid() || vPoint.IsNaN())
    {
      const plCamera& camera = GetOwnerView()->m_pViewConfig->m_Camera;
      const plVec3 vAnchor = m_ClipPoints.IsEmpty() ? camera.GetCenterPosition() + camera.GetDirForwards() * 5.0f : m_ClipPoints.PeekBack();
      const plPlane plane = plPlane::MakeFromNormalAndPoint(camera.GetDirForwards(), vAnchor);

      if (GetOwnerView()->PickPlane(static_cast<plUInt16>(vPos.x), static_cast<plUInt16>(vPos.y), plane, vPoint).Failed())
        return plEditorInput::WasExclusivelyHandled;
    }

    const plVec3 vSnapped = plBrushToolSettings::GetSnapToGrid() ? SnapToGrid(vPoint) : vPoint;

    if (m_ClipPoints.GetCount() < 3)
      m_ClipPoints.PushBack(vSnapped);

    // in an orthographic view two points and the view direction define the plane
    const plCamera& camera = GetOwnerView()->m_pViewConfig->m_Camera;
    if (m_ClipPoints.GetCount() == 2 && camera.IsOrthographic())
      m_ClipPoints.PushBack(m_ClipPoints[0] + camera.GetDirForwards());

    UpdateClipPreview();

    if (GetOwnerWindow())
      UpdateStatusBarText(GetOwnerWindow());

    return plEditorInput::WasExclusivelyHandled;
  }

  // the pivot gizmo's handles come before drawing (Ctrl+drag) and selection, so Ctrl can snap the drag
  if (m_bPivotEdit && e.m_uiButton == plEditorInputEvent::LeftButton && !e.HasModifier(plEditorInputEvent::Alt))
  {
    plEditorInput result;
    if (PressActiveGizmoHandle(e, GetOwnerView()->PickObject(static_cast<plUInt16>(vPos.x), static_cast<plUInt16>(vPos.y)), result))
      return result;
  }

  if (m_SubMode == plBrushSubMode::Object)
  {
    return m_pDrawGizmo ? m_pDrawGizmo->MousePressEvent(e) : plEditorInput::MayBeHandledByOthers;
  }

  // camera navigation always wins
  if (e.HasModifier(plEditorInputEvent::Alt))
    return plEditorInput::MayBeHandledByOthers;

  if (e.m_uiButton == plEditorInputEvent::LeftButton)
  {
    const plObjectPickingResult& res = GetOwnerView()->PickObject(static_cast<plUInt16>(vPos.x), static_cast<plUInt16>(vPos.y));

    // a click on an active transform-gizmo handle belongs to the gizmo, not to selection
    plEditorInput gizmoResult;
    if (PressActiveGizmoHandle(e, res, gizmoResult))
      return gizmoResult;

    // a click selects on release; dragging first turns it into a marquee
    m_bPressPending = true;
    m_bMarqueeActive = false;
    m_vPressPos = vPos;
    m_PressHandle = m_HandleToElement.Contains(res.m_PickedOther) ? res.m_PickedOther : plUuid();

    return plEditorInput::WasExclusivelyHandled;
  }

  return plEditorInput::MayBeHandledByOthers;
}

plEditorInput plBrushToolContext::DoMouseReleaseEvent(const plEditorInputEvent& e)
{
  if (m_ModalOp != ModalOp::None)
    return plEditorInput::WasExclusivelyHandled;

  if (m_bClipActive)
    return (e.m_uiButton == plEditorInputEvent::LeftButton) ? plEditorInput::WasExclusivelyHandled : plEditorInput::MayBeHandledByOthers;

  if (m_SubMode == plBrushSubMode::Object)
  {
    const plEditorInput result = m_pDrawGizmo ? m_pDrawGizmo->MouseReleaseEvent(e) : plEditorInput::MayBeHandledByOthers;
    m_sInfoText.Clear();
    return result;
  }

  if (e.m_uiButton == plEditorInputEvent::LeftButton && m_bPressPending)
  {
    m_bPressPending = false;

    const bool bShift = e.HasModifier(plEditorInputEvent::Shift);
    const bool bCtrl = e.HasModifier(plEditorInputEvent::Control);

    if (m_bMarqueeActive)
    {
      m_bMarqueeActive = false;
      FinishMarquee(e.GetPosition(), bShift, bCtrl);
      return plEditorInput::WasExclusivelyHandled;
    }

    HoverHit hit;
    plUInt32 uiElement = 0;

    // an element cube selects its element exactly; otherwise the nearest element on the clicked face
    if (!(m_PressHandle.IsValid() && m_HandleToElement.TryGetValue(m_PressHandle, uiElement) && MakeHoverFromElement(uiElement, hit)))
    {
      PickAt(m_vPressPos, hit);
    }

    ApplyClickSelection(hit, bShift);
    RepositionGizmos();
    UpdateOverlays();
    NotifySelectionChanged();

    if (GetOwnerWindow())
    {
      UpdateStatusBarText(GetOwnerWindow());
    }

    return plEditorInput::WasExclusivelyHandled;
  }

  if (e.m_uiButton == plEditorInputEvent::LeftButton)
    return plEditorInput::WasExclusivelyHandled;

  return plEditorInput::MayBeHandledByOthers;
}

plEditorInput plBrushToolContext::DoWheelEvent(const plEditorInputEvent& e)
{
  if (m_ModalOp == ModalOp::Bevel || m_ModalOp == ModalOp::LoopCut)
  {
    if (e.m_fWheel > 0.0f)
      m_uiOpSegments = plMath::Min<plUInt32>(m_uiOpSegments + 1, 16);
    else if (e.m_fWheel < 0.0f)
      m_uiOpSegments = plMath::Max<plUInt32>(m_uiOpSegments - 1, 1);

    UpdateModalPreview();
    return plEditorInput::WasExclusivelyHandled;
  }

  return plEditorInput::MayBeHandledByOthers;
}

plEditorInput plBrushToolContext::DoMouseMoveEvent(const plEditorInputEvent& e)
{
  const plVec2I32 vPos = e.GetPosition();

  if (m_ModalOp != ModalOp::None)
  {
    m_vLastMouse = vPos;
    UpdateModalPreview();
    return plEditorInput::WasExclusivelyHandled;
  }

  if (m_bClipActive)
    return plEditorInput::MayBeHandledByOthers;

  // the pivot gizmo follows the selection, and undo, while the mouse is free
  if (m_bPivotEdit && e.m_uiButtons == plEditorInputEvent::NoButton)
  {
    UpdatePivotGizmo();
  }

  if (m_SubMode == plBrushSubMode::Object)
  {
    if (m_bPivotEdit && e.m_uiButtons == plEditorInputEvent::NoButton)
    {
      const plObjectPickingResult& res = GetOwnerView()->PickObject(static_cast<plUInt16>(vPos.x), static_cast<plUInt16>(vPos.y));
      HighlightHandle(FindActiveGizmoHandle(res) ? res.m_PickedOther : plUuid());
    }

    const plEditorInput result = m_pDrawGizmo ? m_pDrawGizmo->MouseMoveEvent(e) : plEditorInput::MayBeHandledByOthers;

    // live size readout while a box is drawn
    if (m_pDrawGizmo && m_pDrawGizmo->GetCurrentMode() != plDrawBoxGizmo::ManipulateMode::None)
    {
      plVec3 vOrigin;
      float nx, px, ny, py, nz, pz;
      m_pDrawGizmo->GetResult(vOrigin, nx, px, ny, py, nz, pz);
      m_sInfoText.SetFormat("W {} m   D {} m   H {} m", plArgF(nx + px, 2), plArgF(ny + py, 2), plArgF(nz + pz, 2));
    }
    else
    {
      m_sInfoText.Clear();
    }

    return result;
  }

  // a left drag that started on the brush becomes a marquee
  if (m_bPressPending && e.IsHeld(plEditorInputEvent::LeftButton))
  {
    if (!m_bMarqueeActive && plMath::Abs(vPos.x - m_vPressPos.x) + plMath::Abs(vPos.y - m_vPressPos.y) > 4)
      m_bMarqueeActive = true;

    if (m_bMarqueeActive)
      UpdateMarquee(vPos);

    return plEditorInput::WasExclusivelyHandled;
  }

  // let camera drags through
  if (e.m_uiButtons != plEditorInputEvent::NoButton)
    return plEditorInput::MayBeHandledByOthers;

  // transform-gizmo handles and element cubes both highlight through the engine's highlight system
  {
    const plObjectPickingResult& res = GetOwnerView()->PickObject(static_cast<plUInt16>(vPos.x), static_cast<plUInt16>(vPos.y));

    plUuid hoveredHandle;
    const bool bOverTransformGizmo = FindActiveGizmoHandle(res) != nullptr;

    if (bOverTransformGizmo || m_HandleToElement.Contains(res.m_PickedOther))
    {
      hoveredHandle = res.m_PickedOther;
    }

    HighlightHandle(hoveredHandle);

    if (bOverTransformGizmo)
    {
      // the cursor is on the gizmo — no brush hover underneath it
      if (m_Hover.IsValid())
      {
        m_Hover.Reset();
        UpdateOverlays();
      }

      return plEditorInput::WasExclusivelyHandled;
    }

    // hover feedback is the engine highlight on the cube alone — no lines until something is selected
  }

  m_Hover.Reset();

  return plEditorInput::WasExclusivelyHandled;
}

//////////////////////////////////////////////////////////////////////////
// overlays

static plColor GetElementIdleColor()
{
  return plColorScheme::LightUI(plColorScheme::Gray);
}

static plColor GetElementSelectedColor()
{
  return plColorScheme::LightUI(plColorScheme::PlasmaBranding);
}

void plBrushToolContext::BuildEdgeList(const plEditableMesh& mesh, plDynamicArray<plUInt32>& out_edgePairs) const
{
  // deterministic order (face traversal), each unique edge once — element index = pair index
  out_edgePairs.Clear();

  plHashSet<plUInt64> seen;

  for (const plBrushFace& face : mesh.m_Faces)
  {
    for (plUInt32 i = 0; i < face.m_Corners.GetCount(); ++i)
    {
      const plUInt32 uiV0 = face.m_Corners[i];
      const plUInt32 uiV1 = face.m_Corners[(i + 1) % face.m_Corners.GetCount()];

      if (!seen.Insert(MakeEdgeKey(uiV0, uiV1)))
      {
        out_edgePairs.PushBack(uiV0);
        out_edgePairs.PushBack(uiV1);
      }
    }
  }
}

plInt32 plBrushToolContext::FindFaceUsingVertex(const plEditableMesh& mesh, plUInt32 uiVertex) const
{
  for (plUInt32 uiFace = 0; uiFace < mesh.m_Faces.GetCount(); ++uiFace)
  {
    for (plUInt32 uiCorner : mesh.m_Faces[uiFace].m_Corners)
    {
      if (uiCorner == uiVertex)
        return static_cast<plInt32>(uiFace);
    }
  }

  return -1;
}

bool plBrushToolContext::MakeHoverFromElement(plUInt32 uiElement, HoverHit& out_hit) const
{
  const plEditableMesh& mesh = GetDisplayMesh();

  out_hit.Reset();

  switch (m_SubMode)
  {
    case plBrushSubMode::Vertex:
      if (uiElement >= mesh.m_Vertices.GetCount())
        return false;
      out_hit.m_iVertex = static_cast<plInt32>(uiElement);
      out_hit.m_iFace = FindFaceUsingVertex(mesh, uiElement);
      return out_hit.m_iFace >= 0;

    case plBrushSubMode::Edge:
      if (uiElement * 2 + 1 >= m_CurrentEdges.GetCount())
        return false;
      out_hit.m_iEdgeV0 = static_cast<plInt32>(m_CurrentEdges[uiElement * 2]);
      out_hit.m_iEdgeV1 = static_cast<plInt32>(m_CurrentEdges[uiElement * 2 + 1]);
      out_hit.m_iFace = FindFaceUsingVertex(mesh, m_CurrentEdges[uiElement * 2]);
      return out_hit.m_iFace >= 0;

    case plBrushSubMode::Face:
      if (UsesSurfaces())
      {
        // CSG faces: one handle per surface, the hit is its largest piece in the surface mesh
        UpdateSurfaces();
        if (uiElement >= m_SurfaceRepresentative.GetCount())
          return false;
        out_hit.m_iFace = static_cast<plInt32>(m_SurfaceRepresentative[uiElement]);
        return true;
      }
      if (uiElement >= mesh.m_Faces.GetCount())
        return false;
      out_hit.m_iFace = static_cast<plInt32>(uiElement);
      return true;

    default:
      return false;
  }
}

void plBrushToolContext::UpdateElementHandles()
{
  // cubes get expensive to sync on dense meshes — beyond the cap the wireframe carries the mode alone
  constexpr plUInt32 uiMaxHandles = 256;

  const plEditableMesh& mesh = GetDisplayMesh();

  plUInt32 uiElementCount = 0;

  switch (m_SubMode)
  {
    case plBrushSubMode::Vertex:
      uiElementCount = mesh.m_Vertices.GetCount();
      break;
    case plBrushSubMode::Edge:
      BuildEdgeList(mesh, m_CurrentEdges);
      uiElementCount = m_CurrentEdges.GetCount() / 2;
      break;
    case plBrushSubMode::Face:
      if (UsesSurfaces())
      {
        UpdateSurfaces();
        uiElementCount = m_SurfaceRepresentative.GetCount();
      }
      else
      {
        uiElementCount = mesh.m_Faces.GetCount();
      }
      break;
    default:
      break;
  }

  if (uiElementCount > uiMaxHandles)
  {
    HideElementHandles();
    return;
  }

  auto* pDocument = GetOwnerWindow() ? GetOwnerWindow()->GetDocument() : nullptr;

  if (pDocument == nullptr)
    return;

  // grow the pool; the guid map only changes when a handle is created
  while (m_ElementHandles.GetCount() < uiElementCount)
  {
    plUniquePtr<plEngineGizmoHandle> pHandle = PL_DEFAULT_NEW(plEngineGizmoHandle);
    pHandle->ConfigureHandle(this, plEngineGizmoHandleType::Box, GetElementIdleColor(),
      plGizmoFlags::ConstantSize | plGizmoFlags::OnTop | plGizmoFlags::ShowInOrtho | plGizmoFlags::Pickable);

    pDocument->AddSyncObject(pHandle.Borrow());

    m_HandleToElement[pHandle->GetGuid()] = m_ElementHandles.GetCount();
    m_ElementHandles.PushBack(std::move(pHandle));
  }

  // Gizmos draw over the scene, so occlusion is tested here instead: a handle is hidden (and so not
  // pickable) when the brush's rendered surface lies between it and the camera. The test runs in brush
  // space, where the camera-to-handle segment keeps its parameterisation under the affine transform.
  plDynamicArray<plVec3> occluders;
  plVec3 vLocalCamera = plVec3::MakeZero();
  plVec3 vLocalForward = plVec3::MakeZero();
  bool bOrtho = false;

  if (!m_bShowHiddenHandles && GetOwnerView() != nullptr)
  {
    const plCamera& camera = GetOwnerView()->m_pViewConfig->m_Camera;
    const plTransform invObject = m_ObjectTransform.GetInverse();

    bOrtho = camera.IsOrthographic();
    vLocalCamera = invObject.TransformPosition(camera.GetPosition());
    vLocalForward = invObject.TransformDirection(camera.GetDirForwards());
    m_vHandleCameraPos = camera.GetPosition();
    m_vHandleCameraDir = camera.GetDirForwards();

    const plEditableMesh& occluderMesh = (m_bCarved && &mesh == &m_BaseMesh && !m_CarvedMesh.m_Faces.IsEmpty()) ? m_CarvedMesh : mesh;
    plHybridArray<plUInt32, 32> tris;

    for (plUInt32 uiFace = 0; uiFace < occluderMesh.m_Faces.GetCount(); ++uiFace)
    {
      const plBrushFace& face = occluderMesh.m_Faces[uiFace];

      if ((face.m_uiFlags & plBrushFaceFlags::Internal) != 0)
        continue;

      occluderMesh.TriangulateFace(uiFace, tris);

      for (plUInt32 uiCorner : tris)
      {
        occluders.PushBack(occluderMesh.m_Vertices[face.m_Corners[uiCorner]]);
      }
    }
  }

  auto isOccluded = [&](const plVec3& vLocalPos)
  {
    const plVec3 vStart = bOrtho ? vLocalPos - vLocalForward * 1000.0f : vLocalCamera;
    const plVec3 vDir = vLocalPos - vStart;

    // the handle sits on the surface: hits within a centimetre of it are its own faces
    const float fLength = vDir.GetLength();
    if (fLength <= 0.01f)
      return false;

    const float fMaxTime = 1.0f - 0.01f / fLength;

    for (plUInt32 i = 0; i + 2 < occluders.GetCount(); i += 3)
    {
      float fTime = 0.0f;
      if (plIntersectionUtils::RayTriangleIntersection(vStart, vDir, occluders[i], occluders[i + 1], occluders[i + 2], &fTime) && fTime > 0.0f && fTime < fMaxTime)
        return true;
    }

    return false;
  };

  for (plUInt32 uiElement = 0; uiElement < m_ElementHandles.GetCount(); ++uiElement)
  {
    plEngineGizmoHandle* pHandle = m_ElementHandles[uiElement].Borrow();

    if (uiElement >= uiElementCount)
    {
      pHandle->SetVisible(false);
      continue;
    }

    plVec3 vLocalPos = plVec3::MakeZero();
    bool bSelected = false;

    switch (m_SubMode)
    {
      case plBrushSubMode::Vertex:
        vLocalPos = mesh.m_Vertices[uiElement];
        bSelected = m_SelectedVertices.Contains(uiElement);
        break;

      case plBrushSubMode::Edge:
      {
        const plUInt32 uiV0 = m_CurrentEdges[uiElement * 2];
        const plUInt32 uiV1 = m_CurrentEdges[uiElement * 2 + 1];
        vLocalPos = plMath::Lerp(mesh.m_Vertices[uiV0], mesh.m_Vertices[uiV1], 0.5f);
        bSelected = m_SelectedEdges.Contains(MakeEdgeKey(uiV0, uiV1));
        break;
      }

      case plBrushSubMode::Face:
        if (UsesSurfaces())
        {
          const plUInt32 uiFace = m_SurfaceRepresentative[uiElement];
          vLocalPos = m_SurfaceMesh.ComputeFaceCenter(uiFace);
          bSelected = m_SelectedFaces.Contains(m_SurfaceMesh.m_Faces[uiFace].m_uiFaceId);
        }
        else
        {
          vLocalPos = mesh.ComputeFaceCenter(uiElement);
          bSelected = m_SelectedFaces.Contains(mesh.m_Faces[uiElement].m_uiFaceId);
        }
        break;

      default:
        break;
    }

    if (!occluders.IsEmpty() && isOccluded(vLocalPos))
    {
      pHandle->SetVisible(false);
      continue;
    }

    // the manipulator-nob look: solid constant-size cube, sized a touch below theirs
    plTransform t(m_ObjectTransform.TransformPosition(vLocalPos));
    t.m_vScale.Set(0.1f);

    pHandle->SetTransformation(t);
    pHandle->SetColor(bSelected ? GetElementSelectedColor() : GetElementIdleColor());
    pHandle->SetVisible(true);
  }
}

void plBrushToolContext::SetSelectThrough(bool b)
{
  if (m_bSelectThrough == b)
    return;

  m_bSelectThrough = b;

  plBrushModeEvent e;
  e.m_pContext = this;
  e.m_pDocument = m_pDocument;
  e.m_Mode = m_SubMode;
  s_ModeEvents.Broadcast(e);
}

void plBrushToolContext::SetShowHiddenHandles(bool b)
{
  if (m_bShowHiddenHandles == b)
    return;

  m_bShowHiddenHandles = b;

  plBrushModeEvent e;
  e.m_pContext = this;
  e.m_pDocument = m_pDocument;
  e.m_Mode = m_SubMode;
  s_ModeEvents.Broadcast(e);

  UpdateOverlays();
}

void plBrushToolContext::UpdateForCamera()
{
  if (m_bShowHiddenHandles || m_SubMode == plBrushSubMode::Object || GetOwnerView() == nullptr)
    return;

  const plCamera& camera = GetOwnerView()->m_pViewConfig->m_Camera;

  if (camera.GetPosition().IsEqual(m_vHandleCameraPos, 0.001f) && camera.GetDirForwards().IsEqual(m_vHandleCameraDir, 0.0001f))
    return;

  UpdateElementHandles();
}

void plBrushToolContext::HideElementHandles()
{
  for (auto& pHandle : m_ElementHandles)
  {
    pHandle->SetVisible(false);
  }
}

void plBrushToolContext::AppendSelectionBorder(plDynamicArray<plVec3>& ref_lines, const plEditableMesh& mesh, const plHashSet<plUInt32>& faceIds)
{
  // edges matched by position, since CSG pieces sit in separate shells: an edge two selected faces share
  // lies inside the selection and is left out, so a face cut into pieces is outlined as one
  const auto positionKey = [&](plUInt32 uiVertex)
  {
    const plVec3& v = mesh.m_Vertices[uiVertex];
    const plInt64 key[3] = {static_cast<plInt64>(plMath::Round(v.x * 10000.0f)), static_cast<plInt64>(plMath::Round(v.y * 10000.0f)), static_cast<plInt64>(plMath::Round(v.z * 10000.0f))};
    return plHashingUtils::xxHash64(key, sizeof(key));
  };

  const auto edgeKey = [&](plUInt32 uiV0, plUInt32 uiV1)
  {
    const plUInt64 k0 = positionKey(uiV0);
    const plUInt64 k1 = positionKey(uiV1);
    const plUInt64 key[2] = {plMath::Min(k0, k1), plMath::Max(k0, k1)};
    return plHashingUtils::xxHash64(key, sizeof(key));
  };

  plHashTable<plUInt64, plUInt32> edgeUses;

  for (const plBrushFace& face : mesh.m_Faces)
  {
    if (!faceIds.Contains(face.m_uiFaceId))
      continue;

    for (plUInt32 i = 0; i < face.m_Corners.GetCount(); ++i)
      ++edgeUses[edgeKey(face.m_Corners[i], face.m_Corners[(i + 1) % face.m_Corners.GetCount()])];
  }

  for (const plBrushFace& face : mesh.m_Faces)
  {
    if (!faceIds.Contains(face.m_uiFaceId))
      continue;

    for (plUInt32 i = 0; i < face.m_Corners.GetCount(); ++i)
    {
      const plUInt32 uiV0 = face.m_Corners[i];
      const plUInt32 uiV1 = face.m_Corners[(i + 1) % face.m_Corners.GetCount()];

      if (edgeUses[edgeKey(uiV0, uiV1)] == 1)
      {
        ref_lines.PushBack(mesh.m_Vertices[uiV0]);
        ref_lines.PushBack(mesh.m_Vertices[uiV1]);
      }
    }
  }
}

void plBrushToolContext::AppendFaceWireframe(plDynamicArray<plVec3>& ref_lines, const plEditableMesh& mesh, plUInt32 uiFace)
{
  const plBrushFace& face = mesh.m_Faces[uiFace];

  plHybridArray<plUInt32, 32> tris;
  mesh.TriangulateFace(uiFace, tris);

  for (plUInt32 t = 0; t + 2 < tris.GetCount(); t += 3)
  {
    for (plUInt32 e = 0; e < 3; ++e)
    {
      ref_lines.PushBack(mesh.m_Vertices[face.m_Corners[tris[t + e]]]);
      ref_lines.PushBack(mesh.m_Vertices[face.m_Corners[tris[t + (e + 1) % 3]]]);
    }
  }
}

void plBrushToolContext::ClearOverlays()
{
  m_hCandidateLines.SetVisible(false);
  m_hHoverLines.SetVisible(false);
  m_hSelectionLines.SetVisible(false);
  m_hFaceWireLines.SetVisible(false);

  HideElementHandles();
}

void plBrushToolContext::UpdateOverlays()
{
  if (m_SubMode == plBrushSubMode::Object)
  {
    ClearOverlays();
    return;
  }

  const plEditableMesh& mesh = GetDisplayMesh();

  // the cubes ARE the idle UI: grey = selectable, engine highlight = hovered, brand = selected.
  // Lines appear only for selected elements, so the viewport stays clean until a choice is made.
  UpdateElementHandles();

  // a carved brush renders its cut result, but the handles edit the uncut shape: draw that shape as a cage
  if (m_bCarved)
  {
    plDynamicArray<plUInt32> edgePairs;
    BuildEdgeList(mesh, edgePairs);

    plDynamicArray<plVec3> cage;
    for (plUInt32 i = 0; i + 1 < edgePairs.GetCount(); i += 2)
    {
      cage.PushBack(mesh.m_Vertices[edgePairs[i]]);
      cage.PushBack(mesh.m_Vertices[edgePairs[i + 1]]);
    }

    m_hCandidateLines.SetLines(cage);
    m_hCandidateLines.SetTransformation(m_ObjectTransform);
    m_hCandidateLines.SetVisible(!cage.IsEmpty());
  }
  else
  {
    m_hCandidateLines.SetVisible(false);
  }

  m_hHoverLines.SetVisible(false);

  // selection accents: selected edges as brand lines, selected faces as their outline (their triangles
  // are drawn on top, below)
  plDynamicArray<plVec3> lines;

  for (plUInt64 uiEdge : m_SelectedEdges)
  {
    const plUInt32 uiV0 = static_cast<plUInt32>(uiEdge >> 32);
    const plUInt32 uiV1 = static_cast<plUInt32>(uiEdge & 0xFFFFFFFF);

    if (uiV0 < mesh.m_Vertices.GetCount() && uiV1 < mesh.m_Vertices.GetCount())
    {
      lines.PushBack(mesh.m_Vertices[uiV0]);
      lines.PushBack(mesh.m_Vertices[uiV1]);
    }
  }

  // CSG faces are selected (and so drawn) on the rendered mesh, mesh faces on the authored one
  const bool bSurfaces = UsesSurfaces();
  const plEditableMesh& faceMesh = bSurfaces ? GetSurfaceMesh() : mesh;

  if (!m_SelectedFaces.IsEmpty())
  {
    AppendSelectionBorder(lines, faceMesh, m_SelectedFaces);
  }

  if (lines.IsEmpty())
  {
    m_hSelectionLines.SetVisible(false);
  }
  else
  {
    m_hSelectionLines.SetLines(lines);
    m_hSelectionLines.SetTransformation(m_ObjectTransform);
    m_hSelectionLines.SetVisible(true);
  }

  // the selected faces' triangles as rendered: the cut pieces of a carved brush (they keep the id of the
  // face they were cut from), the live preview during a modal operation
  plDynamicArray<plVec3> wire;
  const plEditableMesh& wireMesh = bSurfaces ? faceMesh : (m_bCarved && &mesh == &m_BaseMesh && !m_CarvedMesh.m_Faces.IsEmpty()) ? m_CarvedMesh : mesh;

  if (!m_SelectedFaces.IsEmpty())
  {
    for (plUInt32 uiFace = 0; uiFace < wireMesh.m_Faces.GetCount(); ++uiFace)
    {
      if (m_SelectedFaces.Contains(wireMesh.m_Faces[uiFace].m_uiFaceId))
      {
        AppendFaceWireframe(wire, wireMesh, uiFace);
      }
    }
  }

  if (wire.IsEmpty())
  {
    m_hFaceWireLines.SetVisible(false);
  }
  else
  {
    m_hFaceWireLines.SetLines(wire);
    m_hFaceWireLines.SetTransformation(m_ObjectTransform);
    m_hFaceWireLines.SetVisible(true);
  }
}

//////////////////////////////////////////////////////////////////////////
// status bar

void plBrushToolContext::UpdateStatusBarText(plEngineDocumentWindow* pWindow)
{
  if (m_bClipActive)
  {
    const char* szKeep[] = {"front (arrow side)", "back", "both halves"};

    if (m_ClipPoints.GetCount() < 3)
      pWindow->SetPermanentStatusBarMsg(plFmt("Clip: click point {} of 3 on a surface - Backspace removes a point, ESC cancels.", m_ClipPoints.GetCount() + 1));
    else
      pWindow->SetPermanentStatusBarMsg(plFmt("Clip: keeping the {} - Tab changes, Enter applies, Backspace moves the last point, ESC cancels.", szKeep[m_iClipKeep]));
    return;
  }

  if (m_ModalOp >= ModalOp::Extrude)
  {
    pWindow->SetPermanentStatusBarMsg(plFmt("Brush - {}: drag or type a value, Enter/click to commit, ESC/right-click to cancel.", m_sInfoText));
    return;
  }

  if (m_ModalOp != ModalOp::None)
  {
    const char* szOp = (m_ModalOp == ModalOp::Move) ? "Move" : (m_ModalOp == ModalOp::Rotate) ? "Rotate" : "Scale";
    const char* szAxis = (m_AxisLock == AxisLock::X) ? " [X]" : (m_AxisLock == AxisLock::Y) ? " [Y]" : (m_AxisLock == AxisLock::Z) ? " [Z]" : "";
    const char* szSnap = (m_ModalOp == ModalOp::Move) ? (plBrushToolSettings::GetSnapToGrid() ? " Moves snap to the grid, CTRL snaps to vertices." : " CTRL snaps to vertices and the grid.") : "";

    if (m_sNumericInput.IsEmpty())
    {
      pWindow->SetPermanentStatusBarMsg(plFmt("Brush {}{}: move the mouse, X/Y/Z to lock an axis, type a value, Enter/click to commit, ESC to cancel.{}", szOp, szAxis, szSnap));
    }
    else
    {
      pWindow->SetPermanentStatusBarMsg(plFmt("Brush {}{}: {} - Enter/click to commit, ESC to cancel.", szOp, szAxis, m_sNumericInput));
    }

    return;
  }

  if (m_bPivotEdit)
  {
    pWindow->SetPermanentStatusBarMsg(plBrushToolSettings::GetSnapToGrid() ? "Brush - Edit Pivot: drag the gizmo to move the brush's origin, the geometry stays put. The pivot snaps to the grid, CTRL to vertices. ESC ends."
                                                                         : "Brush - Edit Pivot: drag the gizmo to move the brush's origin, the geometry stays put. CTRL snaps to vertices and the grid. ESC ends.");
    return;
  }

  const plString sMenu = plBrushActions::GetShortcutText(plModelingCommand::MarkingMenu);
  const plString sGrab = plBrushActions::GetShortcutText(plModelingCommand::Grab);

  switch (m_SubMode)
  {
    case plBrushSubMode::Vertex:
      pWindow->SetPermanentStatusBarMsg(plFmt("Brush - Vertex mode ({} selected): click or drag a box to select, SHIFT adds, CTRL removes - {} grab, hold CTRL to snap to vertices and the grid - W/E/R gizmos - {} marking menu.", m_SelectedVertices.GetCount(), sGrab, sMenu));
      break;
    case plBrushSubMode::Edge:
      pWindow->SetPermanentStatusBarMsg(plFmt("Brush - Edge mode ({} selected): click or drag a box to select, SHIFT adds, CTRL removes - {} grab - W/E/R gizmos - {} marking menu.", m_SelectedEdges.GetCount(), sGrab, sMenu));
      break;
    case plBrushSubMode::Face:
      pWindow->SetPermanentStatusBarMsg(plFmt("Brush - Face mode, {} faces ({} switches): click or drag a box to select, SHIFT adds, CTRL removes - {} grab - W/E/R gizmos - {} marking menu.",
        m_FaceGranularity == plBrushFaceGranularity::Surface ? "CSG" : "mesh", plBrushActions::GetShortcutText(plModelingCommand::ToggleFaceGranularity), sGrab, sMenu));
      break;
    default:
      pWindow->SetPermanentStatusBarMsg(plFmt("Brush: hold CTRL and drag to draw - {}/{}/{} edit the selected brush's vertices/edges/faces - {} marking menu.", plBrushActions::GetShortcutText(plModelingCommand::ModeVertex),
        plBrushActions::GetShortcutText(plModelingCommand::ModeEdge), plBrushActions::GetShortcutText(plModelingCommand::ModeFace), sMenu));
      break;
  }
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_BrushToolContext);
