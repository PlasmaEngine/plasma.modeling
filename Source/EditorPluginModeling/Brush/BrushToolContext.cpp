#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EditorFramework/Document/GameObjectDocument.h>
#include <Foundation/Math/ColorScheme.h>
#include <EditorFramework/DocumentWindow/EngineDocumentWindow.moc.h>
#include <EditorFramework/DocumentWindow/EngineViewWidget.moc.h>
#include <EditorFramework/Gizmos/DrawBoxGizmo.h>
#include <EditorFramework/Gizmos/SnapProvider.h>
#include <EditorPluginModeling/Brush/BrushPicking.h>
#include <EditorPluginModeling/Brush/BrushToolContext.h>
#include <ModelingPlugin/Modeling/BrushGenerators.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <ToolsFoundation/Object/DocumentObjectBase.h>

#include <QKeyEvent>
#include <QMouseEvent>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plBrushToolContext, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

plEvent<const plBrushModeEvent&> plBrushToolContext::s_ModeEvents;

plBrushToolContext::plBrushToolContext()
{
  // three overlay layers, the mockup's language: grey = selectable, teal-green fill = selected,
  // white = hovered
  m_hCandidateLines.ConfigureHandle(this, plEngineGizmoHandleType::CustomLines, plColorLinearUB(130, 135, 150), plGizmoFlags::OnTop | plGizmoFlags::ShowInOrtho);
  m_hSelectionLines.ConfigureHandle(this, plEngineGizmoHandleType::CustomLines, plColorLinearUB(6, 250, 201), plGizmoFlags::OnTop | plGizmoFlags::ShowInOrtho);
  m_hHoverLines.ConfigureHandle(this, plEngineGizmoHandleType::CustomLines, plColorLinearUB(235, 240, 250), plGizmoFlags::OnTop | plGizmoFlags::ShowInOrtho);

  m_TranslateGizmo.m_GizmoEvents.AddEventHandler(plMakeDelegate(&plBrushToolContext::GizmoEventHandler, this));
  m_RotateGizmo.m_GizmoEvents.AddEventHandler(plMakeDelegate(&plBrushToolContext::GizmoEventHandler, this));
  m_ScaleGizmo.m_GizmoEvents.AddEventHandler(plMakeDelegate(&plBrushToolContext::GizmoEventHandler, this));

  SetVisible(false);
  SetTransformation(plTransform::MakeIdentity());
}

plBrushToolContext::~plBrushToolContext()
{
  m_TranslateGizmo.m_GizmoEvents.RemoveEventHandler(plMakeDelegate(&plBrushToolContext::GizmoEventHandler, this));
  m_RotateGizmo.m_GizmoEvents.RemoveEventHandler(plMakeDelegate(&plBrushToolContext::GizmoEventHandler, this));
  m_ScaleGizmo.m_GizmoEvents.RemoveEventHandler(plMakeDelegate(&plBrushToolContext::GizmoEventHandler, this));

  SetDocument(nullptr);
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
  UpdateOverlays();
}

void plBrushToolContext::OnSetOwner(plQtEngineDocumentWindow* pOwnerWindow, plQtEngineViewWidget* pOwnerView)
{
  pOwnerWindow->GetDocument()->AddSyncObject(&m_hCandidateLines);
  pOwnerWindow->GetDocument()->AddSyncObject(&m_hHoverLines);
  pOwnerWindow->GetDocument()->AddSyncObject(&m_hSelectionLines);

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

  return true;
}

//////////////////////////////////////////////////////////////////////////
// picking & selection

plUInt64 plBrushToolContext::MakeEdgeKey(plUInt32 uiV0, plUInt32 uiV1)
{
  const plUInt32 uiLow = plMath::Min(uiV0, uiV1);
  const plUInt32 uiHigh = plMath::Max(uiV0, uiV1);
  return (static_cast<plUInt64>(uiHigh) << 32) | uiLow;
}

