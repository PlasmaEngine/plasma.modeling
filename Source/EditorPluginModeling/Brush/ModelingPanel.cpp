#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorFramework/EngineShell/DocumentContent.h>
#include <EditorFramework/EngineShell/DocumentFrame.h>
#include <EditorPluginModeling/Brush/BrushCommands.h>
#include <EditorPluginModeling/Brush/BrushEditTool.h>
#include <EditorPluginModeling/Brush/BrushExport.h>
#include <EditorPluginModeling/Brush/BrushPicking.h>
#include <EditorPluginModeling/Brush/EngineBrushOverlays.h>
#include <EditorPluginModeling/Brush/EngineModelingPanel.h>
#include <EditorPluginModeling/Brush/ModelingPanel.h>
#include <EditorPluginModeling/Brush/ModelingPanelContent.h>
#include <Foundation/IO/OSFile.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.h>
#include <ModelingPlugin/Modeling/BrushComponent.h>
#include <ModelingPlugin/Modeling/BrushGenerators.h>
#include <ModelingPlugin/Modeling/MeshCSG.h>
#include <ModelingPlugin/Modeling/MeshOps.h>
#include <ModelingPlugin/Modeling/MeshValidator.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <ToolsFoundation/CommandHistory/CommandHistory.h>

namespace
{
  /// The panels that exist, so the plugin can unhook them from their documents when it unloads.
  plDynamicArray<plModelingPanel*> s_Panels;

  /// The panel in a scene's frame in the engine shell.
  constexpr const char* s_szShellPanelId = "Modeling";
  constexpr const char* s_szPanelIcon = ":/TypeIcons/plBrushEditTool.svg";

  bool IsSceneDocument(const plDocument* pDocument)
  {
    // prefabs are scene documents too, but they get no modeling panel
    const plDocumentTypeDescriptor* pTypeDesc = pDocument->GetDocumentTypeDescriptor();
    if (pTypeDesc != nullptr && pTypeDesc->m_sDocumentTypeName == "Prefab")
      return false;

    // walk the type chain by name, the scene types live in a plugin this package does not link
    for (const plRTTI* pRtti = pDocument->GetDynamicRTTI(); pRtti != nullptr; pRtti = pRtti->GetParentType())
    {
      if (pRtti->GetTypeName() == "plSceneDocument")
        return true;
    }

    return false;
  }

  const char* c_szNoFaces = "Select faces in face mode (3) to edit their texturing.";

  /// What the texture tools need to know about one selected face, from the mesh as rendered.
  struct FaceInfo
  {
    plUInt32 m_uiFaceId = 0;
    plBrushFaceUV m_UV;
    plUInt8 m_uiFlags = 0;
    plUInt8 m_uiSlot = 0;
    plVec3 m_vNormal = plVec3(0, 0, 1);
    float m_fMinU = 0, m_fMaxU = 0, m_fMinV = 0, m_fMaxV = 0; ///< corner projections onto the (rotated) axes
  };

  /// Whether a material projects its textures in world space and so ignores the brush's UVs: a visual
  /// shader with a three-way (triplanar) texture node, or UseWorldUV set. Inherited from the base material.
  bool MaterialIgnoresMeshUVs(plStringView sMaterialGuid, plUInt32 uiDepth = 0)
  {
    plUuid guid;
    if (uiDepth > 4 || !plConversionUtils::TryConvertStringToUuid(sMaterialGuid, guid))
      return false;

    auto pAsset = plAssetCurator::GetSingleton()->GetSubAsset(guid);
    if (!pAsset)
      return false;

    plOSFile file;
    if (file.Open(pAsset->m_pAssetInfo->m_Path.GetAbsolutePath(), plFileOpenMode::Read).Failed())
      return false;

    plDataBuffer data;
    file.ReadAll(data);
    data.PushBack(0);

    const plStringView sContent(reinterpret_cast<const char*>(data.GetData()));

    if (sContent.FindSubString("ShaderNode::Texture3Way") != nullptr || sContent.FindSubString("%UseWorldUV{1}") != nullptr)
      return true;

    const char* szBase = sContent.FindSubString("%BaseMaterial{\"");
    if (szBase == nullptr)
      return false;

    plStringView sBase(szBase + 15, sContent.GetEndPointer());
    if (const char* szEnd = sBase.FindSubString("\""))
      sBase = plStringView(sBase.GetStartPointer(), szEnd);

    return !sBase.IsEmpty() && MaterialIgnoresMeshUVs(sBase, uiDepth + 1);
  }

