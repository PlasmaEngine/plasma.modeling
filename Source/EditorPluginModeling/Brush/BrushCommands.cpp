#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <Core/World/GameObject.h>
#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorPluginModeling/Brush/BrushCommands.h>
#include <EditorPluginModeling/Brush/BrushPicking.h>
#include <ModelingPlugin/Modeling/MeshCSG.h>
#include <ModelingPlugin/Modeling/MeshOps.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <ToolsFoundation/CommandHistory/CommandHistory.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

namespace
{
  /// Opens a transaction unless one is already open, in which case the commands join it.
  class ScopedTransaction
  {
  public:
    ScopedTransaction(plDocument* pDocument, plStringView sName)
      : m_pHistory(pDocument->GetCommandHistory())
    {
      m_bOwned = !m_pHistory->IsInTransaction();

      if (m_bOwned)
        m_pHistory->StartTransaction(sName);
    }

    ~ScopedTransaction()
    {
      if (m_bOwned && !m_bDone)
        m_pHistory->CancelTransaction();
    }

    void Commit()
    {
      if (m_bOwned)
        m_pHistory->FinishTransaction();

      m_bDone = true;
    }

    plCommandHistory* m_pHistory;

  private:
    bool m_bOwned = false;
    bool m_bDone = false;
  };

  plTransform GetGlobal(const plDocumentObject* pBrush)
  {
    return plBrushPicking::GetComponentGlobalTransform(pBrush);
  }

  plString GetMaterialGuid(const plDocumentObject* pBrush, plUInt32 uiSlot)
  {
    const auto& acc = pBrush->GetTypeAccessor();

    if (static_cast<plInt32>(uiSlot) >= acc.GetCount("Materials"))
      return plString();

    return acc.GetValue("Materials", uiSlot).ConvertTo<plString>();
  }

  /// The slot holding sMaterial on the brush, appending it (as a command) when absent.
  plUInt8 FindOrAddMaterialSlot(plCommandHistory* pHistory, const plDocumentObject* pBrush, plStringView sMaterial, plDynamicArray<plString>& ref_pendingMaterials)
  {
    // the pending list mirrors the Materials array including the inserts issued in this transaction
    for (plUInt32 i = 0; i < ref_pendingMaterials.GetCount(); ++i)
    {
      if (ref_pendingMaterials[i] == sMaterial)
        return static_cast<plUInt8>(i);
    }

    plInsertObjectPropertyCommand cmd;
    cmd.m_Object = pBrush->GetGuid();
    cmd.m_sProperty = "Materials";
    cmd.m_Index = static_cast<plInt32>(ref_pendingMaterials.GetCount());
    cmd.m_NewValue = plString(sMaterial);
    pHistory->AddCommand(cmd).LogFailure();

    ref_pendingMaterials.PushBack(sMaterial);
    return static_cast<plUInt8>(ref_pendingMaterials.GetCount() - 1);
  }

  void ReadMaterials(const plDocumentObject* pBrush, plDynamicArray<plString>& out_materials)
  {
    out_materials.Clear();
    const auto& acc = pBrush->GetTypeAccessor();

    for (plInt32 i = 0; i < acc.GetCount("Materials"); ++i)
    {
      out_materials.PushBack(acc.GetValue("Materials", i).ConvertTo<plString>());
    }
  }

  void SetProperty(plCommandHistory* pHistory, const plUuid& object, plStringView sProperty, const plVariant& value)
  {
    plSetObjectPropertyCommand cmd;
    cmd.m_Object = object;
    cmd.m_sProperty = sProperty;
    cmd.m_NewValue = value;
    pHistory->AddCommand(cmd).LogFailure();
  }

  void RemoveObject(plCommandHistory* pHistory, const plDocumentObject* pBrush)
  {
    plRemoveObjectCommand cmd;
    cmd.m_Object = pBrush->GetParent()->GetGuid();
    pHistory->AddCommand(cmd).LogFailure();
  }

