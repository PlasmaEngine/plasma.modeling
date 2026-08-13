#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorFramework/DragDrop/DragDropInfo.h>
#include <EditorPluginModeling/Brush/BrushFaceMaterialDragDropHandler.h>
#include <EditorPluginModeling/Brush/BrushPicking.h>
#include <GameEngine/Gameplay/GreyBoxComponent.h>
#include <ModelingPlugin/Modeling/BrushComponent.h>
#include <RendererCore/Meshes/MeshComponentBase.h>
#include <ToolsFoundation/Command/TreeCommands.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plBrushFaceMaterialDragDropHandler, 1, plRTTIDefaultAllocator<plBrushFaceMaterialDragDropHandler>)
PL_END_DYNAMIC_REFLECTED_TYPE;

void plBrushFaceMaterialDragDropHandler::RequestConfiguration(plDragDropConfig* pConfigToFillOut)
{
  pConfigToFillOut->m_bPickSelectedObjects = true;
}

float plBrushFaceMaterialDragDropHandler::CanHandle(const plDragDropInfo* pInfo) const
{
  if (pInfo->m_sTargetContext != "viewport")
    return 0.0f;

  const plDocument* pDocument = plDocumentManager::GetDocumentByGuid(pInfo->m_TargetDocument);

  if (!pDocument->GetDynamicRTTI()->IsDerivedFrom<plGameObjectDocument>())
    return 0.0f;

  return IsSpecificAssetType(pInfo, "Material") ? 1.5f : 0.0f;
}

void plBrushFaceMaterialDragDropHandler::OnDragBegin(const plDragDropInfo* pInfo)
{
  m_pDocument = plDocumentManager::GetDocumentByGuid(pInfo->m_TargetDocument);
  PL_ASSERT_DEV(m_pDocument != nullptr, "Invalid document GUID in drag & drop operation");

  m_AppliedToComponent = plUuid();
  m_iAppliedToSlot = -1;
  m_uiAppliedFaceId = plInvalidIndex;

  m_pDocument->GetCommandHistory()->BeginTemporaryCommands("Drag Material", true);
}

void plBrushFaceMaterialDragDropHandler::OnDragUpdate(const plDragDropInfo* pInfo)
{
  if (!pInfo->m_TargetComponent.IsValid())
    return;

  const plDocumentObject* pComponent = m_pDocument->GetObjectManager()->GetObject(pInfo->m_TargetComponent);

  if (!pComponent)
    return;

  const plRTTI* pType = pComponent->GetTypeAccessor().GetType();

  if (pType->GetTypeName() == "plBrushComponent")
  {
    ApplyToBrushFace(pInfo, pComponent);
    return;
  }

  if (m_AppliedToComponent == pInfo->m_TargetComponent && m_iAppliedToSlot == pInfo->m_iTargetObjectSubID)
    return;

  m_AppliedToComponent = pInfo->m_TargetComponent;
  m_iAppliedToSlot = pInfo->m_iTargetObjectSubID;
  m_uiAppliedFaceId = plInvalidIndex;

  // stock behavior for the component types the scene plugin's handler covered
  if (pType->IsDerivedFrom<plMeshComponentBase>())
  {
    plResizeAndSetObjectPropertyCommand cmd;
    cmd.m_Object = pInfo->m_TargetComponent;
    cmd.m_Index = pInfo->m_iTargetObjectSubID;
    cmd.m_sProperty = "Materials";
    cmd.m_NewValue = GetAssetGuidString(pInfo);

    m_pDocument->GetCommandHistory()->StartTransaction("Assign Material");
    m_pDocument->GetCommandHistory()->AddCommand(cmd).AssertSuccess();
    m_pDocument->GetCommandHistory()->FinishTransaction();
  }
  else if (pType->IsDerivedFrom<plGreyBoxComponent>())
  {
    plSetObjectPropertyCommand cmd;
    cmd.m_Object = pInfo->m_TargetComponent;
    cmd.m_sProperty = "Material";
    cmd.m_NewValue = GetAssetGuidString(pInfo);

    m_pDocument->GetCommandHistory()->StartTransaction("Assign Material");
    m_pDocument->GetCommandHistory()->AddCommand(cmd).AssertSuccess();
    m_pDocument->GetCommandHistory()->FinishTransaction();
  }
}