  bool GatherFaceInfos(const plDocumentObject* pBrush, const plHashSet<plUInt32>& faceIds, plDynamicArray<FaceInfo>& out_infos)
  {
    out_infos.Clear();

    plEditableMesh mesh;
    if (plBrushPicking::BuildEvaluatedMeshFromObject(pBrush, mesh).Failed())
      return false;

    const bool bTextureLock = pBrush->GetTypeAccessor().GetValue("TextureLock").ConvertTo<bool>();
    const plTransform global = plBrushPicking::GetComponentGlobalTransform(pBrush);

    plHashSet<plUInt32> done;

    for (plUInt32 f = 0; f < mesh.m_Faces.GetCount(); ++f)
    {
      const plBrushFace& face = mesh.m_Faces[f];

      if (!faceIds.Contains(face.m_uiFaceId) || done.Contains(face.m_uiFaceId))
        continue;

      done.Insert(face.m_uiFaceId);

      FaceInfo& info = out_infos.ExpandAndGetRef();
      info.m_uiFaceId = face.m_uiFaceId;
      info.m_UV = face.m_UV;
      info.m_uiFlags = face.m_uiFlags;
      info.m_uiSlot = face.m_uiMaterialSlot;
      info.m_vNormal = mesh.ComputeFaceNormal(f);

      plVec3 vU, vV;
      mesh.GetFaceUVAxes(f, info.m_vNormal, vU, vV);

      if (!bTextureLock && face.m_UV.m_bWorldAligned)
      {
        const plVec3 vWorldNormal = global.TransformDirection(info.m_vNormal).GetNormalized();
        plEditableMesh::GetWorldAlignedUVAxes(vWorldNormal, vU, vV);

        if (face.m_UV.m_Rotation != plAngle())
        {
          const plMat3 rot = plMat3::MakeAxisRotation(vWorldNormal, face.m_UV.m_Rotation);
          vU = rot * vU;
          vV = rot * vV;
        }
      }

      info.m_fMinU = info.m_fMinV = plMath::MaxValue<float>();
      info.m_fMaxU = info.m_fMaxV = -plMath::MaxValue<float>();

      for (plUInt32 uiCorner : face.m_Corners)
      {
        const plVec3 vPos = bTextureLock ? mesh.m_Vertices[uiCorner] : global.TransformPosition(mesh.m_Vertices[uiCorner]);
        info.m_fMinU = plMath::Min(info.m_fMinU, vPos.Dot(vU));
        info.m_fMaxU = plMath::Max(info.m_fMaxU, vPos.Dot(vU));
        info.m_fMinV = plMath::Min(info.m_fMinV, vPos.Dot(vV));
        info.m_fMaxV = plMath::Max(info.m_fMaxV, vPos.Dot(vV));
      }
    }

    return !out_infos.IsEmpty();
  }

  float Frac(float f)
  {
    return f - plMath::Floor(f);
  }

  void ShowStatusMessage(plDocument* pDocument, plStringView sMessage, plTime duration = plTime::MakeFromSeconds(5))
  {
    if (plDocumentWindow* pWindow = plDocumentWindow::FindWindowByDocument(pDocument))
      pWindow->ShowTemporaryStatusBarMsg(plFmt("{}", sMessage), duration);
  }
} // namespace

plModelingPanel::plModelingPanel(plDocument* pDocument)
  : m_pDocument(pDocument)
{
  plModelingPanelManager::RegisterPanel(this);

  // the checked buttons are mode indicators, so they have to follow tool and keyboard state
  if (plGameObjectDocument* pGameDoc = plDynamicCast<plGameObjectDocument*>(m_pDocument))
  {
    pGameDoc->m_GameObjectEvents.AddEventHandler(plMakeDelegate(&plModelingPanel::GameObjectEventHandler, this));
  }

  plBrushToolContext::s_ModeEvents.AddEventHandler(plMakeDelegate(&plModelingPanel::BrushModeEventHandler, this));
  plBrushToolContext::s_SelectionEvents.AddEventHandler(plMakeDelegate(&plModelingPanel::BrushSelectionEventHandler, this));
  plBrushToolSettings::s_SnapToGridEvents.AddEventHandler(plMakeDelegate(&plModelingPanel::SnapToGridEventHandler, this));
  m_pDocument->GetObjectManager()->m_StructureEvents.AddEventHandler(plMakeDelegate(&plModelingPanel::StructureEventHandler, this));
  m_pDocument->GetObjectManager()->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plModelingPanel::PropertyEventHandler, this));
  m_pDocument->GetSelectionManager()->m_Events.AddEventHandler(plMakeDelegate(&plModelingPanel::SelectionEventHandler, this));
  m_DocumentManagerSubscription = plDocumentManager::s_Events.AddEventHandler(plMakeDelegate(&plModelingPanel::DocumentManagerEventHandler, this));

  SetCheckedMode(-1);

  m_pFixup = PL_DEFAULT_NEW(plBrushDocumentFixup, m_pDocument);
  m_pFixup->AssignMissingIds();

  UpdateEnabledState();
  UpdateTextureFields();
  ScheduleHealthRefresh();
}

plModelingPanel::~plModelingPanel()
{
  DetachFromDocument();
  plDocumentManager::s_Events.RemoveEventHandler(m_DocumentManagerSubscription);
  plModelingPanelManager::UnregisterPanel(this);
}

void plModelingPanel::DocumentManagerEventHandler(const plDocumentManager::Event& e)
{
  // the document goes before its frame destroys the view and with it the panel
  if (e.m_Type == plDocumentManager::Event::Type::DocumentClosing && e.m_pDocument == m_pDocument)
    DetachFromDocument();
}

