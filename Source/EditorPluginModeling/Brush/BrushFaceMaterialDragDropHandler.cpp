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
  m_PickMeshComponent = plUuid();
  m_PickMesh.Clear();

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

  RevertPreview();

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

    RunInTransaction("Assign Material", cmd);
  }
  else if (pType->IsDerivedFrom<plGreyBoxComponent>())
  {
    plSetObjectPropertyCommand cmd;
    cmd.m_Object = pInfo->m_TargetComponent;
    cmd.m_sProperty = "Material";
    cmd.m_NewValue = GetAssetGuidString(pInfo);

    RunInTransaction("Assign Material", cmd);
  }
}

void plBrushFaceMaterialDragDropHandler::RevertPreview()
{
  // the preview follows the cursor: what was assigned to the previous face or part is taken back
  if (!m_AppliedToComponent.IsValid())
    return;

  auto* pHistory = m_pDocument->GetCommandHistory();
  pHistory->CancelTemporaryCommands();
  pHistory->BeginTemporaryCommands("Drag Material", true);

  m_AppliedToComponent = plUuid();
  m_iAppliedToSlot = -1;
  m_uiAppliedFaceId = plInvalidIndex;
}

void plBrushFaceMaterialDragDropHandler::RunInTransaction(plStringView sName, plCommand& ref_cmd)
{
  plCommand* commands[] = {&ref_cmd};
  RunInTransaction(sName, commands);
}

void plBrushFaceMaterialDragDropHandler::RunInTransaction(plStringView sName, plArrayPtr<plCommand* const> commands)
{
  auto* pHistory = m_pDocument->GetCommandHistory();
  pHistory->StartTransaction(sName);

  plStatus result(PL_SUCCESS);

  for (plCommand* pCommand : commands)
  {
    if (result.Succeeded())
      result = pHistory->AddCommand(*pCommand);
  }

  if (result.Failed())
  {
    // a failed preview must not leave the transaction open, or the document blocks every later edit
    pHistory->CancelTransaction();
    plLog::Error("Assigning the material failed: {}", result.m_sMessage);
    return;
  }

  pHistory->FinishTransaction();
}

void plBrushFaceMaterialDragDropHandler::ApplyToBrushFace(const plDragDropInfo* pInfo, const plDocumentObject* pComponent)
{
  if (pInfo->m_vDropPosition.IsNaN())
    return;

  // the EVALUATED mesh, so doorway jambs (cap faces from live cutters) are assignable too
  if (m_PickMeshComponent != pComponent->GetGuid())
  {
    m_PickMeshComponent = plUuid();

    if (plBrushPicking::BuildEvaluatedMeshFromObject(pComponent, m_PickMesh).Failed())
      return;

    // the drop position is world space; the mesh lives in the owner object's local space
    const plGameObjectDocument* pGameDoc = plStaticCast<const plGameObjectDocument*>(m_pDocument);
    m_PickMeshInverseTransform = pGameDoc->GetGlobalTransform(pComponent->GetParent()).GetInverse();
    m_PickMeshComponent = pComponent->GetGuid();
  }

  const plVec3 vLocalPos = m_PickMeshInverseTransform.TransformPosition(pInfo->m_vDropPosition);
  const plUInt32 uiFaceId = plBrushPicking::PickFaceAtLocalPos(m_PickMesh, vLocalPos);

  if (uiFaceId == plInvalidIndex)
    return;

  if (m_AppliedToComponent == pInfo->m_TargetComponent && m_uiAppliedFaceId == uiFaceId)
    return;

  RevertPreview();

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

  // every face without an override uses slot 0, which shows the default material while the Materials
  // array is empty: that slot gets the default material written out, so the new one only reaches this face
  plHybridArray<plInsertObjectPropertyCommand, 2> insertMaterials;

  if (iSlot < 0 && iMaterialCount == 0)
  {
    plInsertObjectPropertyCommand& cmdDefault = insertMaterials.ExpandAndGetRef();
    cmdDefault.m_Object = pInfo->m_TargetComponent;
    cmdDefault.m_sProperty = "Materials";
    cmdDefault.m_Index = 0;
    cmdDefault.m_NewValue = plString(plBrushComponent::GetDefaultMaterialId());
  }

  if (iSlot < 0)
  {
    iSlot = iMaterialCount + static_cast<plInt32>(insertMaterials.GetCount());

    plInsertObjectPropertyCommand& cmdMat = insertMaterials.ExpandAndGetRef();
    cmdMat.m_Object = pInfo->m_TargetComponent;
    cmdMat.m_sProperty = "Materials";
    cmdMat.m_Index = iSlot;
    cmdMat.m_NewValue = sMaterialGuid;
  }

  plBrushFaceAttributeMap attributes;
  plBrushPicking::ReadFaceAttributes(pComponent, attributes);

  plBrushFaceAttributes& attr = attributes[uiFaceId];
  attr.m_uiMask |= plBrushFaceAttributes::Slot;
  attr.m_uiMaterialSlot = static_cast<plUInt8>(iSlot);

  plDataBuffer newFaceData;
  plBrushFaceAttributeCodec::Write(attributes, newFaceData);

  plSetObjectPropertyCommand cmdFace;
  cmdFace.m_Object = pInfo->m_TargetComponent;
  cmdFace.m_sProperty = "FaceData";
  cmdFace.m_NewValue = newFaceData;

  plHybridArray<plCommand*, 3> commands;
  for (plInsertObjectPropertyCommand& cmd : insertMaterials)
    commands.PushBack(&cmd);
  commands.PushBack(&cmdFace);

  RunInTransaction("Assign Material To Face", commands.GetArrayPtr());
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