void plBrushFaceMaterialDragDropHandler::ApplyToBrushFace(const plDragDropInfo* pInfo, const plDocumentObject* pComponent)
{
  if (pInfo->m_vDropPosition.IsNaN())
    return;

  // the EVALUATED mesh, so doorway jambs (cap faces from live cutters) are assignable too
  plEditableMesh mesh;
  if (plBrushPicking::BuildEvaluatedMeshFromObject(pComponent, mesh).Failed())
    return;

  // the drop position is world space; the mesh lives in the owner object's local space
  const plGameObjectDocument* pGameDoc = plStaticCast<const plGameObjectDocument*>(m_pDocument);
  const plTransform globalTransform = pGameDoc->GetGlobalTransform(pComponent->GetParent());
  const plVec3 vLocalPos = globalTransform.GetInverse().TransformPosition(pInfo->m_vDropPosition);

  const plUInt32 uiFaceId = plBrushPicking::PickFaceAtLocalPos(mesh, vLocalPos);

  if (uiFaceId == plInvalidIndex)
    return;

  if (m_AppliedToComponent == pInfo->m_TargetComponent && m_uiAppliedFaceId == uiFaceId)
    return;

  m_AppliedToComponent = pInfo->m_TargetComponent;
  m_iAppliedToSlot = -1;
  m_uiAppliedFaceId = uiFaceId;

  const plString sMaterialGuid = GetAssetGuidString(pInfo);
  const auto& acc = pComponent->GetTypeAccessor();

  // the material's slot: an existing entry in the Materials array, or a new one appended at the end
  plInt32 iSlot = -1;
  const plInt32 iMaterialCount = acc.GetCount("Materials");

  for (plInt32 i = 0; i < iMaterialCount; ++i)
  {
    if (acc.GetValue("Materials", i).ConvertTo<plString>() == sMaterialGuid)
    {
      iSlot = i;
      break;
    }
  }

  plHashTable<plUInt32, plUInt8> overrides;
  plBrushComponent::ParseFaceData(acc.GetValue("FaceData").ConvertTo<plDataBuffer>(), overrides);

  overrides[uiFaceId] = static_cast<plUInt8>(iSlot >= 0 ? iSlot : iMaterialCount);

  plDataBuffer newFaceData;
  plBrushComponent::WriteFaceData(overrides, newFaceData);

  m_pDocument->GetCommandHistory()->StartTransaction("Assign Material To Face");

  if (iSlot < 0)
  {
    plInsertObjectPropertyCommand cmdMat;
    cmdMat.m_Object = pInfo->m_TargetComponent;
    cmdMat.m_sProperty = "Materials";
    cmdMat.m_Index = iMaterialCount;
    cmdMat.m_NewValue = sMaterialGuid;
    m_pDocument->GetCommandHistory()->AddCommand(cmdMat).AssertSuccess();
  }

  plSetObjectPropertyCommand cmdFace;
  cmdFace.m_Object = pInfo->m_TargetComponent;
  cmdFace.m_sProperty = "FaceData";
  cmdFace.m_NewValue = newFaceData;
  m_pDocument->GetCommandHistory()->AddCommand(cmdFace).AssertSuccess();

  m_pDocument->GetCommandHistory()->FinishTransaction();
}

void plBrushFaceMaterialDragDropHandler::OnDragCancel()
{
  m_pDocument->GetCommandHistory()->CancelTemporaryCommands();
}

void plBrushFaceMaterialDragDropHandler::OnDrop(const plDragDropInfo* pInfo)
{
  if (pInfo->m_TargetComponent.IsValid())
  {
    const plDocumentObject* pComponent = m_pDocument->GetObjectManager()->GetObject(pInfo->m_TargetComponent);

    if (pComponent)
    {
      const plRTTI* pType = pComponent->GetTypeAccessor().GetType();

      if (pType->GetTypeName() == "plBrushComponent" ||
          pType->IsDerivedFrom<plMeshComponentBase>() || pType->IsDerivedFrom<plGreyBoxComponent>())
      {
        m_pDocument->GetCommandHistory()->FinishTemporaryCommands();
        return;
      }
    }
  }

  m_pDocument->GetCommandHistory()->CancelTemporaryCommands();
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_BrushFaceMaterialDragDropHandler);