void plModelingPanel::DetachFromDocument()
{
  if (m_pDocument == nullptr)
    return;

  m_Timers.StopAll();
  m_pFixup.Clear();

  m_pDocument->GetSelectionManager()->m_Events.RemoveEventHandler(plMakeDelegate(&plModelingPanel::SelectionEventHandler, this));
  m_pDocument->GetObjectManager()->m_PropertyEvents.RemoveEventHandler(plMakeDelegate(&plModelingPanel::PropertyEventHandler, this));
  m_pDocument->GetObjectManager()->m_StructureEvents.RemoveEventHandler(plMakeDelegate(&plModelingPanel::StructureEventHandler, this));
  plBrushToolContext::s_SelectionEvents.RemoveEventHandler(plMakeDelegate(&plModelingPanel::BrushSelectionEventHandler, this));
  plBrushToolSettings::s_SnapToGridEvents.RemoveEventHandler(plMakeDelegate(&plModelingPanel::SnapToGridEventHandler, this));
  plBrushToolContext::s_ModeEvents.RemoveEventHandler(plMakeDelegate(&plModelingPanel::BrushModeEventHandler, this));

  if (plGameObjectDocument* pGameDoc = plDynamicCast<plGameObjectDocument*>(m_pDocument))
  {
    pGameDoc->m_GameObjectEvents.RemoveEventHandler(plMakeDelegate(&plModelingPanel::GameObjectEventHandler, this));
  }

  m_HealthIssues.Clear();
  m_pDocument = nullptr;
}

void plModelingPanel::InvalidateView()
{
  if (m_pView != nullptr)
    m_pView->Invalidate();
}

//////////////////////////////////////////////////////////////////////////
// mode and draw

void plModelingPanel::PickShape(plInt32 iShape)
{
  plBrushToolSettings::s_iPendingShape = iShape;
  m_iArmedShape = iShape;

  // arm the draw tool: the next click in the viewport starts drawing this primitive
  if (plGameObjectDocument* pGameDoc = plDynamicCast<plGameObjectDocument*>(m_pDocument))
  {
    pGameDoc->SetActiveEditTool(plGetStaticRTTI<plBrushEditTool>());

    // drawing happens in Object mode
    if (plBrushEditTool* pTool = plDynamicCast<plBrushEditTool*>(pGameDoc->GetActiveEditTool()))
    {
      pTool->GetToolContext().SetSubMode(plBrushSubMode::Object);
      SetCheckedMode(plBrushSubMode::Object);
    }
  }

  InvalidateView();
}

void plModelingPanel::PickMode(plInt32 iMode)
{
  const plModelingCommand::Enum commands[] = {plModelingCommand::ModeObject, plModelingCommand::ModeVertex, plModelingCommand::ModeEdge, plModelingCommand::ModeFace};
  plBrushActions::Execute(m_pDocument, commands[iMode]);

  // entering a sub-object mode can be refused (no brush selected) — show the mode that resulted
  plBrushToolContext* pContext = GetBrushContext();
  SetCheckedMode(pContext ? static_cast<int>(pContext->GetSubMode()) : -1);
}

void plModelingPanel::RunCommand(plModelingCommand::Enum cmd)
{
  plBrushActions::Execute(m_pDocument, cmd);
  UpdateEnabledState();
}

void plModelingPanel::SetFaceGranularity(plInt32 iGranularity)
{
  if (plBrushToolContext* pContext = GetBrushContext())
    pContext->SetFaceGranularity(static_cast<plBrushFaceGranularity::Enum>(iGranularity));
}

void plModelingPanel::SetSelectThrough(bool bEnable)
{
  if (plBrushToolContext* pContext = GetBrushContext())
    pContext->SetSelectThrough(bEnable);
}

void plModelingPanel::SetShowHiddenHandles(bool bEnable)
{
  if (plBrushToolContext* pContext = GetBrushContext())
    pContext->SetShowHiddenHandles(bEnable);
}

void plModelingPanel::SetSnapToGrid(bool bEnable)
{
  plBrushToolSettings::SetSnapToGrid(bEnable);
}

void plModelingPanel::SnapToGridEventHandler(bool bSnap)
{
  PL_IGNORE_UNUSED(bSnap);
  InvalidateView();
}

void plModelingPanel::BrushModeEventHandler(const plBrushModeEvent& e)
{
  if (e.m_pDocument != m_pDocument)
    return;

  SetCheckedMode(static_cast<int>(e.m_Mode));
  SyncToggles(e.m_pContext);
  UpdateEnabledState();
}

void plModelingPanel::SyncToggles(const plBrushToolContext* pContext)
{
  m_bSelectThrough = pContext->GetSelectThrough();
  m_bShowHiddenHandles = pContext->GetShowHiddenHandles();
  m_iFaceGranularity = static_cast<plInt32>(pContext->GetFaceGranularity());
  InvalidateView();
}

void plModelingPanel::BrushSelectionEventHandler(const plBrushSelectionEvent& e)
{
  if (e.m_pDocument != m_pDocument)
    return;

  UpdateEnabledState();
  UpdateTextureFields();
}

void plModelingPanel::GameObjectEventHandler(const plGameObjectEvent& e)
{
  if (e.m_Type != plGameObjectEvent::Type::ActiveEditToolChanged)
    return;

  plBrushToolContext* pContext = GetBrushContext();

  if (pContext)
  {
    SetCheckedMode(static_cast<int>(pContext->GetSubMode()));
    SyncToggles(pContext);
  }
  else
  {
    UncheckAllButtons();
    SetCheckedMode(-1);
  }

  UpdateEnabledState();
  UpdateTextureFields();
}

void plModelingPanel::SelectionEventHandler(const plSelectionManagerEvent& e)
{
  PL_IGNORE_UNUSED(e);
  UpdateEnabledState();
}

void plModelingPanel::StructureEventHandler(const plDocumentObjectStructureEvent& e)
{
  PL_IGNORE_UNUSED(e);
  ScheduleHealthRefresh();
}