bool plBrushToolContext::PickAt(const QPoint& viewPos, HoverHit& out_hit)
{
  out_hit.Reset();

  if (GetOwnerView() == nullptr)
    return false;

  // the GPU pick supplies the surface point (one frame latent — fine for hover); everything
  // sub-object is resolved CPU-side against the rebuilt mesh
  const plObjectPickingResult& res = GetOwnerView()->PickObject(static_cast<plUInt16>(viewPos.x()), static_cast<plUInt16>(viewPos.y()));

  if (!res.m_PickedComponent.IsValid() || res.m_PickedComponent != m_BrushComponent)
    return false;

  if (!RebuildBaseMesh())
    return false;

  const plVec3 vLocalPos = m_ObjectTransform.GetInverse().TransformPosition(res.m_vPickedPosition);

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
      const plUInt32 uiFaceId = m_BaseMesh.m_Faces[hit.m_iFace].m_uiFaceId;

      if (bAdditive)
      {
        if (!m_SelectedFaces.Remove(uiFaceId))
          m_SelectedFaces.Insert(uiFaceId);
      }
      else
      {
        m_SelectedFaces.Clear();
        m_SelectedFaces.Insert(uiFaceId);
      }

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

  for (plUInt32 uiFaceId : m_SelectedFaces)
  {
    for (const plBrushFace& face : m_BaseMesh.m_Faces)
    {
      if (face.m_uiFaceId != uiFaceId)
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
  m_ActiveGizmo = ActiveGizmo::None;

  m_TranslateGizmo.SetVisible(false);
  m_RotateGizmo.SetVisible(false);
  m_ScaleGizmo.SetVisible(false);

  if (m_LastHighlightedHandle.IsValid())
  {
    m_LastHighlightedHandle = plUuid();

    plViewHighlightMsgToEngine msg;
    GetOwnerWindow()->GetEditorEngineConnection()->SendHighlightObjectMessage(&msg);
  }
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
      RepositionGizmos();
      UpdateOverlays();
      break;
    }

    case plGizmoEvent::Type::CancelInteractions:
    {
      if (m_bInTemporaryCommands)
      {
        m_pDocument->GetCommandHistory()->CancelTemporaryCommands();
        m_bInTemporaryCommands = false;
      }

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

plResult plBrushToolContext::PickOnWorkPlane(const QPoint& viewPos, plVec3& out_vPoint) const
{
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
  else if (m_AxisLock != AxisLock::None)
  {
    // translation along an axis: pick on the plane that contains the axis and faces the camera most
    const plVec3 vAxis = GetAxisVector();
    plVec3 vSide = vAxis.CrossRH(vCamFwd);

    if (vSide.GetLengthSquared() > 0.0001f)
    {
      vNormal = vAxis.CrossRH(vSide);
      vNormal.Normalize();
    }
  }

  const plPlane plane = plPlane::MakeFromNormalAndPoint(vNormal, m_vPivot);

  return GetOwnerView()->PickPlane(static_cast<plUInt16>(viewPos.x()), static_cast<plUInt16>(viewPos.y()), plane, out_vPoint);
}

void plBrushToolContext::BeginModalOp(ModalOp::Enum op)
{
  if (!RebuildBaseMesh())
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

  const QPoint viewPos = GetOwnerView()->mapFromGlobal(QCursor::pos());
  m_vLastMouse = plVec2I32(viewPos.x(), viewPos.y());

  if (PickOnWorkPlane(viewPos, m_vStartPoint).Failed())
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

  const QPoint viewPos(m_vLastMouse.x, m_vLastMouse.y);

  double fNumericValue = 0.0;
  const bool bNumeric = !m_sNumericInput.IsEmpty() && plConversionUtils::StringToFloat(m_sNumericInput, fNumericValue).Succeeded();

  const plVec3 vAxis = GetAxisVector();

  plVec3 vDelta = plVec3::MakeZero();
  plQuat qRotation = plQuat::MakeIdentity();
  float fScale = 1.0f;
  float fAngleDeg = 0.0f;

  if (m_ModalOp == ModalOp::Move)
  {
    if (bNumeric && !vAxis.IsZero())
    {
      vDelta = vAxis * static_cast<float>(fNumericValue);
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

      plSnapProvider::SnapTranslation(vDelta);
    }
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

  {
    plSetObjectPropertyCommand cmd;
    cmd.m_Object = m_BrushComponent;
    cmd.m_sProperty = "Shape";
    cmd.m_NewValue = static_cast<plInt64>(plBrushShape::Editable);
    pHistory->AddCommand(cmd).AssertSuccess();
  }
  {
    plSetObjectPropertyCommand cmd;
    cmd.m_Object = m_BrushComponent;
    cmd.m_sProperty = "StoredMesh";
    cmd.m_NewValue = blob;
    pHistory->AddCommand(cmd).AssertSuccess();
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

  MakeActiveInputContext(false);

  UpdateOverlays();

  if (GetOwnerWindow())
  {
    UpdateStatusBarText(GetOwnerWindow());
  }
}

//////////////////////////////////////////////////////////////////////////
// input

plEditorInput plBrushToolContext::DoKeyPressEvent(QKeyEvent* e)
{
  if (m_ModalOp != ModalOp::None)
  {
    switch (e->key())
    {
      case Qt::Key_Escape:
        CancelModalOp();
        return plEditorInput::WasExclusivelyHandled;

      case Qt::Key_Return:
      case Qt::Key_Enter:
        CommitModalOp();
        return plEditorInput::WasExclusivelyHandled;

      case Qt::Key_X:
        m_AxisLock = (m_AxisLock == AxisLock::X) ? AxisLock::None : AxisLock::X;
        UpdateModalPreview();
        return plEditorInput::WasExclusivelyHandled;

      case Qt::Key_Y:
        m_AxisLock = (m_AxisLock == AxisLock::Y) ? AxisLock::None : AxisLock::Y;
        UpdateModalPreview();
        return plEditorInput::WasExclusivelyHandled;

      case Qt::Key_Z:
        m_AxisLock = (m_AxisLock == AxisLock::Z) ? AxisLock::None : AxisLock::Z;
        UpdateModalPreview();
        return plEditorInput::WasExclusivelyHandled;

      case Qt::Key_Backspace:
        if (!m_sNumericInput.IsEmpty())
        {
          m_sNumericInput.Shrink(0, 1);
          UpdateModalPreview();
        }
        return plEditorInput::WasExclusivelyHandled;

      default:
      {
        const QString text = e->text();

        if (text.length() == 1)
        {
          const char c = text.at(0).toLatin1();

          if ((c >= '0' && c <= '9') || c == '.' || c == '-')
          {
            m_sNumericInput.Append(text.toUtf8().data());
            UpdateModalPreview();
          }
        }

        // a running modal operation swallows all keys — document shortcuts must not fire
        return plEditorInput::WasExclusivelyHandled;
      }
    }
  }

  if (m_SubMode == plBrushSubMode::Object)
  {
    switch (e->key())
    {
      case Qt::Key_1:
        SetSubMode(plBrushSubMode::Vertex);
        return plEditorInput::WasExclusivelyHandled;
      case Qt::Key_2:
        SetSubMode(plBrushSubMode::Edge);
        return plEditorInput::WasExclusivelyHandled;
      case Qt::Key_3:
        SetSubMode(plBrushSubMode::Face);
        return plEditorInput::WasExclusivelyHandled;
      case Qt::Key_Tab:
        SetSubMode(m_LastSubMode);
        return plEditorInput::WasExclusivelyHandled;
      default:
        break;
    }

    return m_pDrawGizmo ? m_pDrawGizmo->KeyPressEvent(e) : plEditorInput::MayBeHandledByOthers;
  }

  // vertex / edge / face mode
  switch (e->key())
  {
    case Qt::Key_1:
      SetSubMode(plBrushSubMode::Vertex);
      return plEditorInput::WasExclusivelyHandled;
    case Qt::Key_2:
      SetSubMode(plBrushSubMode::Edge);
      return plEditorInput::WasExclusivelyHandled;
    case Qt::Key_3:
      SetSubMode(plBrushSubMode::Face);
      return plEditorInput::WasExclusivelyHandled;

    case Qt::Key_Escape:
    case Qt::Key_Tab:
    case Qt::Key_0:
      SetSubMode(plBrushSubMode::Object);
      return plEditorInput::WasExclusivelyHandled;

    case Qt::Key_G:
      BeginModalOp(ModalOp::Move);
      return plEditorInput::WasExclusivelyHandled;
    case Qt::Key_S:
      BeginModalOp(ModalOp::Scale);
      return plEditorInput::WasExclusivelyHandled;

    // the editor's standard gizmo keys, retargeted at the sub-object selection
    case Qt::Key_W:
      ShowGizmo(ActiveGizmo::Translate);
      return plEditorInput::WasExclusivelyHandled;
    case Qt::Key_E:
      ShowGizmo(ActiveGizmo::Rotate);
      return plEditorInput::WasExclusivelyHandled;
    case Qt::Key_R:
      ShowGizmo(ActiveGizmo::Scale);
      return plEditorInput::WasExclusivelyHandled;
    case Qt::Key_Q:
      HideGizmos();
      return plEditorInput::WasExclusivelyHandled;

    default:
      return plEditorInput::MayBeHandledByOthers;
  }
}

plEditorInput plBrushToolContext::DoKeyReleaseEvent(QKeyEvent* e)
{
  if (m_ModalOp != ModalOp::None)
    return plEditorInput::WasExclusivelyHandled;

  if (m_SubMode == plBrushSubMode::Object && m_pDrawGizmo != nullptr)
    return m_pDrawGizmo->KeyReleaseEvent(e);

  return plEditorInput::MayBeHandledByOthers;
}

plEditorInput plBrushToolContext::DoMousePressEvent(QMouseEvent* e)
{
  if (m_ModalOp != ModalOp::None)
  {
    if (e->button() == Qt::LeftButton)
    {
      CommitModalOp();
    }
    else if (e->button() == Qt::RightButton)
    {
      CancelModalOp();
    }

    return plEditorInput::WasExclusivelyHandled;
  }

  if (m_SubMode == plBrushSubMode::Object)
  {
    return m_pDrawGizmo ? m_pDrawGizmo->MousePressEvent(e) : plEditorInput::MayBeHandledByOthers;
  }

  // camera navigation always wins
  if (e->modifiers().testFlag(Qt::AltModifier))
    return plEditorInput::MayBeHandledByOthers;

  if (e->button() == Qt::LeftButton)
  {
    const plObjectPickingResult& res = GetOwnerView()->PickObject(static_cast<plUInt16>(e->pos().x()), static_cast<plUInt16>(e->pos().y()));

    if (res.m_PickedOther.IsValid())
    {
      // a click on an active transform-gizmo handle belongs to the gizmo, not to selection
      if (auto* pSO = GetOwnerWindow()->GetDocument()->FindSyncObject(res.m_PickedOther))
      {
        if (pSO->GetDynamicRTTI()->IsDerivedFrom<plGizmoHandle>())
        {
          plGizmoHandle* pHandle = static_cast<plGizmoHandle*>(pSO);
          plGizmo* pGizmo = pHandle->GetOwnerGizmo();

          if (m_ActiveGizmo != ActiveGizmo::None && pGizmo == GetActiveGizmoPtr())
          {
            plQtEngineViewWidget* pView = GetOwnerView();
            pGizmo->ConfigureInteraction(pHandle, &pView->m_pViewConfig->m_Camera, res.m_vPickedPosition, plVec2I32(pView->width(), pView->height()));
            return pGizmo->MousePressEvent(e);
          }
        }
      }

      // a click on an element cube selects its element exactly
      plUInt32 uiElement = 0;

      if (m_HandleToElement.TryGetValue(res.m_PickedOther, uiElement))
      {
        HoverHit hit;

        if (MakeHoverFromElement(uiElement, hit))
        {
          ApplyClickSelection(hit, e->modifiers().testFlag(Qt::ShiftModifier));
          RepositionGizmos();
          UpdateOverlays();

          if (GetOwnerWindow())
          {
            UpdateStatusBarText(GetOwnerWindow());
          }

          return plEditorInput::WasExclusivelyHandled;
        }
      }
    }

    HoverHit hit;
    PickAt(e->pos(), hit);
    ApplyClickSelection(hit, e->modifiers().testFlag(Qt::ShiftModifier));
    RepositionGizmos();
    UpdateOverlays();

    if (GetOwnerWindow())
    {
      UpdateStatusBarText(GetOwnerWindow());
    }

    return plEditorInput::WasExclusivelyHandled;
  }

  return plEditorInput::MayBeHandledByOthers;
}

plEditorInput plBrushToolContext::DoMouseReleaseEvent(QMouseEvent* e)
{
  if (m_ModalOp != ModalOp::None)
    return plEditorInput::WasExclusivelyHandled;

  if (m_SubMode == plBrushSubMode::Object)
  {
    return m_pDrawGizmo ? m_pDrawGizmo->MouseReleaseEvent(e) : plEditorInput::MayBeHandledByOthers;
  }

  if (e->button() == Qt::LeftButton)
    return plEditorInput::WasExclusivelyHandled;

  return plEditorInput::MayBeHandledByOthers;
}

plEditorInput plBrushToolContext::DoMouseMoveEvent(QMouseEvent* e)
{
  if (m_ModalOp != ModalOp::None)
  {
    m_vLastMouse = plVec2I32(e->pos().x(), e->pos().y());
    UpdateModalPreview();
    return plEditorInput::WasExclusivelyHandled;
  }

  if (m_SubMode == plBrushSubMode::Object)
  {
    return m_pDrawGizmo ? m_pDrawGizmo->MouseMoveEvent(e) : plEditorInput::MayBeHandledByOthers;
  }

  // let camera drags and rubber bands through
  if (e->buttons() != Qt::NoButton)
    return plEditorInput::MayBeHandledByOthers;

  // transform-gizmo handles and element cubes both highlight through the engine's highlight system
  {
    const plObjectPickingResult& res = GetOwnerView()->PickObject(static_cast<plUInt16>(e->pos().x()), static_cast<plUInt16>(e->pos().y()));

    plUuid hoveredHandle;
    bool bOverTransformGizmo = false;
    plUInt32 uiElement = plInvalidIndex;

    if (res.m_PickedOther.IsValid())
    {
      if (auto* pSO = GetOwnerWindow()->GetDocument()->FindSyncObject(res.m_PickedOther))
      {
        if (pSO->GetDynamicRTTI()->IsDerivedFrom<plGizmoHandle>())
        {
          if (m_ActiveGizmo != ActiveGizmo::None && static_cast<plGizmoHandle*>(pSO)->GetOwnerGizmo() == GetActiveGizmoPtr())
          {
            hoveredHandle = res.m_PickedOther;
            bOverTransformGizmo = true;
          }
          else if (m_HandleToElement.TryGetValue(res.m_PickedOther, uiElement))
          {
            hoveredHandle = res.m_PickedOther;
          }
        }
      }
    }

    if (hoveredHandle != m_LastHighlightedHandle)
    {
      m_LastHighlightedHandle = hoveredHandle;

      plViewHighlightMsgToEngine msg;
      msg.m_HighlightObject = hoveredHandle;
      GetOwnerWindow()->GetEditorEngineConnection()->SendHighlightObjectMessage(&msg);
    }

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

    if (uiElement != plInvalidIndex)
    {
      // hovering a cube hovers its element — precise, no surface pick involved
      HoverHit previous = m_Hover;

      if (!MakeHoverFromElement(uiElement, m_Hover))
      {
        m_Hover.Reset();
      }

      if (plMemoryUtils::Compare(&previous, &m_Hover, 1) != 0)
      {
        UpdateOverlays();
      }

      return plEditorInput::WasExclusivelyHandled;
    }
  }

  HoverHit previous = m_Hover;

  if (!PickAt(e->pos(), m_Hover))
  {
    m_Hover.Reset();
  }

  if (plMemoryUtils::Compare(&previous, &m_Hover, 1) != 0)
  {
    UpdateOverlays();
  }

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
  const plEditableMesh& mesh = (m_ModalOp != ModalOp::None) ? m_PreviewMesh : m_BaseMesh;

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

  const plEditableMesh& mesh = (m_ModalOp != ModalOp::None) ? m_PreviewMesh : m_BaseMesh;

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
      uiElementCount = mesh.m_Faces.GetCount();
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
        vLocalPos = mesh.ComputeFaceCenter(uiElement);
        bSelected = m_SelectedFaces.Contains(mesh.m_Faces[uiElement].m_uiFaceId);
        break;

      default:
        break;
    }

    // the manipulator-nob look: solid constant-size cube, sized a touch below theirs
    plTransform t(m_ObjectTransform.TransformPosition(vLocalPos));
    t.m_vScale.Set(0.1f);

    pHandle->SetTransformation(t);
    pHandle->SetColor(bSelected ? GetElementSelectedColor() : GetElementIdleColor());
    pHandle->SetVisible(true);
  }
}

void plBrushToolContext::HideElementHandles()
{
  for (auto& pHandle : m_ElementHandles)
  {
    pHandle->SetVisible(false);
  }
}

void plBrushToolContext::AppendFaceOutline(plDynamicArray<plVec3>& ref_lines, plUInt32 uiFace) const
{
  const plEditableMesh& mesh = (m_ModalOp != ModalOp::None) ? m_PreviewMesh : m_BaseMesh;
  const plBrushFace& face = mesh.m_Faces[uiFace];

  for (plUInt32 i = 0; i < face.m_Corners.GetCount(); ++i)
  {
    ref_lines.PushBack(mesh.m_Vertices[face.m_Corners[i]]);
    ref_lines.PushBack(mesh.m_Vertices[face.m_Corners[(i + 1) % face.m_Corners.GetCount()]]);
  }
}

void plBrushToolContext::AppendFaceInset(plDynamicArray<plVec3>& ref_lines, plUInt32 uiFace, float fInset) const
{
  // the face outline shrunk towards its centroid; stacked insets read as a filled face
  const plEditableMesh& mesh = (m_ModalOp != ModalOp::None) ? m_PreviewMesh : m_BaseMesh;
  const plBrushFace& face = mesh.m_Faces[uiFace];

  plVec3 vCenter = plVec3::MakeZero();

  for (plUInt32 uiCorner : face.m_Corners)
  {
    vCenter += mesh.m_Vertices[uiCorner];
  }

  vCenter /= static_cast<float>(face.m_Corners.GetCount());

  for (plUInt32 i = 0; i < face.m_Corners.GetCount(); ++i)
  {
    const plVec3 vCur = vCenter + (mesh.m_Vertices[face.m_Corners[i]] - vCenter) * fInset;
    const plVec3 vNext = vCenter + (mesh.m_Vertices[face.m_Corners[(i + 1) % face.m_Corners.GetCount()]] - vCenter) * fInset;

    ref_lines.PushBack(vCur);
    ref_lines.PushBack(vNext);
  }
}

void plBrushToolContext::AppendWireframe(plDynamicArray<plVec3>& ref_lines) const
{
  // every unique edge once — the structure of what is editable
  const plEditableMesh& mesh = (m_ModalOp != ModalOp::None) ? m_PreviewMesh : m_BaseMesh;

  plHashSet<plUInt64> edges;

  for (const plBrushFace& face : mesh.m_Faces)
  {
    for (plUInt32 i = 0; i < face.m_Corners.GetCount(); ++i)
    {
      const plUInt32 uiV0 = face.m_Corners[i];
      const plUInt32 uiV1 = face.m_Corners[(i + 1) % face.m_Corners.GetCount()];

      if (!edges.Insert(MakeEdgeKey(uiV0, uiV1)))
      {
        ref_lines.PushBack(mesh.m_Vertices[uiV0]);
        ref_lines.PushBack(mesh.m_Vertices[uiV1]);
      }
    }
  }
}

void plBrushToolContext::ClearOverlays()
{
  m_hCandidateLines.SetVisible(false);
  m_hHoverLines.SetVisible(false);
  m_hSelectionLines.SetVisible(false);

  HideElementHandles();
}

void plBrushToolContext::UpdateOverlays()
{
  if (m_SubMode == plBrushSubMode::Object)
  {
    ClearOverlays();
    return;
  }

  const plEditableMesh& mesh = (m_ModalOp != ModalOp::None) ? m_PreviewMesh : m_BaseMesh;

  // the cubes are the selection UI (grey = selectable, brand = selected, highlight = hovered);
  // the line layers stay as thin structural accents around them
  UpdateElementHandles();

  plDynamicArray<plVec3> lines;

  AppendWireframe(lines);

  if (lines.IsEmpty())
  {
    m_hCandidateLines.SetVisible(false);
  }
  else
  {
    m_hCandidateLines.SetLines(lines);
    m_hCandidateLines.SetTransformation(m_ObjectTransform);
    m_hCandidateLines.SetVisible(true);
  }

  // hover accents: the edge under the cursor, the outline of the hovered face
  lines.Clear();

  if (m_Hover.IsValid() && m_ModalOp == ModalOp::None)
  {
    if (m_SubMode == plBrushSubMode::Edge && m_Hover.m_iEdgeV0 >= 0)
    {
      lines.PushBack(mesh.m_Vertices[m_Hover.m_iEdgeV0]);
      lines.PushBack(mesh.m_Vertices[m_Hover.m_iEdgeV1]);
    }
    else if (m_SubMode == plBrushSubMode::Face)
    {
      AppendFaceOutline(lines, static_cast<plUInt32>(m_Hover.m_iFace));
    }
  }

  if (lines.IsEmpty())
  {
    m_hHoverLines.SetVisible(false);
  }
  else
  {
    m_hHoverLines.SetLines(lines);
    m_hHoverLines.SetTransformation(m_ObjectTransform);
    m_hHoverLines.SetVisible(true);
  }

  // selection accents: selected edges as brand lines, selected faces as outline + insets
  lines.Clear();

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

  for (plUInt32 uiFaceId : m_SelectedFaces)
  {
    for (plUInt32 uiFace = 0; uiFace < mesh.m_Faces.GetCount(); ++uiFace)
    {
      if (mesh.m_Faces[uiFace].m_uiFaceId == uiFaceId)
      {
        AppendFaceOutline(lines, uiFace);
        AppendFaceInset(lines, uiFace, 0.7f);
        AppendFaceInset(lines, uiFace, 0.4f);
      }
    }
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
}

//////////////////////////////////////////////////////////////////////////
// status bar

void plBrushToolContext::UpdateStatusBarText(plQtEngineDocumentWindow* pWindow)
{
  if (m_ModalOp != ModalOp::None)
  {
    const char* szOp = (m_ModalOp == ModalOp::Move) ? "Move" : (m_ModalOp == ModalOp::Rotate) ? "Rotate" : "Scale";
    const char* szAxis = (m_AxisLock == AxisLock::X) ? " [X]" : (m_AxisLock == AxisLock::Y) ? " [Y]" : (m_AxisLock == AxisLock::Z) ? " [Z]" : "";

    if (m_sNumericInput.IsEmpty())
    {
      pWindow->SetPermanentStatusBarMsg(plFmt("Brush {}{}: move the mouse, X/Y/Z to lock an axis, type a value, Enter/click to commit, ESC to cancel.", szOp, szAxis));
    }
    else
    {
      pWindow->SetPermanentStatusBarMsg(plFmt("Brush {}{}: {} - Enter/click to commit, ESC to cancel.", szOp, szAxis, m_sNumericInput));
    }

    return;
  }

  switch (m_SubMode)
  {
    case plBrushSubMode::Vertex:
      pWindow->SetPermanentStatusBarMsg(plFmt("Brush - Vertex mode ({} selected): click selects, SHIFT adds - W/E/R move/rotate/scale gizmo, Q hides - G grab, S scale (X/Y/Z lock, type a value) - 2 edges, 3 faces, ESC object mode.", m_SelectedVertices.GetCount()));
      break;
    case plBrushSubMode::Edge:
      pWindow->SetPermanentStatusBarMsg(plFmt("Brush - Edge mode ({} selected): click selects, SHIFT adds - W/E/R move/rotate/scale gizmo, Q hides - G grab, S scale (X/Y/Z lock, type a value) - 1 vertices, 3 faces, ESC object mode.", m_SelectedEdges.GetCount()));
      break;
    case plBrushSubMode::Face:
      pWindow->SetPermanentStatusBarMsg(plFmt("Brush - Face mode ({} selected): click selects, SHIFT adds - W/E/R move/rotate/scale gizmo, Q hides - G grab, S scale (X/Y/Z lock, type a value) - 1 vertices, 2 edges, ESC object mode.", m_SelectedFaces.GetCount()));
      break;
    default:
      pWindow->SetPermanentStatusBarMsg("Brush: drag to draw - 1/2/3 for vertex/edge/face editing of the selected brush.");
      break;
  }
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_BrushToolContext);