  /// A new game object with a brush component as a sibling of pSource's object, copying its settings.
  plUuid AddSiblingBrush(plDocument* pDocument, const plDocumentObject* pSource, plStringView sName, const plEditableMesh& mesh)
  {
    plCommandHistory* pHistory = pDocument->GetCommandHistory();
    const plDocumentObject* pSourceObject = pSource->GetParent();
    const plDocumentObject* pRoot = pDocument->GetObjectManager()->GetRootObject();

    const plUuid objGuid = plUuid::MakeUuid();
    const plUuid compGuid = plUuid::MakeUuid();

    {
      plAddObjectCommand cmd;
      cmd.m_NewObjectGuid = objGuid;
      cmd.m_pType = plGetStaticRTTI<plGameObject>();
      cmd.m_sParentProperty = "Children";
      cmd.m_Index = -1;
      if (pSourceObject->GetParent() != pRoot)
        cmd.m_Parent = pSourceObject->GetParent()->GetGuid();
      pHistory->AddCommand(cmd).LogFailure();
    }

    SetProperty(pHistory, objGuid, "Name", plString(sName));

    for (const char* szTransformProp : {"LocalPosition", "LocalRotation", "LocalScaling", "LocalUniformScaling"})
    {
      SetProperty(pHistory, objGuid, szTransformProp, pSourceObject->GetTypeAccessor().GetValue(szTransformProp));
    }

    {
      plAddObjectCommand cmd;
      cmd.m_NewObjectGuid = compGuid;
      cmd.m_pType = plRTTI::FindTypeByName("plBrushComponent");
      cmd.m_sParentProperty = "Components";
      cmd.m_Parent = objGuid;
      cmd.m_Index = -1;
      pHistory->AddCommand(cmd).LogFailure();
    }

    const auto& acc = pSource->GetTypeAccessor();

    for (const char* szProp : {"Subtractive", "Color", "CustomData", "SmoothingAngle", "TextureLock", "GenerateCollision", "IncludeInNavmesh", "UseAsOccluder",
           "MergeOnExport", "FaceData"})
    {
      SetProperty(pHistory, compGuid, szProp, acc.GetValue(szProp));
    }

    for (plInt32 i = 0; i < acc.GetCount("Materials"); ++i)
    {
      plInsertObjectPropertyCommand cmd;
      cmd.m_Object = compGuid;
      cmd.m_sProperty = "Materials";
      cmd.m_Index = i;
      cmd.m_NewValue = acc.GetValue("Materials", i);
      pHistory->AddCommand(cmd).LogFailure();
    }

    plDataBuffer blob;
    mesh.ToDataBuffer(blob);
    SetProperty(pHistory, compGuid, "Shape", static_cast<plInt64>(plBrushShape::Editable));
    SetProperty(pHistory, compGuid, "StoredMesh", blob);

    return compGuid;
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// queries

void plBrushCommands::GatherBrushes(plArrayPtr<const plDocumentObject* const> objects, plDynamicArray<const plDocumentObject*>& out_brushes)
{
  out_brushes.Clear();

  for (const plDocumentObject* pObject : objects)
  {
    if (plBrushPicking::IsBrushComponent(pObject))
    {
      if (!out_brushes.Contains(pObject))
        out_brushes.PushBack(pObject);
      continue;
    }

    for (const plDocumentObject* pChild : pObject->GetChildren())
    {
      if (plBrushPicking::IsBrushComponent(pChild) && !out_brushes.Contains(pChild))
      {
        out_brushes.PushBack(pChild);
      }
    }
  }
}

void plBrushCommands::GatherSelectedBrushes(const plDocument* pDocument, plDynamicArray<const plDocumentObject*>& out_brushes)
{
  plHybridArray<const plDocumentObject*, 16> selection;
  for (const plDocumentObject* pObject : pDocument->GetSelectionManager()->GetSelection())
    selection.PushBack(pObject);

  GatherBrushes(selection, out_brushes);
}

const plDocumentObject* plBrushCommands::FindGroupObject(const plDocumentObject* pObject)
{
  const plDocumentObject* pRoot = pObject->GetDocumentObjectManager()->GetRootObject();

  for (const plDocumentObject* pCurrent = pObject; pCurrent != nullptr && pCurrent != pRoot; pCurrent = pCurrent->GetParent())
  {
    for (const plDocumentObject* pChild : pCurrent->GetChildren())
    {
      if (pChild->GetTypeAccessor().GetType()->GetTypeName() == "plBrushGroupComponent")
        return pCurrent;
    }
  }

  return nullptr;
}

//////////////////////////////////////////////////////////////////////////
// mesh edits

void plBrushCommands::WriteMesh(plCommandHistory* pHistory, const plDocumentObject* pBrush, const plEditableMesh& mesh)
{
  if (pBrush->GetTypeAccessor().GetValue("Shape").ConvertTo<plInt32>() != plBrushShape::Editable)
  {
    SetProperty(pHistory, pBrush->GetGuid(), "Shape", static_cast<plInt64>(plBrushShape::Editable));
  }

  plDataBuffer blob;
  mesh.ToDataBuffer(blob);
  SetProperty(pHistory, pBrush->GetGuid(), "StoredMesh", blob);
}

plStatus plBrushCommands::SeparateFaceIds(plDocument* pDocument, const plDocumentObject* pBrush, plArrayPtr<const plUInt32> faceIndices, plDynamicArray<plUInt32>& out_faceIds)
{
  out_faceIds.Clear();

  plEditableMesh mesh;
  if (plBrushPicking::BuildMeshFromObject(pBrush, mesh).Failed())
    return plStatus("The brush's stored mesh is damaged and cannot be edited.");

  plHashTable<plUInt32, plUInt32> useCount;
  for (const plBrushFace& face : mesh.m_Faces)
    ++useCount[face.m_uiFaceId];

  plBrushFaceAttributeMap attributes;
  plBrushPicking::ReadFaceAttributes(pBrush, attributes);

  bool bChanged = false;
  bool bAttributesChanged = false;

  for (plUInt32 uiFace : faceIndices)
  {
    if (uiFace >= mesh.m_Faces.GetCount())
      return plStatus("The face selection is out of date.");

    plBrushFace& face = mesh.m_Faces[uiFace];
    plUInt32& uiUses = useCount[face.m_uiFaceId];

    // the last polygon carrying an id keeps it
    if (uiUses > 1)
    {
      --uiUses;

      const plUInt32 uiOldId = face.m_uiFaceId;
      const plUInt32 uiNewId = mesh.m_uiNextFaceId++;
      face.m_uiFaceId = uiNewId;
      useCount[uiNewId] = 1;
      bChanged = true;

      if (const plBrushFaceAttributes* pAttr = attributes.GetValue(uiOldId))
      {
        const plBrushFaceAttributes copy = *pAttr; // the insert below may move the table's storage
        attributes[uiNewId] = copy;
        bAttributesChanged = true;
      }
    }

    out_faceIds.PushBack(face.m_uiFaceId);
  }

  if (!bChanged)
    return plStatus(PL_SUCCESS);

  ScopedTransaction transaction(pDocument, "Separate Faces");
  WriteMesh(transaction.m_pHistory, pBrush, mesh);

  if (bAttributesChanged)
  {
    plDataBuffer blob;
    plBrushFaceAttributeCodec::Write(attributes, blob);
    SetProperty(transaction.m_pHistory, pBrush->GetGuid(), "FaceData", blob);
  }

  transaction.Commit();
  return plStatus(PL_SUCCESS);
}

plStatus plBrushCommands::EditMesh(plDocument* pDocument, const plDocumentObject* pBrush, plStringView sActionName, MeshEditFunc func)
{
  plEditableMesh mesh;

  if (plBrushPicking::BuildMeshFromObject(pBrush, mesh).Failed())
    return plStatus("The brush's stored mesh is damaged and cannot be edited.");

  plStatus result = func(mesh);

  if (result.Failed())
    return result;

  ScopedTransaction transaction(pDocument, sActionName);
  WriteMesh(transaction.m_pHistory, pBrush, mesh);
  transaction.Commit();

  return plStatus(PL_SUCCESS);
}

//////////////////////////////////////////////////////////////////////////
// face attributes

plStatus plBrushCommands::EditFaceAttributes(plDocument* pDocument, const plDocumentObject* pBrush, const plHashSet<plUInt32>& faceIds, plStringView sActionName, AttributeEditFunc func)
{
  if (faceIds.IsEmpty())
    return plStatus("No faces selected.");

  plBrushFaceAttributeMap attributes;
  plBrushPicking::ReadFaceAttributes(pBrush, attributes);

  for (plUInt32 uiFaceId : faceIds)
  {
    func(uiFaceId, attributes[uiFaceId]);
  }

  plDataBuffer blob;
  plBrushFaceAttributeCodec::Write(attributes, blob);

  ScopedTransaction transaction(pDocument, sActionName);
  SetProperty(transaction.m_pHistory, pBrush->GetGuid(), "FaceData", blob);
  transaction.Commit();

  return plStatus(PL_SUCCESS);
}

plStatus plBrushCommands::SetFaceMaterial(plDocument* pDocument, const plDocumentObject* pBrush, const plHashSet<plUInt32>& faceIds, plStringView sMaterialGuid)
{
  if (faceIds.IsEmpty())
    return plStatus("No faces selected.");

  plBrushFaceAttributeMap attributes;
  plBrushPicking::ReadFaceAttributes(pBrush, attributes);

  ScopedTransaction transaction(pDocument, "Assign Material To Faces");

  plDynamicArray<plString> materials;
  ReadMaterials(pBrush, materials);

  const plUInt8 uiSlot = FindOrAddMaterialSlot(transaction.m_pHistory, pBrush, sMaterialGuid, materials);

  for (plUInt32 uiFaceId : faceIds)
  {
    plBrushFaceAttributes& attr = attributes[uiFaceId];
    attr.m_uiMask |= plBrushFaceAttributes::Slot;
    attr.m_uiMaterialSlot = uiSlot;
  }

  plDataBuffer blob;
  plBrushFaceAttributeCodec::Write(attributes, blob);
  SetProperty(transaction.m_pHistory, pBrush->GetGuid(), "FaceData", blob);

  transaction.Commit();
  return plStatus(PL_SUCCESS);
}

plStatus plBrushCommands::SetBrushMaterial(plDocument* pDocument, const plDocumentObject* pBrush, plStringView sMaterialGuid)
{
  plBrushFaceAttributeMap attributes;
  plBrushPicking::ReadFaceAttributes(pBrush, attributes);

  ScopedTransaction transaction(pDocument, "Assign Material");

  if (pBrush->GetTypeAccessor().GetCount("Materials") > 0)
  {
    plSetObjectPropertyCommand cmd;
    cmd.m_Object = pBrush->GetGuid();
    cmd.m_sProperty = "Materials";
    cmd.m_Index = 0;
    cmd.m_NewValue = plString(sMaterialGuid);
    transaction.m_pHistory->AddCommand(cmd).LogFailure();
  }
  else
  {
    plInsertObjectPropertyCommand cmd;
    cmd.m_Object = pBrush->GetGuid();
    cmd.m_sProperty = "Materials";
    cmd.m_Index = 0;
    cmd.m_NewValue = plString(sMaterialGuid);
    transaction.m_pHistory->AddCommand(cmd).LogFailure();
  }

  // every face back on slot 0; UV and edge overrides stay
  bool bChanged = false;
  for (auto it : attributes)
  {
    if (it.Value().m_uiMask & plBrushFaceAttributes::Slot)
    {
      it.Value().m_uiMask &= ~plBrushFaceAttributes::Slot;
      bChanged = true;
    }
  }

  if (bChanged)
  {
    plDataBuffer blob;
    plBrushFaceAttributeCodec::Write(attributes, blob);
    SetProperty(transaction.m_pHistory, pBrush->GetGuid(), "FaceData", blob);
  }

  transaction.Commit();
  return plStatus(PL_SUCCESS);
}

//////////////////////////////////////////////////////////////////////////
// CSG

plStatus plBrushCommands::ToggleCutter(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> brushes)
{
  if (brushes.IsEmpty())
    return plStatus("Select one or more brushes.");

  ScopedTransaction transaction(pDocument, "Toggle Cutter");

  for (const plDocumentObject* pBrush : brushes)
  {
    SetProperty(transaction.m_pHistory, pBrush->GetGuid(), "Subtractive", !pBrush->GetTypeAccessor().GetValue("Subtractive").ConvertTo<bool>());
  }

  transaction.Commit();
  return plStatus(PL_SUCCESS);
}

plStatus plBrushCommands::BakeCuts(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> brushes)
{
  plHybridArray<const plDocumentObject*, 4> cutters;

  for (const plDocumentObject* pBrush : brushes)
  {
    if (pBrush->GetTypeAccessor().GetValue("Subtractive").ConvertTo<bool>())
      cutters.PushBack(pBrush);
  }

  if (cutters.IsEmpty())
    return plStatus("Bake Cuts: select a cutter (a subtractive brush) first. Cutters already cut live; baking makes the cut permanent and removes the cutter.");

  // the selected cutters, as the evaluator takes them: in evaluation order, so cut-face ids match the
  // live cut and materials on jambs survive the bake
  struct CutterEntry
  {
    plBrushCutterInput m_Input;
    const plDocumentObject* m_pScope = nullptr;
  };

  plDynamicArray<CutterEntry> cutterEntries;

  for (const plDocumentObject* pCutter : cutters)
  {
    CutterEntry entry;
    plBrushCutterInput& input = entry.m_Input;

    if (plBrushPicking::BuildMeshFromObject(pCutter, input.m_Mesh).Failed() || input.m_Mesh.m_Faces.IsEmpty())
      continue;

    plEditableMesh pieces;
    if (plMeshCSG::MakeConvexCutter(input.m_Mesh, pieces).Failed())
      return plStatus("Bake Cuts: the cutter's shape cannot be split into convex pieces. Check it for open edges in the Health section.");

    input.m_GlobalTransform = GetGlobal(pCutter);
    input.m_uiBrushId = pCutter->GetTypeAccessor().GetValue("BrushId").ConvertTo<plUInt32>();
    input.m_WorldBounds = input.m_Mesh.ComputeBounds();
    input.m_WorldBounds.TransformFromOrigin(input.m_GlobalTransform.GetAsMat4());
    entry.m_pScope = plBrushPicking::FindCutterScope(pCutter);
    cutterEntries.PushBack(std::move(entry));
  }

  cutterEntries.Sort([](const CutterEntry& a, const CutterEntry& b)
    { return plBrushEvaluator::CutterLess(a.m_Input.m_uiBrushId, a.m_Input.m_GlobalTransform.m_vPosition, b.m_Input.m_uiBrushId, b.m_Input.m_GlobalTransform.m_vPosition); });

  // results are computed first; a refused cutter aborts before anything changes
  struct TargetResult
  {
    const plDocumentObject* m_pTarget = nullptr;
    plEditableMesh m_Mesh;
  };

  plDynamicArray<TargetResult> results;
  plDynamicArray<const plDocumentObject*> allBrushes;
  plBrushPicking::FindAllBrushComponents(cutters[0], allBrushes);

  plDynamicArray<plBrushCutterInput> scopedCutters;

  for (const plDocumentObject* pTarget : allBrushes)
  {
    if (pTarget->GetTypeAccessor().GetValue("Subtractive").ConvertTo<bool>())
      continue;

    // cutters only cut brushes in their own scope
    const plDocumentObject* pScope = plBrushPicking::FindCutterScope(pTarget);
    scopedCutters.Clear();

    for (const CutterEntry& entry : cutterEntries)
    {
      if (entry.m_pScope == pScope)
        scopedCutters.PushBack(entry.m_Input);
    }

    plEditableMesh targetMesh;
    if (scopedCutters.IsEmpty() || plBrushPicking::BuildMeshFromObject(pTarget, targetMesh).Failed() || targetMesh.m_Faces.IsEmpty())
      continue;

    plEditableMesh result = targetMesh;
    plBrushPicking::CarveMesh(pTarget, result, scopedCutters);

    plDataBuffer before, after;
    targetMesh.ToDataBuffer(before);
    result.ToDataBuffer(after);

    if (before == after)
      continue;

    TargetResult& r = results.ExpandAndGetRef();
    r.m_pTarget = pTarget;
    r.m_Mesh = std::move(result);
  }

  ScopedTransaction transaction(pDocument, "Bake Cuts");

  for (const TargetResult& r : results)
  {
    WriteMesh(transaction.m_pHistory, r.m_pTarget, r.m_Mesh);
  }

  for (const plDocumentObject* pCutter : cutters)
  {
    RemoveObject(transaction.m_pHistory, pCutter);
  }

  transaction.Commit();
  return plStatus(PL_SUCCESS);
}

plStatus plBrushCommands::Hollow(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> brushes)
{
  if (brushes.IsEmpty())
    return plStatus("Select one or more brushes.");

  plDynamicArray<plEditableMesh> results;
  plEditableMesh mesh;

  for (const plDocumentObject* pBrush : brushes)
  {
    if (plBrushPicking::BuildMeshFromObject(pBrush, mesh).Failed())
      return plStatus("Hollow: a brush's stored mesh is damaged.");

    const float fThickness = pBrush->GetTypeAccessor().GetValue("Thickness").ConvertTo<float>();

    if (plMeshCSG::MakeHollow(mesh, fThickness, 0, results.ExpandAndGetRef()).Failed())
      return plStatus(plFmt("Hollow: the brush is not convex, or too small for walls {} thick (the Thickness property).", fThickness));
  }

  ScopedTransaction transaction(pDocument, "Hollow");

  for (plUInt32 i = 0; i < brushes.GetCount(); ++i)
  {
    WriteMesh(transaction.m_pHistory, brushes[i], results[i]);
  }

  transaction.Commit();
  return plStatus(PL_SUCCESS);
}

plStatus plBrushCommands::Union(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> brushes)
{
  if (brushes.GetCount() < 2)
    return plStatus("Union: select two or more brushes. The first one keeps the result.");

  const plDocumentObject* pTarget = brushes[0];

  plEditableMesh result;
  if (plBrushPicking::BuildMeshFromObject(pTarget, result).Failed())
    return plStatus("Union: the first brush's stored mesh is damaged.");

  // the target's own overrides are baked in so every face carries its final slot
  plBrushFaceAttributeMap targetAttributes;
  plBrushPicking::ReadFaceAttributes(pTarget, targetAttributes);
  plBrushFaceAttributeCodec::Apply(targetAttributes, result);

  ScopedTransaction transaction(pDocument, "Union");

  plDynamicArray<plString> materials;
  ReadMaterials(pTarget, materials);

  const plTransform targetTransform = GetGlobal(pTarget);
  plEditableMesh other, merged;

  for (plUInt32 i = 1; i < brushes.GetCount(); ++i)
  {
    const plDocumentObject* pOther = brushes[i];

    if (plBrushPicking::BuildMeshFromObject(pOther, other).Failed())
      return plStatus("Union: a brush's stored mesh is damaged.");

    plBrushFaceAttributeMap attributes;
    plBrushPicking::ReadFaceAttributes(pOther, attributes);
    plBrushFaceAttributeCodec::Apply(attributes, other);

    // the other brush's slots move into the target's material list
    for (plBrushFace& face : other.m_Faces)
    {
      face.m_uiMaterialSlot = FindOrAddMaterialSlot(transaction.m_pHistory, pTarget, GetMaterialGuid(pOther, face.m_uiMaterialSlot), materials);
    }

    if (plMeshCSG::Union(result, other, plTransform::MakeLocalTransform(targetTransform, GetGlobal(pOther)), merged).Failed())
      return plStatus("Union: neither brush is convex. Decompose one of them first.");

    result = merged;
    RemoveObject(transaction.m_pHistory, pOther);
  }

  // overrides are baked into the mesh now
  SetProperty(transaction.m_pHistory, pTarget->GetGuid(), "FaceData", plDataBuffer());
  WriteMesh(transaction.m_pHistory, pTarget, result);

  transaction.Commit();
  return plStatus(PL_SUCCESS);
}

plStatus plBrushCommands::Decompose(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> brushes)
{
  if (brushes.IsEmpty())
    return plStatus("Select one or more brushes.");

  plDynamicArray<plEditableMesh> results;
  plEditableMesh mesh;

  for (const plDocumentObject* pBrush : brushes)
  {
    if (plBrushPicking::BuildMeshFromObject(pBrush, mesh).Failed())
      return plStatus("Decompose: a brush's stored mesh is damaged.");

    plMeshCSG::DecomposeConvex(mesh, results.ExpandAndGetRef());
  }

  ScopedTransaction transaction(pDocument, "Decompose Into Convex Pieces");

  for (plUInt32 i = 0; i < brushes.GetCount(); ++i)
  {
    WriteMesh(transaction.m_pHistory, brushes[i], results[i]);
  }

  transaction.Commit();
  return plStatus(PL_SUCCESS);
}

plStatus plBrushCommands::Clip(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> brushes, const plPlane& worldPlane, ClipKeep::Enum keep)
{
  if (brushes.IsEmpty())
    return plStatus("Clip: select one or more brushes.");

  struct ClipResult
  {
    const plDocumentObject* m_pBrush = nullptr;
    plEditableMesh m_Front;
    plEditableMesh m_Back;
  };

  plDynamicArray<ClipResult> results;
  plEditableMesh mesh;

  for (const plDocumentObject* pBrush : brushes)
  {
    if (plBrushPicking::BuildMeshFromObject(pBrush, mesh).Failed())
      return plStatus("Clip: a brush's stored mesh is damaged.");

    // the plane in the brush's local space; normals scale with S (inverse transpose of S^-1)
    const plTransform global = GetGlobal(pBrush);
    const plVec3 vLocalPoint = global.GetInverse().TransformPosition(worldPlane.m_vNormal * -worldPlane.m_fNegDistance);
    plVec3 vLocalNormal = global.m_qRotation.GetInverse() * worldPlane.m_vNormal;
    vLocalNormal = vLocalNormal.CompMul(global.m_vScale);
    vLocalNormal.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();

    const plPlane localPlane = plPlane::MakeFromNormalAndPoint(vLocalNormal, vLocalPoint);

    ClipResult& r = results.ExpandAndGetRef();
    r.m_pBrush = pBrush;

    plUInt32 uiNextFaceId = mesh.m_uiNextFaceId;
    plMeshCSG::ClipByPlane(mesh, localPlane, true, 0, uiNextFaceId, r.m_Front);
    plMeshCSG::ClipByPlane(mesh, localPlane, false, 0, uiNextFaceId, r.m_Back);
  }

  ScopedTransaction transaction(pDocument, "Clip");

  for (ClipResult& r : results)
  {
    const plEditableMesh& kept = (keep == ClipKeep::Back) ? r.m_Back : r.m_Front;

    if (kept.m_Faces.IsEmpty())
    {
      if (keep != ClipKeep::Both || r.m_Back.m_Faces.IsEmpty())
      {
        RemoveObject(transaction.m_pHistory, r.m_pBrush);
        continue;
      }
    }

    if (keep == ClipKeep::Both)
    {
      if (r.m_Front.m_Faces.IsEmpty())
      {
        WriteMesh(transaction.m_pHistory, r.m_pBrush, r.m_Back);
        continue;
      }

      WriteMesh(transaction.m_pHistory, r.m_pBrush, r.m_Front);

      if (!r.m_Back.m_Faces.IsEmpty())
      {
        plStringBuilder sName = r.m_pBrush->GetParent()->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
        sName.Append(" (clipped)");
        AddSiblingBrush(pDocument, r.m_pBrush, sName, r.m_Back);
      }
    }
    else
    {
      WriteMesh(transaction.m_pHistory, r.m_pBrush, kept);
    }
  }

  transaction.Commit();
  return plStatus(PL_SUCCESS);
}

plStatus plBrushCommands::Detach(plDocument* pDocument, const plDocumentObject* pBrush, const plHashSet<plUInt32>& faceIds, plUuid* out_pNewBrush)
{
  plEditableMesh mesh, detached;

  if (plBrushPicking::BuildMeshFromObject(pBrush, mesh).Failed())
    return plStatus("Detach: the brush's stored mesh is damaged.");

  plStatus status = plMeshOps::Detach(mesh, faceIds, detached);
  if (status.Failed())
    return status;

  ScopedTransaction transaction(pDocument, "Detach Faces");

  WriteMesh(transaction.m_pHistory, pBrush, mesh);

  plStringBuilder sName = pBrush->GetParent()->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
  sName.Append(" (detached)");
  const plUuid newBrush = AddSiblingBrush(pDocument, pBrush, sName, detached);

  if (out_pNewBrush)
    *out_pNewBrush = newBrush;

  transaction.Commit();
  return plStatus(PL_SUCCESS);
}

//////////////////////////////////////////////////////////////////////////
// pivot

namespace
{
  /// Texture-locked UVs are projected from brush-local positions, which a pivot move shifts by vShift.
  /// Offsets the projections by the same amount so every texture stays on its spot of the surface: in
  /// the face's UV override where it has one, else in the stored mesh, else as a new override. Returns
  /// whether the attribute map changed.
  bool CompensateTextureShift(const plDocumentObject* pBrush, const plVec3& vShift, plEditableMesh* pStoredMesh, plBrushFaceAttributeMap& ref_attributes)
  {
    plEditableMesh mesh;
    if (plBrushPicking::BuildEvaluatedMeshFromObject(pBrush, mesh).Failed())
      return false;

    plHashTable<plUInt32, plVec2> storedShifts;
    plHashSet<plUInt32> done;
    bool bAttributesChanged = false;

    for (plUInt32 f = 0; f < mesh.m_Faces.GetCount(); ++f)
    {
      const plBrushFace& face = mesh.m_Faces[f];

      // imported per-corner UVs do not depend on positions; fragments share their face's id
      if ((face.m_uiFlags & plBrushFaceFlags::ExplicitUV) || done.Insert(face.m_uiFaceId))
        continue;

      plVec3 vU, vV;
      mesh.GetFaceUVAxes(f, mesh.ComputeFaceNormal(f), vU, vV);

      const plVec2 vDelta(vShift.Dot(vU) * face.m_UV.m_vScale.x, vShift.Dot(vV) * face.m_UV.m_vScale.y);

      if (vDelta.IsZero(0.000001f))
        continue;

      plBrushFaceAttributes* pAttr = ref_attributes.GetValue(face.m_uiFaceId);

      if (pAttr != nullptr && (pAttr->m_uiMask & plBrushFaceAttributes::UV))
      {
        pAttr->m_UV.m_vOffset += vDelta;
        bAttributesChanged = true;
      }
      else if (pStoredMesh != nullptr && !plBrushFaceIds::IsDerived(face.m_uiFaceId))
      {
        storedShifts.Insert(face.m_uiFaceId, vDelta);
      }
      else
      {
        plBrushFaceAttributes& attr = ref_attributes[face.m_uiFaceId];
        attr.m_uiMask |= plBrushFaceAttributes::UV;
        attr.m_UV = face.m_UV;
        attr.m_UV.m_vOffset += vDelta;
        bAttributesChanged = true;
      }
    }

    if (pStoredMesh != nullptr && !storedShifts.IsEmpty())
    {
      for (plBrushFace& face : pStoredMesh->m_Faces)
      {
        if (const plVec2* pDelta = storedShifts.GetValue(face.m_uiFaceId))
          face.m_UV.m_vOffset += *pDelta;
      }
    }

    return bAttributesChanged;
  }
} // namespace

plStatus plBrushCommands::SetPivot(plDocument* pDocument, const plDocumentObject* pBrush, const plVec3& vWorldPivot)
{
  plGameObjectDocument* pGameDoc = plDynamicCast<plGameObjectDocument*>(pDocument);
  const plDocumentObject* pOwner = pBrush->GetParent();

  if (pGameDoc == nullptr || pOwner == nullptr)
    return plStatus("The brush has no object to move.");

  const plTransform global = pGameDoc->GetGlobalTransform(pOwner);

  // the new origin in brush-local space; the geometry shifts the opposite way
  const plVec3 vShift = global.GetInverse().TransformPosition(vWorldPivot);

  if (!vShift.IsValid())
    return plStatus("The brush's object is scaled to zero; its pivot cannot move.");

  if (vShift.IsZero(0.00001f))
    return plStatus(PL_SUCCESS);

  const auto& acc = pBrush->GetTypeAccessor();
  const bool bEditable = acc.GetValue("Shape").ConvertTo<plInt32>() == plBrushShape::Editable;

  plEditableMesh stored;
  if (bEditable && plBrushPicking::BuildMeshFromObject(pBrush, stored).Failed())
    return plStatus("The brush's stored mesh is damaged and cannot be edited.");

  plBrushFaceAttributeMap attributes;
  plBrushPicking::ReadFaceAttributes(pBrush, attributes);

  bool bAttributesChanged = false;

  // cutters never render, so their texture placement does not matter
  if (acc.GetValue("TextureLock").ConvertTo<bool>() && !acc.GetValue("Subtractive").ConvertTo<bool>())
  {
    bAttributesChanged = CompensateTextureShift(pBrush, vShift, bEditable ? &stored : nullptr, attributes);
  }

  ScopedTransaction transaction(pDocument, "Move Brush Pivot");
  plCommandHistory* pHistory = transaction.m_pHistory;

  if (bEditable)
  {
    for (plVec3& v : stored.m_Vertices)
    {
      v -= vShift;
    }

    WriteMesh(pHistory, pBrush, stored);
  }
  else
  {
    // the generator places the shape between the extents, so moving both ends of an axis moves the
    // shape and it stays parametric; extents may go negative when the pivot leaves the brush
    const char* szNeg[3] = {"SizeNegX", "SizeNegY", "SizeNegZ"};
    const char* szPos[3] = {"SizePosX", "SizePosY", "SizePosZ"};
    const float fShift[3] = {vShift.x, vShift.y, vShift.z};

    for (plUInt32 i = 0; i < 3; ++i)
    {
      SetProperty(pHistory, pBrush->GetGuid(), szNeg[i], acc.GetValue(szNeg[i]).ConvertTo<float>() + fShift[i]);
      SetProperty(pHistory, pBrush->GetGuid(), szPos[i], acc.GetValue(szPos[i]).ConvertTo<float>() - fShift[i]);
    }
  }

  if (bAttributesChanged)
  {
    plDataBuffer blob;
    plBrushFaceAttributeCodec::Write(attributes, blob);
    SetProperty(pHistory, pBrush->GetGuid(), "FaceData", blob);
  }

  // child objects keep their place in the world
  plHybridArray<const plDocumentObject*, 16> children;
  plHybridArray<plTransform, 16> childTransforms;

  for (const plDocumentObject* pChild : pOwner->GetChildren())
  {
    if (pChild->GetTypeAccessor().GetType()->IsDerivedFrom<plGameObject>())
    {
      children.PushBack(pChild);
      childTransforms.PushBack(pGameDoc->GetGlobalTransform(pChild));
    }
  }

  plTransform newGlobal = global;
  newGlobal.m_vPosition = global.TransformPosition(vShift);
  pGameDoc->SetGlobalTransform(pOwner, newGlobal, TransformationChanges::Translation);

  for (plUInt32 i = 0; i < children.GetCount(); ++i)
  {
    pGameDoc->SetGlobalTransform(children[i], childTransforms[i], TransformationChanges::All);
  }

  transaction.Commit();
  return plStatus(PL_SUCCESS);
}

plStatus plBrushCommands::PlacePivot(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> brushes, PivotPlacement::Enum placement)
{
  if (brushes.IsEmpty())
    return plStatus("Select one or more brushes first.");

  ScopedTransaction transaction(pDocument, placement == PivotPlacement::Center ? "Pivot To Center" : "Pivot To Bottom");

  for (const plDocumentObject* pBrush : brushes)
  {
    plEditableMesh mesh;
    if (plBrushPicking::BuildMeshFromObject(pBrush, mesh).Failed() || mesh.m_Vertices.IsEmpty())
      continue;

    // the brush's own bounds: for a rotated brush "bottom" is its local -Z side
    const plBoundingBox bounds = mesh.ComputeBounds();
    plVec3 vLocalPivot = bounds.GetCenter();

    if (placement == PivotPlacement::Bottom)
      vLocalPivot.z = bounds.m_vMin.z;

    const plStatus result = SetPivot(pDocument, pBrush, GetGlobal(pBrush).TransformPosition(vLocalPivot));

    if (result.Failed())
      return result;
  }

  transaction.Commit();
  return plStatus(PL_SUCCESS);
}

//////////////////////////////////////////////////////////////////////////
// creation

plUuid plBrushCommands::CreateBrush(plDocument* pDocument, const BrushDesc& desc)
{
  plGameObjectDocument* pGameDoc = plDynamicCast<plGameObjectDocument*>(pDocument);

  if (pGameDoc == nullptr)
    return plUuid();

  ScopedTransaction transaction(pDocument, "Add Brush");
  plCommandHistory* pHistory = transaction.m_pHistory;

  const plUuid objGuid = plUuid::MakeUuid();
  const plUuid compGuid = plUuid::MakeUuid();

  // placement relative to the parent, so the brush lands where it was asked to in the world
  plTransform local = desc.m_GlobalTransform;

  if (desc.m_Parent.IsValid())
  {
    if (const plDocumentObject* pParent = pDocument->GetObjectManager()->GetObject(desc.m_Parent))
    {
      local = plTransform::MakeLocalTransform(pGameDoc->GetGlobalTransform(pParent), desc.m_GlobalTransform);
    }
  }

  {
    plAddObjectCommand cmd;
    cmd.m_NewObjectGuid = objGuid;
    cmd.m_pType = plGetStaticRTTI<plGameObject>();
    cmd.m_sParentProperty = "Children";
    cmd.m_Parent = desc.m_Parent;
    cmd.m_Index = -1;

    if (pHistory->AddCommand(cmd).Failed())
      return plUuid();
  }

  SetProperty(pHistory, objGuid, "Name", desc.m_sName);
  SetProperty(pHistory, objGuid, "LocalPosition", local.m_vPosition);
  SetProperty(pHistory, objGuid, "LocalRotation", local.m_qRotation);

  {
    plAddObjectCommand cmd;
    cmd.m_NewObjectGuid = compGuid;
    cmd.m_pType = plRTTI::FindTypeByName("plBrushComponent");
    cmd.m_sParentProperty = "Components";
    cmd.m_Parent = objGuid;
    cmd.m_Index = -1;
    pHistory->AddCommand(cmd).LogFailure();
  }

  if (!desc.m_sMaterial.IsEmpty())
  {
    plInsertObjectPropertyCommand cmd;
    cmd.m_Object = compGuid;
    cmd.m_sProperty = "Materials";
    cmd.m_Index = 0;
    cmd.m_NewValue = desc.m_sMaterial;
    pHistory->AddCommand(cmd).LogFailure();
  }

  SetProperty(pHistory, compGuid, "Shape", static_cast<plInt64>(desc.m_Shape));
  SetProperty(pHistory, compGuid, "SizeNegX", desc.m_Params.m_fSizeNegX);
  SetProperty(pHistory, compGuid, "SizePosX", desc.m_Params.m_fSizePosX);
  SetProperty(pHistory, compGuid, "SizeNegY", desc.m_Params.m_fSizeNegY);
  SetProperty(pHistory, compGuid, "SizePosY", desc.m_Params.m_fSizePosY);
  SetProperty(pHistory, compGuid, "SizeNegZ", desc.m_Params.m_fSizeNegZ);
  SetProperty(pHistory, compGuid, "SizePosZ", desc.m_Params.m_fSizePosZ);
  SetProperty(pHistory, compGuid, "Detail", desc.m_Params.m_uiDetail);
  SetProperty(pHistory, compGuid, "Curvature", desc.m_Params.m_Curvature);
  SetProperty(pHistory, compGuid, "Thickness", desc.m_Params.m_fThickness);

  if (desc.m_bSubtractive)
    SetProperty(pHistory, compGuid, "Subtractive", true);

  transaction.Commit();
  return compGuid;
}

plStatus plBrushCommands::CreateGroup(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> objects, plUuid* out_pGroupObject)
{
  plGameObjectDocument* pGameDoc = plDynamicCast<plGameObjectDocument*>(pDocument);

  if (pGameDoc == nullptr || objects.IsEmpty())
    return plStatus("Select the objects to group.");

  // game objects only: a selected component groups its owner
  plHybridArray<const plDocumentObject*, 16> gameObjects;
  for (const plDocumentObject* pObject : objects)
  {
    const plDocumentObject* pGameObject = plBrushPicking::IsBrushComponent(pObject) ? pObject->GetParent() : pObject;
    if (!gameObjects.Contains(pGameObject))
      gameObjects.PushBack(pGameObject);
  }

  const plDocumentObject* pRoot = pDocument->GetObjectManager()->GetRootObject();
  const plDocumentObject* pFirstParent = gameObjects[0]->GetParent();

  ScopedTransaction transaction(pDocument, "Group Brushes");
  plCommandHistory* pHistory = transaction.m_pHistory;

  const plUuid groupGuid = plUuid::MakeUuid();

  {
    plAddObjectCommand cmd;
    cmd.m_NewObjectGuid = groupGuid;
    cmd.m_pType = plGetStaticRTTI<plGameObject>();
    cmd.m_sParentProperty = "Children";
    cmd.m_Index = -1;
    if (pFirstParent != pRoot)
      cmd.m_Parent = pFirstParent->GetGuid();
    pHistory->AddCommand(cmd).LogFailure();
  }

  SetProperty(pHistory, groupGuid, "Name", plString("Brush Group"));

  {
    plAddObjectCommand cmd;
    cmd.m_NewObjectGuid = plUuid::MakeUuid();
    cmd.m_pType = plRTTI::FindTypeByName("plBrushGroupComponent");
    cmd.m_sParentProperty = "Components";
    cmd.m_Parent = groupGuid;
    cmd.m_Index = -1;
    pHistory->AddCommand(cmd).LogFailure();
  }

  // the group sits at identity under the first object's parent, so that parent's children keep
  // their placement as they move in; anything from elsewhere is re-placed explicitly
  for (const plDocumentObject* pObject : gameObjects)
  {
    const plTransform global = pGameDoc->GetGlobalTransform(pObject);
    const bool bSameParent = pObject->GetParent() == pFirstParent;

    plMoveObjectCommand cmd;
    cmd.m_Object = pObject->GetGuid();
    cmd.m_NewParent = groupGuid;
    cmd.m_sParentProperty = "Children";
    cmd.m_Index = -1;
    pHistory->AddCommand(cmd).LogFailure();

    if (!bSameParent)
    {
      pGameDoc->SetGlobalTransform(pObject, global, TransformationChanges::All);
    }
  }

  if (out_pGroupObject)
    *out_pGroupObject = groupGuid;

  transaction.Commit();
  return plStatus(PL_SUCCESS);
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_BrushCommands);