void plModelingPanel::PropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  if (e.m_pObject && plBrushPicking::IsBrushComponent(e.m_pObject))
  {
    // health only depends on the geometry and the brush's role, not on how its faces look
    if (e.m_sProperty != "FaceData" && e.m_sProperty != "Materials" && e.m_sProperty != "Color" && e.m_sProperty != "CustomData")
      ScheduleHealthRefresh();

    // a drag writes a preview mesh per mouse move; the fields are refreshed once it ends
    const plBrushToolContext* pContext = GetBrushContext();
    if (pContext != nullptr && pContext->IsPreviewing())
      return;

    if (e.m_sProperty == "FaceData" || e.m_sProperty == "StoredMesh")
      UpdateTextureFields();
  }
}

void plModelingPanel::SetCheckedMode(int iMode)
{
  m_iCheckedMode = iMode;
  InvalidateView();
}

void plModelingPanel::UncheckAllButtons()
{
  m_iArmedShape = -1;
  InvalidateView();
}

plBrushToolContext* plModelingPanel::GetBrushContext() const
{
  plGameObjectDocument* pGameDoc = plDynamicCast<plGameObjectDocument*>(m_pDocument);

  if (pGameDoc == nullptr)
    return nullptr;

  plBrushEditTool* pTool = plDynamicCast<plBrushEditTool*>(pGameDoc->GetActiveEditTool());
  return pTool ? &pTool->GetToolContext() : nullptr;
}

void plModelingPanel::UpdateEnabledState()
{
  if (m_pDocument == nullptr)
    return;

  plBrushToolContext* pContext = GetBrushContext();
  const plBrushSubMode::Enum mode = pContext ? pContext->GetSubMode() : plBrushSubMode::Object;

  const plUInt32 uiVerts = pContext ? pContext->GetSelectedVertices().GetCount() : 0;
  const plUInt32 uiEdges = pContext ? pContext->GetSelectedEdges().GetCount() : 0;
  const plUInt32 uiFaces = pContext ? pContext->GetSelectedFaces().GetCount() : 0;

  plDynamicArray<const plDocumentObject*> brushes;
  plBrushCommands::GatherSelectedBrushes(m_pDocument, brushes);

  bool bAnyCutter = false;
  for (const plDocumentObject* pBrush : brushes)
    bAnyCutter |= pBrush->GetTypeAccessor().GetValue("Subtractive").ConvertTo<bool>();

  const bool bFaceMode = mode == plBrushSubMode::Face;
  const bool bEdgeMode = mode == plBrushSubMode::Edge;
  const bool bVertexMode = mode == plBrushSubMode::Vertex;
  const bool bSub = mode != plBrushSubMode::Object && pContext && pContext->GetTargetBrush();
  const bool bAnySelected = uiVerts + uiEdges + uiFaces > 0;

  for (plUInt32 i = 0; i < plModelingCommand::Count; ++i)
  {
    bool bEnabled = true;

    switch (static_cast<plModelingCommand::Enum>(i))
    {
      case plModelingCommand::SelectAll:
      case plModelingCommand::SelectNone:
      case plModelingCommand::SelectInvert:
        bEnabled = bSub;
        break;
      case plModelingCommand::SelectGrow:
      case plModelingCommand::SelectShrink:
        bEnabled = bSub && bAnySelected;
        break;
      case plModelingCommand::SelectLoop:
      case plModelingCommand::SelectRing:
        bEnabled = bEdgeMode && uiEdges > 0;
        break;
      case plModelingCommand::SelectSameMaterial:
        bEnabled = bFaceMode && uiFaces > 0;
        break;

      case plModelingCommand::Extrude:
      case plModelingCommand::Inset:
      case plModelingCommand::Flip:
      case plModelingCommand::Subdivide:
      case plModelingCommand::Detach:
      case plModelingCommand::Triangulate:
      case plModelingCommand::Flatten:
        bEnabled = bFaceMode && uiFaces > 0;
        break;
      case plModelingCommand::Bridge:
        bEnabled = bFaceMode && uiFaces == 2;
        break;
      case plModelingCommand::Bevel:
        bEnabled = bEdgeMode && uiEdges > 0;
        break;
      case plModelingCommand::LoopCut:
        bEnabled = bEdgeMode && uiEdges == 1;
        break;
      case plModelingCommand::FillHole:
        bEnabled = bEdgeMode && uiEdges > 0;
        break;
      case plModelingCommand::Weld:
        bEnabled = (bVertexMode && uiVerts > 1) || (bEdgeMode && uiEdges > 0);
        break;
      case plModelingCommand::Connect:
        bEnabled = bVertexMode && uiVerts == 2;
        break;
      case plModelingCommand::Delete:
        bEnabled = bSub && bAnySelected;
        break;

      case plModelingCommand::ToggleCutter:
      case plModelingCommand::Hollow:
      case plModelingCommand::Decompose:
      case plModelingCommand::Clip:
        bEnabled = !brushes.IsEmpty();
        break;
      case plModelingCommand::BakeCuts:
        bEnabled = bAnyCutter;
        break;
      case plModelingCommand::Union:
        bEnabled = brushes.GetCount() >= 2;
        break;
      case plModelingCommand::Group:
        bEnabled = !m_pDocument->GetSelectionManager()->GetSelection().IsEmpty();
        break;

      case plModelingCommand::EditPivot:
        bEnabled = !brushes.IsEmpty() || bSub;
        break;
      case plModelingCommand::PivotToCenter:
      case plModelingCommand::PivotToBottom:
        bEnabled = !brushes.IsEmpty();
        break;
      case plModelingCommand::PivotToSelection:
        bEnabled = bSub && bAnySelected;
        break;

      default:
        break;
    }

    m_CommandEnabled[i] = bEnabled;
  }

  // Edit Pivot is a toggle: lit while the pivot gizmo is up
  m_bPivotEditActive = pContext && pContext->IsPivotEditActive();
  InvalidateView();
}

