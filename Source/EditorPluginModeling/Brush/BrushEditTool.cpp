#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/DocumentWindow/EngineViewWidget.moc.h>
#include <EditorFramework/DocumentWindow/GameObjectDocumentWindow.moc.h>
#include <EditorFramework/Gizmos/SnapProvider.h>
#include <EditorFramework/Panels/AssetBrowserPanel/AssetBrowserPanel.moc.h>
#include <EditorFramework/Preferences/ScenePreferences.h>
#include <EditorPluginModeling/Brush/BrushEditTool.h>
#include <GuiFoundation/PropertyGrid/ManipulatorManager.h>
#include <ToolsFoundation/Command/TreeCommands.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plBrushEditTool, 1, plRTTIDefaultAllocator<plBrushEditTool>)
PL_END_DYNAMIC_REFLECTED_TYPE;

plInt32 plBrushToolSettings::s_iPendingShape = 0; // plBrushShape::Box

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
    return &m_DrawBoxGizmo;

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

  ref_msg.m_fGridDensity = plSnapProvider::GetTranslationSnapValue(); // negative density = local space
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
}

void plBrushEditTool::OnActiveChanged(bool bIsActive)
{
  if (bIsActive)
  {
    m_DrawBoxGizmo.UpdateStatusBarText(GetWindow());
  }
}

void plBrushEditTool::GizmoEventHandler(const plGizmoEvent& e)
{
  if (e.m_Type != plGizmoEvent::Type::EndInteractions)
    return;

  plVec3 vCenter;
  float negx, posx, negy, posy, negz, posz;
  m_DrawBoxGizmo.GetResult(vCenter, negx, posx, negy, posy, negz, posz);

  auto* pDoc = GetDocument();
  auto* pHistory = pDoc->GetCommandHistory();

  plUuid materialGuid;

  // a material selected in the asset browser becomes slot 0 of the new brush
  {
    const plUuid lastSelected = plQtAssetBrowserPanel::GetSingleton()->GetLastSelectedAsset();

    if (lastSelected.IsValid())
    {
      const auto pSubAsset = plAssetCurator::GetSingleton()->GetSubAsset(lastSelected);
      if (pSubAsset && plStringUtils::IsEqual(pSubAsset->m_pAssetInfo->m_Info->GetAssetsDocumentTypeName(), "Material"))
      {
        materialGuid = lastSelected;
      }
    }
  }

  pHistory->StartTransaction("Add Brush");

  plUuid objGuid, compGuid;
  objGuid = plUuid::MakeUuid();
  compGuid = plUuid::MakeUuid();

  {
    plAddObjectCommand cmdAdd;
    cmdAdd.m_NewObjectGuid = objGuid;
    cmdAdd.m_pType = plGetStaticRTTI<plGameObject>();
    cmdAdd.m_sParentProperty = "Children";
    pHistory->AddCommand(cmdAdd).AssertSuccess();
  }
  {
    plSetObjectPropertyCommand cmdName;
    cmdName.m_NewValue = plString("Brush");
    cmdName.m_Object = objGuid;
    cmdName.m_sProperty = "Name";
    pHistory->AddCommand(cmdName).AssertSuccess();
  }
  {
    plSetObjectPropertyCommand cmdPos;
    cmdPos.m_NewValue = vCenter;
    cmdPos.m_Object = objGuid;
    cmdPos.m_sProperty = "LocalPosition";
    pHistory->AddCommand(cmdPos).AssertSuccess();
  }
  {
    plAddObjectCommand cmdComp;
    cmdComp.m_NewObjectGuid = compGuid;
    cmdComp.m_pType = plRTTI::FindTypeByName("plBrushComponent");
    cmdComp.m_sParentProperty = "Components";
    cmdComp.m_Parent = objGuid;
    cmdComp.m_Index = -1;
    pHistory->AddCommand(cmdComp).AssertSuccess();
  }
  if (materialGuid.IsValid())
  {
    plStringBuilder tmp;
    plInsertObjectPropertyCommand cmdMat;
    cmdMat.m_NewValue = plConversionUtils::ToString(materialGuid, tmp).GetData();
    cmdMat.m_Object = compGuid;
    cmdMat.m_sProperty = "Materials";
    cmdMat.m_Index = 0;
    pHistory->AddCommand(cmdMat).AssertSuccess();
  }
  if (plBrushToolSettings::s_iPendingShape != 0)
  {
    plSetObjectPropertyCommand cmdShape;
    cmdShape.m_NewValue = static_cast<plInt64>(plBrushToolSettings::s_iPendingShape);
    cmdShape.m_Object = compGuid;
    cmdShape.m_sProperty = "Shape";
    pHistory->AddCommand(cmdShape).AssertSuccess();
  }
  {
    plSetObjectPropertyCommand cmdSize;
    cmdSize.m_Object = compGuid;

    cmdSize.m_NewValue = negx;
    cmdSize.m_sProperty = "SizeNegX";
    pHistory->AddCommand(cmdSize).AssertSuccess();

    cmdSize.m_NewValue = posx;
    cmdSize.m_sProperty = "SizePosX";
    pHistory->AddCommand(cmdSize).AssertSuccess();

    cmdSize.m_NewValue = negy;
    cmdSize.m_sProperty = "SizeNegY";
    pHistory->AddCommand(cmdSize).AssertSuccess();

    cmdSize.m_NewValue = posy;
    cmdSize.m_sProperty = "SizePosY";
    pHistory->AddCommand(cmdSize).AssertSuccess();

    cmdSize.m_NewValue = negz;
    cmdSize.m_sProperty = "SizeNegZ";
    pHistory->AddCommand(cmdSize).AssertSuccess();

    cmdSize.m_NewValue = posz;
    cmdSize.m_sProperty = "SizePosZ";
    pHistory->AddCommand(cmdSize).AssertSuccess();
  }

  pHistory->FinishTransaction();

  pDoc->GetSelectionManager()->SetSelection(pDoc->GetObjectManager()->GetObject(objGuid));
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_BrushEditTool);
