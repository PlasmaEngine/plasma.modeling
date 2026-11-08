#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/DocumentWindow/EngineViewWidget.h>
#include <EditorFramework/DocumentWindow/GameObjectDocumentWindow.h>
#include <EditorFramework/Gizmos/SnapProvider.h>
#include <EditorFramework/Panels/AssetBrowserPanel/AssetBrowserPanel.h>
#include <EditorFramework/Preferences/ScenePreferences.h>
#include <EditorPluginModeling/Brush/BrushEditTool.h>
#include <EditorPluginModeling/Brush/EngineBrushOverlays.h>
#include <GuiFoundation/PropertyGrid/ManipulatorManager.h>
#include <ToolsFoundation/Command/TreeCommands.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plBrushEditTool, 1, plRTTIDefaultAllocator<plBrushEditTool>)
PL_END_DYNAMIC_REFLECTED_TYPE;

plInt32 plBrushToolSettings::s_iPendingShape = 0; // plBrushShape::Box
bool plBrushToolSettings::s_bSnapToGrid = true;
plEvent<bool> plBrushToolSettings::s_SnapToGridEvents;

void plBrushToolSettings::SetSnapToGrid(bool bSnap)
{
  if (s_bSnapToGrid == bSnap)
    return;

  s_bSnapToGrid = bSnap;
  s_SnapToGridEvents.Broadcast(bSnap);
}

float plBrushToolSettings::GetGridStep()
{
  const float fStep = plSnapProvider::GetTranslationSnapValue();
  return fStep > 0.0f ? fStep : 0.25f;
}

plVec3 plBrushToolSettings::SnapToGrid(const plVec3& vWorld)
{
  const float fStep = GetGridStep();
  return plVec3(plMath::RoundToMultiple(vWorld.x, fStep), plMath::RoundToMultiple(vWorld.y, fStep), plMath::RoundToMultiple(vWorld.z, fStep));
}

plBrushEditTool::plBrushEditTool()
{
  m_DrawBoxGizmo.m_GizmoEvents.AddEventHandler(plMakeDelegate(&plBrushEditTool::GizmoEventHandler, this));
}

plBrushEditTool::~plBrushEditTool()
{
  m_DrawBoxGizmo.m_GizmoEvents.RemoveEventHandler(plMakeDelegate(&plBrushEditTool::GizmoEventHandler, this));
}

plEditorInputContext* plBrushEditTool::GetEditorInputContextOverride()
{
  if (IsActive())
    return &m_ToolContext;

  return nullptr;
}

plEditToolSupportedSpaces plBrushEditTool::GetSupportedSpaces() const
{
  return plEditToolSupportedSpaces::WorldSpaceOnly;
}

bool plBrushEditTool::GetSupportsMoveParentOnly() const
{
  return false;
}

void plBrushEditTool::GetGridSettings(plGridSettingsMsgToEngine& ref_msg)
{
  plScenePreferencesUser* pPreferences = plPreferences::QueryPreferences<plScenePreferencesUser>(GetDocument());

  ref_msg.m_fGridDensity = plBrushToolSettings::GetGridStep(); // negative density = local space
  ref_msg.m_vGridTangent1.SetZero();
  ref_msg.m_vGridTangent2.SetZero();

  if (pPreferences->GetShowGrid())
  {
    if (m_DrawBoxGizmo.GetCurrentMode() == plDrawBoxGizmo::ManipulateMode::DrawBase)
    {
      ref_msg.m_vGridCenter = m_DrawBoxGizmo.GetStartPosition();

      ref_msg.m_vGridTangent1 = plVec3(1, 0, 0);
      ref_msg.m_vGridTangent2 = plVec3(0, 1, 0);
    }
    else if (m_DrawBoxGizmo.GetCurrentMode() == plDrawBoxGizmo::ManipulateMode::DrawHeight)
    {
      const plVec3 vCamDir = GetWindow()->GetFocusedViewWidget()->m_pViewConfig->m_Camera.GetDirForwards();

      ref_msg.m_vGridCenter = m_DrawBoxGizmo.GetStartPosition();

      if (plMath::Abs(plVec3(1, 0, 0).Dot(vCamDir)) < plMath::Abs(plVec3(0, 1, 0).Dot(vCamDir)))
      {
        ref_msg.m_vGridTangent1 = plVec3(1, 0, 0);
        ref_msg.m_vGridTangent2 = plVec3(0, 0, 1);
      }
      else
      {
        ref_msg.m_vGridTangent1 = plVec3(0, 1, 0);
        ref_msg.m_vGridTangent2 = plVec3(0, 0, 1);
      }
    }
    else if (m_DrawBoxGizmo.GetCurrentMode() == plDrawBoxGizmo::ManipulateMode::None)
    {
      if (m_DrawBoxGizmo.GetDisplayGrid())
      {
        ref_msg.m_vGridCenter = m_DrawBoxGizmo.GetStartPosition();

        ref_msg.m_vGridTangent1 = plVec3(1, 0, 0);
        ref_msg.m_vGridTangent2 = plVec3(0, 1, 0);
      }
    }
  }
}