//////////////////////////////////////////////////////////////////////////
// texture

bool plModelingPanel::GetFaceSelection(const plDocumentObject*& out_pBrush, plHashSet<plUInt32>& out_faceIds) const
{
  plBrushToolContext* pContext = GetBrushContext();

  if (pContext == nullptr || pContext->GetSubMode() != plBrushSubMode::Face || pContext->GetSelectedFaces().IsEmpty() || pContext->GetTargetBrush() == nullptr)
    return false;

  out_pBrush = pContext->GetTargetBrush();
  out_faceIds = pContext->GetSelectedFaces();
  return true;
}

void plModelingPanel::UpdateTextureFields()
{
  const plDocumentObject* pBrush = nullptr;
  plHashSet<plUInt32> faceIds;

  plDynamicArray<FaceInfo> infos;

  if (!GetFaceSelection(pBrush, faceIds) || !GatherFaceInfos(pBrush, faceIds, infos))
  {
    m_bTextureAvailable = false;
    m_sTextureInfo = c_szNoFaces;
    InvalidateView();
    return;
  }

  const FaceInfo& info = infos[0];

  m_bTextureAvailable = true;
  m_TextureSettings.m_bWorldAligned = info.m_UV.m_bWorldAligned;
  m_TextureSettings.m_fTileU = 1.0 / plMath::Max(0.0001f, info.m_UV.m_vScale.x);
  m_TextureSettings.m_fTileV = 1.0 / plMath::Max(0.0001f, info.m_UV.m_vScale.y);
  m_TextureSettings.m_fOffsetU = info.m_UV.m_vOffset.x;
  m_TextureSettings.m_fOffsetV = info.m_UV.m_vOffset.y;
  m_TextureSettings.m_fRotation = info.m_UV.m_Rotation.GetDegree();
  m_TextureSettings.m_bHardEdges = (info.m_uiFlags & plBrushFaceFlags::HardEdges) != 0;

  const plString sMaterial = pBrush->GetTypeAccessor().GetCount("Materials") > info.m_uiSlot ? pBrush->GetTypeAccessor().GetValue("Materials", info.m_uiSlot).ConvertTo<plString>() : plString();
  plStringBuilder sMaterialName = "default material";

  if (!sMaterial.IsEmpty())
  {
    plUuid guid;
    if (plConversionUtils::TryConvertStringToUuid(sMaterial, guid))
    {
      if (auto pAsset = plAssetCurator::GetSingleton()->GetSubAsset(guid))
        sMaterialName = pAsset->GetName();
    }
  }

  plStringBuilder sInfo;
  sInfo.SetFormat("{} face(s). Material: {}. Texel density {} px/m at 1024 px.", infos.GetCount(), sMaterialName, plArgF(1024.0f * info.m_UV.m_vScale.x, 0));

  if (MaterialIgnoresMeshUVs(sMaterial.IsEmpty() ? plStringView(plBrushComponent::GetDefaultMaterialId()) : plStringView(sMaterial)))
  {
    sInfo.Append("\nThis material projects its textures in world space, so these settings have no visible effect on it. Use a material with mesh UVs (for example one without 'Use World UV').");
  }

  m_sTextureInfo = sInfo;
  InvalidateView();
}

void plModelingPanel::ApplyTextureSettings(const plModelingTextureSettings& settings)
{
  const plDocumentObject* pBrush = nullptr;
  plHashSet<plUInt32> faceIds;
  plDynamicArray<FaceInfo> infos;

  if (!GetFaceSelection(pBrush, faceIds) || !GatherFaceInfos(pBrush, faceIds, infos))
    return;

  const bool bWorld = settings.m_bWorldAligned;
  const plVec2 vScale(static_cast<float>(1.0 / settings.m_fTileU), static_cast<float>(1.0 / settings.m_fTileV));
  const plVec2 vOffset(static_cast<float>(settings.m_fOffsetU), static_cast<float>(settings.m_fOffsetV));
  const plAngle rotation = plAngle::MakeFromDegree(static_cast<float>(settings.m_fRotation));
  const bool bHard = settings.m_bHardEdges;

  plHashTable<plUInt32, const FaceInfo*> infoById;
  for (const FaceInfo& info : infos)
    infoById[info.m_uiFaceId] = &info;

  plBrushCommands::EditFaceAttributes(m_pDocument, pBrush, faceIds, "Edit Face Texturing", [&](plUInt32 uiFaceId, plBrushFaceAttributes& attr)
    {
      const FaceInfo* pInfo = nullptr;
      if (!infoById.TryGetValue(uiFaceId, pInfo))
        return;

      const FaceInfo& info = *pInfo;

      attr.m_uiMask |= plBrushFaceAttributes::UV | plBrushFaceAttributes::Flags;
      attr.m_UV = info.m_UV;
      attr.m_UV.m_vScale = vScale;
      attr.m_UV.m_vOffset = vOffset;
      attr.m_UV.m_Rotation = rotation;

      // switching to face alignment takes the axes from the face itself
      if (!bWorld && info.m_UV.m_bWorldAligned)
      {
        plVec3 vU = info.m_vNormal.GetOrthogonalVector();
        if (plMath::Abs(info.m_vNormal.z) < 0.9f)
          vU = plVec3(0, 0, 1).CrossRH(info.m_vNormal);
        vU.NormalizeIfNotZero(plVec3(1, 0, 0)).IgnoreResult();
        attr.m_UV.m_vUAxis = vU;
        attr.m_UV.m_vVAxis = -info.m_vNormal.CrossRH(vU).GetNormalized();
      }

      attr.m_UV.m_bWorldAligned = bWorld;
      attr.m_uiFlags = bHard ? plBrushFaceFlags::HardEdges : 0;
    })
    .IgnoreResult();

  UpdateTextureFields();
}

