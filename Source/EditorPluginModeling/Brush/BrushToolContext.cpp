#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorFramework/Document/GameObjectDocument.h>
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
  m_hHoverLines.ConfigureHandle(this, plEngineGizmoHandleType::CustomLines, plColorLinearUB(6, 250, 201), plGizmoFlags::OnTop | plGizmoFlags::ShowInOrtho);
  m_hSelectionLines.ConfigureHandle(this, plEngineGizmoHandleType::CustomLines, plColorLinearUB(255, 160, 32), plGizmoFlags::OnTop | plGizmoFlags::ShowInOrtho);

  SetVisible(false);
  SetTransformation(plTransform::MakeIdentity());
}

plBrushToolContext::~plBrushToolContext() = default;

void plBrushToolContext::OnSetOwner(plQtEngineDocumentWindow* pOwnerWindow, plQtEngineViewWidget* pOwnerView)
{
  pOwnerWindow->GetDocument()->AddSyncObject(&m_hHoverLines);
  pOwnerWindow->GetDocument()->AddSyncObject(&m_hSelectionLines);
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
  }

  m_SubMode = mode;

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

  // the preview is always recomputed from the base mesh, so input parameter changes never accumulate
  m_PreviewMesh = m_BaseMesh;

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

  // ops run in world space: local -> world, transform, world -> local
  const plTransform invTransform = m_ObjectTransform.GetInverse();

  for (plUInt32 uiVertex : m_ModalVertices)
  {
    plVec3 vWorld = m_ObjectTransform.TransformPosition(m_BaseMesh.m_Vertices[uiVertex]);

    switch (m_ModalOp)
    {
      case ModalOp::Move:
        vWorld += vDelta;
        break;

      case ModalOp::Rotate:
        vWorld = m_vPivot + qRotation * (vWorld - m_vPivot);
        break;

      case ModalOp::Scale:
      {
        plVec3 vFromPivot = vWorld - m_vPivot;

        if (vAxis.IsZero())
        {
          vFromPivot *= fScale;
        }
        else
        {
          vFromPivot += vAxis * ((fScale - 1.0f) * vAxis.Dot(vFromPivot));
        }

        vWorld = m_vPivot + vFromPivot;
        break;
      }

      default:
        break;
    }

    m_PreviewMesh.m_Vertices[uiVertex] = invTransform.TransformPosition(vWorld);
  }

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
    case Qt::Key_R:
      BeginModalOp(ModalOp::Rotate);
      return plEditorInput::WasExclusivelyHandled;
    case Qt::Key_S:
      BeginModalOp(ModalOp::Scale);
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
    HoverHit hit;
    PickAt(e->pos(), hit);
    ApplyClickSelection(hit, e->modifiers().testFlag(Qt::ShiftModifier));
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

float plBrushToolContext::GetMarkerSize() const
{
  plBoundingBox bbox = plBoundingBox::MakeInvalid();

  for (const plVec3& v : m_BaseMesh.m_Vertices)
  {
    bbox.ExpandToInclude(v);
  }

  if (!bbox.IsValid())
    return 0.05f;

  return plMath::Clamp(bbox.GetHalfExtents().GetLength() * 0.03f, 0.02f, 0.25f);
}

void plBrushToolContext::AppendVertexMarker(plDynamicArray<plVec3>& ref_lines, const plVec3& vPos, float fSize) const
{
  ref_lines.PushBack(vPos - plVec3(fSize, 0, 0));
  ref_lines.PushBack(vPos + plVec3(fSize, 0, 0));
  ref_lines.PushBack(vPos - plVec3(0, fSize, 0));
  ref_lines.PushBack(vPos + plVec3(0, fSize, 0));
  ref_lines.PushBack(vPos - plVec3(0, 0, fSize));
  ref_lines.PushBack(vPos + plVec3(0, 0, fSize));
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

void plBrushToolContext::ClearOverlays()
{
  m_hHoverLines.SetVisible(false);
  m_hSelectionLines.SetVisible(false);
}

void plBrushToolContext::UpdateOverlays()
{
  if (m_SubMode == plBrushSubMode::Object)
  {
    ClearOverlays();
    return;
  }

  const plEditableMesh& mesh = (m_ModalOp != ModalOp::None) ? m_PreviewMesh : m_BaseMesh;
  const float fMarker = GetMarkerSize();

  plDynamicArray<plVec3> lines;

  // hover
  if (m_Hover.IsValid() && m_ModalOp == ModalOp::None)
  {
    if (m_SubMode == plBrushSubMode::Vertex && m_Hover.m_iVertex >= 0)
    {
      AppendVertexMarker(lines, mesh.m_Vertices[m_Hover.m_iVertex], fMarker);
    }
    else if (m_SubMode == plBrushSubMode::Edge && m_Hover.m_iEdgeV0 >= 0)
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

  // selection
  lines.Clear();

  for (plUInt32 uiVertex : m_SelectedVertices)
  {
    if (uiVertex < mesh.m_Vertices.GetCount())
    {
      AppendVertexMarker(lines, mesh.m_Vertices[uiVertex], fMarker * 0.8f);
    }
  }

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
      pWindow->SetPermanentStatusBarMsg(plFmt("Brush - Vertex mode: click to select ({} selected, SHIFT adds) - G move, R rotate, S scale - 2 edges, 3 faces, ESC object mode.", m_SelectedVertices.GetCount()));
      break;
    case plBrushSubMode::Edge:
      pWindow->SetPermanentStatusBarMsg(plFmt("Brush - Edge mode: click to select ({} selected, SHIFT adds) - G move, R rotate, S scale - 1 vertices, 3 faces, ESC object mode.", m_SelectedEdges.GetCount()));
      break;
    case plBrushSubMode::Face:
      pWindow->SetPermanentStatusBarMsg(plFmt("Brush - Face mode: click to select ({} selected, SHIFT adds) - G move, R rotate, S scale - 1 vertices, 2 edges, ESC object mode.", m_SelectedFaces.GetCount()));
      break;
    default:
      pWindow->SetPermanentStatusBarMsg("Brush: drag to draw - 1/2/3 for vertex/edge/face editing of the selected brush.");
      break;
  }
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_BrushToolContext);