void plBrushEditTool::UpdateGizmoState()
{
  plManipulatorManager::GetSingleton()->HideActiveManipulator(GetDocument(), GetDocument()->GetActiveEditTool() != nullptr);

  m_DrawBoxGizmo.SetVisible(IsActive());
  m_DrawBoxGizmo.SetTransformation(plTransform::MakeIdentity());
}

void plBrushEditTool::GameObjectEventHandler(const plGameObjectEvent& e)
{
  switch (e.m_Type)
  {
    case plGameObjectEvent::Type::ActiveEditToolChanged:
      UpdateGizmoState();
      break;

    default:
      break;
  }
}

void plBrushEditTool::ManipulatorManagerEventHandler(const plManipulatorManagerEvent& e)
{
  if (!IsActive())
    return;

  // make sure the gizmo is deactivated when a manipulator becomes active
  if (e.m_pDocument == GetDocument() && e.m_pManipulator != nullptr && e.m_pSelection != nullptr && !e.m_pSelection->IsEmpty() &&
      !e.m_bHideManipulators)
  {
    GetDocument()->SetActiveEditTool(nullptr);
  }
}

void plBrushEditTool::OnConfigured()
{
  GetDocument()->m_GameObjectEvents.AddEventHandler(plMakeDelegate(&plBrushEditTool::GameObjectEventHandler, this));
  plManipulatorManager::GetSingleton()->m_Events.AddEventHandler(plMakeDelegate(&plBrushEditTool::ManipulatorManagerEventHandler, this));

  m_DrawBoxGizmo.SetOwner(GetWindow(), nullptr);

  m_ToolContext.SetOwner(GetWindow(), nullptr);
  m_ToolContext.SetDrawGizmo(&m_DrawBoxGizmo);
  m_ToolContext.SetDocument(GetDocument());
}

void plBrushEditTool::OnActiveChanged(bool bIsActive)
{
  if (bIsActive)
  {
    m_DrawBoxGizmo.UpdateStatusBarText(GetWindow());
    CreateModeStrips();
  }
  else
  {
    // leaving the tool ends any sub-object or pivot session; the next activation starts in draw mode
    m_ToolContext.SetPivotEdit(false).IgnoreResult();
    m_ToolContext.SetSubMode(plBrushSubMode::Object);
    DestroyModeStrips();
  }
}

void plBrushEditTool::CreateModeStrips()
{
  // recreated on every activation: the window's view set can change (single <-> quad view)
  DestroyModeStrips();

  for (plEngineViewWidget* pView : GetWindow()->GetViewWidgets())
  {
    m_ModeStrips.PushBack(PL_DEFAULT_NEW(plEngineBrushModeStrip, pView, &m_ToolContext));
  }
}

void plBrushEditTool::DestroyModeStrips()
{
  m_ModeStrips.Clear();
}