void plModelingPanel::Justify(char cMode)
{
  const plDocumentObject* pBrush = nullptr;
  plHashSet<plUInt32> faceIds;
  plDynamicArray<FaceInfo> infos;

  if (!GetFaceSelection(pBrush, faceIds) || !GatherFaceInfos(pBrush, faceIds, infos))
    return;

  plHashTable<plUInt32, const FaceInfo*> infoById;
  for (const FaceInfo& info : infos)
    infoById[info.m_uiFaceId] = &info;

  plBrushCommands::EditFaceAttributes(m_pDocument, pBrush, faceIds, cMode == 'F' ? "Fit Texture" : "Justify Texture", [&](plUInt32 uiFaceId, plBrushFaceAttributes& attr)
    {
      const FaceInfo* pInfo = nullptr;
      if (!infoById.TryGetValue(uiFaceId, pInfo))
        return;

      const FaceInfo& info = *pInfo;

      attr.m_uiMask |= plBrushFaceAttributes::UV;
      attr.m_UV = info.m_UV;

      plVec2& vScale = attr.m_UV.m_vScale;
      plVec2& vOffset = attr.m_UV.m_vOffset;

      // UV = projection * scale + offset; the V axis points down on walls, so "top" is the smallest V
      switch (cMode)
      {
        case 'L':
          vOffset.x = Frac(-info.m_fMinU * vScale.x);
          break;
        case 'R':
          vOffset.x = Frac(-info.m_fMaxU * vScale.x);
          break;
        case 'T':
          vOffset.y = Frac(-info.m_fMinV * vScale.y);
          break;
        case 'B':
          vOffset.y = Frac(-info.m_fMaxV * vScale.y);
          break;
        case 'C':
          vOffset.x = Frac(0.5f - 0.5f * (info.m_fMinU + info.m_fMaxU) * vScale.x);
          vOffset.y = Frac(0.5f - 0.5f * (info.m_fMinV + info.m_fMaxV) * vScale.y);
          break;
        case 'F':
          vScale.x = 1.0f / plMath::Max(info.m_fMaxU - info.m_fMinU, 0.0001f);
          vScale.y = 1.0f / plMath::Max(info.m_fMaxV - info.m_fMinV, 0.0001f);
          vOffset.x = -info.m_fMinU * vScale.x;
          vOffset.y = -info.m_fMinV * vScale.y;
          break;
        default:
          break;
      }
    })
    .IgnoreResult();

  UpdateTextureFields();
}

void plModelingPanel::SampleFace()
{
  const plDocumentObject* pBrush = nullptr;
  plHashSet<plUInt32> faceIds;
  plDynamicArray<FaceInfo> infos;

  if (!GetFaceSelection(pBrush, faceIds) || !GatherFaceInfos(pBrush, faceIds, infos))
    return;

  m_Sample = plBrushFaceAttributes();
  m_Sample.m_uiMask = plBrushFaceAttributes::UV | plBrushFaceAttributes::Flags;
  m_Sample.m_UV = infos[0].m_UV;
  m_Sample.m_uiFlags = infos[0].m_uiFlags & plBrushFaceFlags::HardEdges;

  const auto& acc = pBrush->GetTypeAccessor();
  m_sSampleMaterial = acc.GetCount("Materials") > infos[0].m_uiSlot ? acc.GetValue("Materials", infos[0].m_uiSlot).ConvertTo<plString>() : plString();
  m_bHasSample = true;

  ShowStatusMessage(m_pDocument, "Picked up the face's material and texturing. Select other faces and press Apply.");
}

void plModelingPanel::ApplySample()
{
  const plDocumentObject* pBrush = nullptr;
  plHashSet<plUInt32> faceIds;

  if (!m_bHasSample || !GetFaceSelection(pBrush, faceIds))
    return;

  auto* pHistory = m_pDocument->GetCommandHistory();
  pHistory->StartTransaction("Apply Sampled Texturing");

  if (!m_sSampleMaterial.IsEmpty())
  {
    plBrushCommands::SetFaceMaterial(m_pDocument, pBrush, faceIds, m_sSampleMaterial).IgnoreResult();
  }

  plBrushCommands::EditFaceAttributes(m_pDocument, pBrush, faceIds, "Apply Sampled Texturing", [&](plUInt32, plBrushFaceAttributes& attr)
    {
      attr.m_uiMask |= plBrushFaceAttributes::UV | plBrushFaceAttributes::Flags;
      attr.m_UV = m_Sample.m_UV;
      attr.m_uiFlags = m_Sample.m_uiFlags;
    })
    .IgnoreResult();

  pHistory->FinishTransaction();
  UpdateTextureFields();
}