void plBrushEditTool::GizmoEventHandler(const plGizmoEvent& e)
{
  if (e.m_Type != plGizmoEvent::Type::EndInteractions)
    return;

  plVec3 vCenter;
  float negx, posx, negy, posy, negz, posz;
  m_DrawBoxGizmo.GetResult(vCenter, negx, posx, negy, posy, negz, posz);

  // the draw gizmo follows the editor's snapping, which may be off: the brush lands on the grid anyway
  if (plBrushToolSettings::GetSnapToGrid())
  {
    const float fStep = plBrushToolSettings::GetGridStep();
    const plVec3 vMin = plBrushToolSettings::SnapToGrid(vCenter - plVec3(negx, negy, negz));
    plVec3 vMax = plBrushToolSettings::SnapToGrid(vCenter + plVec3(posx, posy, posz));

    // a side drawn thinner than one step stays one step thick
    for (plUInt32 i = 0; i < 3; ++i)
    {
      if (vMax.GetData()[i] <= vMin.GetData()[i])
        vMax.GetData()[i] = vMin.GetData()[i] + fStep;
    }

    vCenter = plBrushToolSettings::SnapToGrid(vCenter).CompMax(vMin).CompMin(vMax);
    negx = vCenter.x - vMin.x;
    negy = vCenter.y - vMin.y;
    negz = vCenter.z - vMin.z;
    posx = vMax.x - vCenter.x;
    posy = vMax.y - vCenter.y;
    posz = vMax.z - vCenter.z;
  }

  auto* pDoc = GetDocument();
  auto* pHistory = pDoc->GetCommandHistory();

  plUuid materialGuid;

  // a material selected in the asset browser becomes slot 0 of the new brush
  {
    const plUuid lastSelected = plAssetBrowserPanel::GetSingleton() != nullptr ? plAssetBrowserPanel::GetSingleton()->GetLastSelectedAsset() : plUuid();

    if (lastSelected.IsValid())
    {
      const auto pSubAsset = plAssetCurator::GetSingleton()->GetSubAsset(lastSelected);
      if (pSubAsset && plStringUtils::IsEqual(pSubAsset->m_pAssetInfo->m_Info->GetAssetsDocumentTypeName(), "Material"))
      {
        materialGuid = lastSelected;
      }
    }
  }

  const plDocumentObject* pGroup = FindTargetGroup();

  pHistory->StartTransaction("Add Brush");

  // the first failing command cancels the whole brush: an open transaction would block the document
  plStatus result(PL_SUCCESS);
  auto add = [&](plCommand& ref_cmd)
  {
    if (result.Succeeded())
      result = pHistory->AddCommand(ref_cmd);
  };

  plUuid objGuid, compGuid;
  objGuid = plUuid::MakeUuid();
  compGuid = plUuid::MakeUuid();

  {
    plAddObjectCommand cmdAdd;
    cmdAdd.m_NewObjectGuid = objGuid;
    cmdAdd.m_pType = plGetStaticRTTI<plGameObject>();
    cmdAdd.m_sParentProperty = "Children";
    cmdAdd.m_Index = -1;

    if (pGroup != nullptr)
      cmdAdd.m_Parent = pGroup->GetGuid();

    add(cmdAdd);
  }
  {
    plSetObjectPropertyCommand cmdName;
    cmdName.m_NewValue = plString("Brush");
    cmdName.m_Object = objGuid;
    cmdName.m_sProperty = "Name";
    add(cmdName);
  }

  // the box was drawn in world space: a brush inside a group keeps that placement and orientation
  if (pGroup != nullptr)
  {
    if (const plDocumentObject* pObject = pDoc->GetObjectManager()->GetObject(objGuid); pObject != nullptr && result.Succeeded())
    {
      pDoc->SetGlobalTransform(pObject, plTransform(vCenter), TransformationChanges::All);
    }
  }
  else
  {
    plSetObjectPropertyCommand cmdPos;
    cmdPos.m_NewValue = vCenter;
    cmdPos.m_Object = objGuid;
    cmdPos.m_sProperty = "LocalPosition";
    add(cmdPos);
  }
  {
    plAddObjectCommand cmdComp;
    cmdComp.m_NewObjectGuid = compGuid;
    cmdComp.m_pType = plRTTI::FindTypeByName("plBrushComponent");
    cmdComp.m_sParentProperty = "Components";
    cmdComp.m_Parent = objGuid;
    cmdComp.m_Index = -1;
    add(cmdComp);
  }
  if (materialGuid.IsValid())
  {
    plStringBuilder tmp;
    plInsertObjectPropertyCommand cmdMat;
    cmdMat.m_NewValue = plConversionUtils::ToString(materialGuid, tmp).GetData();
    cmdMat.m_Object = compGuid;
    cmdMat.m_sProperty = "Materials";
    cmdMat.m_Index = 0;
    add(cmdMat);
  }
  if (plBrushToolSettings::s_iPendingShape != 0)
  {
    plSetObjectPropertyCommand cmdShape;
    cmdShape.m_NewValue = static_cast<plInt64>(plBrushToolSettings::s_iPendingShape);
    cmdShape.m_Object = compGuid;
    cmdShape.m_sProperty = "Shape";
    add(cmdShape);
  }

  const struct
  {
    const char* m_szProperty;
    float m_fValue;
  } sizes[] = {{"SizeNegX", negx}, {"SizePosX", posx}, {"SizeNegY", negy}, {"SizePosY", posy}, {"SizeNegZ", negz}, {"SizePosZ", posz}};

  for (const auto& size : sizes)
  {
    plSetObjectPropertyCommand cmdSize;
    cmdSize.m_Object = compGuid;
    cmdSize.m_NewValue = size.m_fValue;
    cmdSize.m_sProperty = size.m_szProperty;
    add(cmdSize);
  }

  if (result.Failed())
  {
    pHistory->CancelTransaction();
    plLog::Error("Adding the brush failed: {}", result.m_sMessage);
    GetWindow()->ShowTemporaryStatusBarMsg(plFmt("Adding the brush failed: {}", result.m_sMessage), plTime::MakeFromSeconds(8));
    return;
  }

  pHistory->FinishTransaction();

  pDoc->GetSelectionManager()->SetSelection(pDoc->GetObjectManager()->GetObject(objGuid));
}

const plDocumentObject* plBrushEditTool::FindTargetGroup() const
{
  // the selected object, or its nearest ancestor, that owns a brush group; after a brush is drawn it is
  // the selection, so drawing again stays in its group
  const auto& selection = GetDocument()->GetSelectionManager()->GetSelection();

  if (selection.IsEmpty())
    return nullptr;

  for (const plDocumentObject* pObject = selection[0]; pObject != nullptr && pObject->GetType() == plGetStaticRTTI<plGameObject>(); pObject = pObject->GetParent())
  {
    for (const plDocumentObject* pChild : pObject->GetChildren())
    {
      if (pChild->GetType()->GetTypeName() == "plBrushGroupComponent")
        return pObject;
    }
  }

  return nullptr;
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_BrushEditTool);