void plModelingPanel::ResetTexture()
{
  const plDocumentObject* pBrush = nullptr;
  plHashSet<plUInt32> faceIds;
  if (!GetFaceSelection(pBrush, faceIds))
    return;

  plBrushCommands::EditFaceAttributes(m_pDocument, pBrush, faceIds, "Reset Texture", [](plUInt32, plBrushFaceAttributes& attr)
    { attr.m_uiMask &= ~plBrushFaceAttributes::UV; })
    .IgnoreResult();

  UpdateTextureFields();
}

//////////////////////////////////////////////////////////////////////////
// output

void plModelingPanel::RunOutput(plUInt32 uiIndex)
{
  plStatus status(PL_SUCCESS);

  switch (uiIndex)
  {
    case 0:
    {
      plDynamicArray<const plDocumentObject*> brushes;
      plBrushCommands::GatherSelectedBrushes(m_pDocument, brushes);
      plString sFile;
      status = plBrushExport::ExportObj(m_pDocument, brushes, plStringView(), &sFile);
      break;
    }

    case 1:
    {
      plDynamicArray<const plDocumentObject*> brushes;
      plBrushCommands::GatherSelectedBrushes(m_pDocument, brushes);
      status = plBrushExport::BakeToMeshAsset(m_pDocument, brushes, true);
      break;
    }

    case 2:
      status = plBrushExport::ConvertGreyBoxes(m_pDocument);
      break;

    case 3:
    {
      const auto& selection = m_pDocument->GetSelectionManager()->GetSelection();
      plHybridArray<const plDocumentObject*, 16> objects;
      for (const plDocumentObject* pObject : selection)
        objects.PushBack(pObject);
      status = plBrushExport::ConvertMeshesToBrushes(m_pDocument, objects);
      break;
    }

    default:
      return;
  }

  ShowStatusMessage(m_pDocument, status.Succeeded() && status.m_sMessage.IsEmpty() ? plStringView("Done.") : status.m_sMessage.GetView(), plTime::MakeFromSeconds(8));
}

//////////////////////////////////////////////////////////////////////////
// health

void plModelingPanel::ScheduleHealthRefresh()
{
  if (m_pDocument == nullptr)
    return;

  m_Timers.Restart(m_uiHealthTimer, plTime::MakeFromMilliseconds(400), [this]()
    { OnRefreshHealth(); });
}

void plModelingPanel::SelectHealthIssue(plUInt32 uiIndex)
{
  if (m_pDocument == nullptr || uiIndex >= m_HealthIssues.GetCount())
    return;

  if (const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(m_HealthIssues[uiIndex].m_BrushObject))
    m_pDocument->GetSelectionManager()->SetSelection(pObject);
}

void plModelingPanel::FixHealthIssue(plUInt32 uiIndex)
{
  if (m_pDocument == nullptr || uiIndex >= m_HealthIssues.GetCount() || !m_HealthIssues[uiIndex].m_Fix)
    return;

  // the fix may refresh the list, so it runs from a copy
  const auto fix = m_HealthIssues[uiIndex].m_Fix;

  if (const plDocumentObject* pTarget = m_pDocument->GetObjectManager()->GetObject(m_HealthIssues[uiIndex].m_Brush))
  {
    const plStatus status = fix(pTarget);
    if (status.Failed())
      ShowStatusMessage(m_pDocument, status.m_sMessage);
  }

  ScheduleHealthRefresh();
}

void plModelingPanel::CollectHealthIssues()
{
  m_HealthIssues.Clear();

  plDynamicArray<const plDocumentObject*> brushes;
  plBrushPicking::FindAllBrushComponents(m_pDocument->GetObjectManager()->GetRootObject(), brushes);

  plEditableMesh mesh;

  auto addIssue = [&](const plDocumentObject* pBrush, plStringView sText, plStringView sFix, std::function<plStatus(const plDocumentObject*)> fix)
  {
    plModelingHealthIssue& issue = m_HealthIssues.ExpandAndGetRef();
    issue.m_sText = sText;
    issue.m_BrushObject = pBrush->GetParent()->GetGuid();
    issue.m_Brush = pBrush->GetGuid();
    issue.m_sFix = sFix;
    issue.m_Fix = std::move(fix);
  };

  auto nameOf = [](const plDocumentObject* pBrush)
  {
    plString sName = pBrush->GetParent()->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
    return sName.IsEmpty() ? plString("(unnamed brush)") : sName;
  };

  plStringBuilder sText;

  for (const plDocumentObject* pBrush : brushes)
  {
    if (m_HealthIssues.GetCount() >= 50)
      break;

    if (plBrushPicking::BuildMeshFromObject(pBrush, mesh).Failed())
    {
      sText.SetFormat("{}: the stored mesh is damaged and renders nothing", nameOf(pBrush));
      addIssue(pBrush, sText, {}, nullptr);
      continue;
    }

    const bool bCutter = pBrush->GetTypeAccessor().GetValue("Subtractive").ConvertTo<bool>();

    plEditableMesh pieces;
    if (bCutter && !mesh.m_Faces.IsEmpty() && plMeshCSG::MakeConvexCutter(mesh, pieces).Failed())
    {
      sText.SetFormat("{}: cutter cannot be split into convex pieces, so it cuts nothing", nameOf(pBrush));
      addIssue(pBrush, sText, "Decompose", [this](const plDocumentObject* pTarget)
        {
          const plDocumentObject* targets[] = {pTarget};
          return plBrushCommands::Decompose(m_pDocument, targets);
        });
    }

    plMeshValidationReport report;
    plMeshValidator::Validate(mesh, report, false); // cutter convexity is reported above

    if (report.m_uiNonPlanarFaces > 0)
    {
      plHashSet<plUInt32> ids;
      for (plUInt32 id : report.m_NonPlanarFaceIds)
        ids.Insert(id);

      sText.SetFormat("{}: {} bent face(s)", nameOf(pBrush), report.m_uiNonPlanarFaces);
      addIssue(pBrush, sText, "Triangulate", [this, ids](const plDocumentObject* pTarget)
        {
          return plBrushCommands::EditMesh(m_pDocument, pTarget, "Triangulate Bent Faces", [&](plEditableMesh& m)
            { return plMeshOps::Triangulate(m, ids); });
        });
    }

    if (report.m_uiOpenEdges > 0)
    {
      sText.SetFormat("{}: {} open edge(s) - the surface has holes", nameOf(pBrush), report.m_uiOpenEdges);
      addIssue(pBrush, sText, {}, nullptr);
    }

    if (report.m_uiDegenerateFaces > 0)
    {
      sText.SetFormat("{}: {} face(s) with no area", nameOf(pBrush), report.m_uiDegenerateFaces);
      addIssue(pBrush, sText, "Clean up", [this](const plDocumentObject* pTarget)
        {
          return plBrushCommands::EditMesh(m_pDocument, pTarget, "Clean Up Brush", [](plEditableMesh& m)
            {
              plMeshOps::WeldByDistance(m, 0.0005f);
              return plStatus(PL_SUCCESS);
            });
        });
    }

    // collision only comes from static objects
    const plDocumentObject* pOwner = pBrush->GetParent();
    if (!bCutter && pBrush->GetTypeAccessor().GetValue("GenerateCollision").ConvertTo<bool>() && pOwner->GetTypeAccessor().GetValue("Mode").ConvertTo<plInt32>() == 1 /* plObjectMode::ForceDynamic */)
    {
      sText.SetFormat("{}: the object is dynamic, so the brush gets no collision", nameOf(pBrush));
      addIssue(pBrush, sText, "Make static", [this](const plDocumentObject* pTarget)
        {
          auto* pHistory = m_pDocument->GetCommandHistory();
          pHistory->StartTransaction("Make Brush Static");
          plSetObjectPropertyCommand cmd;
          cmd.m_Object = pTarget->GetParent()->GetGuid();
          cmd.m_sProperty = "Mode";
          cmd.m_NewValue = static_cast<plInt64>(0); // plObjectMode::Automatic: static unless something moves it
          pHistory->AddCommand(cmd).LogFailure();
          pHistory->FinishTransaction();
          return plStatus(PL_SUCCESS);
        });
    }
  }
}

void plModelingPanel::OnRefreshHealth()
{
  if (m_pDocument == nullptr)
    return;

  CollectHealthIssues();
  InvalidateView();
}

//////////////////////////////////////////////////////////////////////////

void plModelingPanelManager::Startup()
{
  plEditorUiDocumentContentProvider::RegisterExtraPanels(plGetStaticRTTI<plGameObjectDocument>(), &plModelingPanelManager::AddShellPanel);
}

void plModelingPanelManager::Shutdown()
{
  plEditorUiDocumentContentProvider::UnregisterExtraPanels(&plModelingPanelManager::AddShellPanel);

  // the frames keep their views until they close; their panels must not hear from the documents any more
  for (plModelingPanel* pPanel : s_Panels)
  {
    pPanel->DetachFromDocument();
  }
}

void plModelingPanelManager::RegisterPanel(plModelingPanel* pPanel)
{
  s_Panels.PushBack(pPanel);
}

void plModelingPanelManager::UnregisterPanel(plModelingPanel* pPanel)
{
  s_Panels.RemoveAndCopy(pPanel);
}

void plModelingPanelManager::AddShellPanel(plEditorUiDocumentFrame& ref_frame)
{
  plDocument* pDocument = ref_frame.GetDocument();

  // scene documents only
  if (pDocument == nullptr || !IsSceneDocument(pDocument))
    return;

  // the frame owns the view, the view owns the panel
  plUniquePtr<plModelingPanel> pPanel = PL_DEFAULT_NEW(plModelingPanel, pDocument);
  plEngineModelingView* pView = PL_DEFAULT_NEW(plEngineModelingView, std::move(pPanel));
  if (ref_frame.AddContent({s_szShellPanelId, "Modeling"}, plUniquePtr<plEditorUiContent>(pView, plFoundation::GetDefaultAllocator())) == nullptr)
    return;

  // a tab beside the properties; the properties stay in front
  plEditorUiDockManager& panels = ref_frame.GetPanels();
  panels.SetPanelIcon(s_szShellPanelId, s_szPanelIcon);
  panels.DockPanel(s_szShellPanelId, "Properties", plEditorUiDockSide::Tab);
  panels.SetCurrentPanel("Properties");
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_ModelingPanel);
