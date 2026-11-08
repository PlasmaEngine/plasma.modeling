#include <EnginePluginModeling/EnginePluginModelingPCH.h>

#include <EnginePluginModeling/SceneExport/BakeBrushesExportModifier.h>
#include <ModelingPlugin/Modeling/BrushComponent.h>
#include <ModelingPlugin/Modeling/BrushGroupComponent.h>
#include <RendererCore/Material/MaterialResource.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plSceneExportModifier_BakeBrushes, 1, plRTTIDefaultAllocator<plSceneExportModifier_BakeBrushes>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  // Data/Base/Materials/Common/Pattern.plMaterialAsset — what an empty material slot renders with
  const char* s_szDefaultBrushMaterial = "{ 1c47ee4c-0379-4280-85f5-b8cda61941d2 }";

  /// Brushes merge only when everything the merged brush would apply to all of them is identical.
  struct MergeKey
  {
    const plBrushGroupComponent* m_pGroup = nullptr;
    bool m_bCollision = false;
    bool m_bNavmesh = false;
    bool m_bOccluder = false;
    float m_fSmoothingDeg = 0.0f;
    plColor m_Color;
    plVec4 m_vCustomData;

    bool operator==(const MergeKey& other) const
    {
      return m_pGroup == other.m_pGroup && m_bCollision == other.m_bCollision && m_bNavmesh == other.m_bNavmesh && m_bOccluder == other.m_bOccluder &&
             m_fSmoothingDeg == other.m_fSmoothingDeg && m_Color == other.m_Color && m_vCustomData == other.m_vCustomData;
    }
  };

  struct MergeBucket
  {
    MergeKey m_Key;
    plHybridArray<plBrushComponent*, 16> m_Brushes;
  };

  plString GetMaterialId(const plBrushComponent* pBrush, plUInt32 uiSlot)
  {
    const plMaterialResourceHandle hMaterial = pBrush->GetMaterial(uiSlot);
    return hMaterial.IsValid() ? plString(hMaterial.GetResourceID()) : plString(s_szDefaultBrushMaterial);
  }
} // namespace

void plSceneExportModifier_BakeBrushes::ModifyWorld(plWorld& ref_world, plStringView sDocumentType, const plUuid& documentGuid, bool bForExport)
{
  PL_IGNORE_UNUSED(sDocumentType);
  PL_IGNORE_UNUSED(documentGuid);

  // play-in-editor keeps the live, editable brushes
  if (!bForExport)
    return;

  PL_LOCK(ref_world.GetWriteMarker());

  plBrushComponentManager* pManager = ref_world.GetComponentManager<plBrushComponentManager>();

  if (pManager == nullptr)
    return;

  plDynamicArray<plBrushComponent*> additive;
  plDynamicArray<plComponentHandle> cutters;

  for (auto it = pManager->GetComponents(); it.IsValid(); it.Next())
  {
    if (it->GetSubtractive())
      cutters.PushBack(it->GetHandle());
    else
      additive.PushBack(it);
  }

  // 1. evaluate everything first: every result must see all cutters still in place
  plDynamicArray<plDataBuffer> baked;
  baked.SetCount(additive.GetCount());

  plEditableMesh mesh;

  for (plUInt32 i = 0; i < additive.GetCount(); ++i)
  {
    additive[i]->BuildEvaluatedMesh(mesh);
    mesh.ToDataBuffer(baked[i]);
  }

  // 2. the evaluated meshes become the brushes' authored meshes; attributes are already applied
  for (plUInt32 i = 0; i < additive.GetCount(); ++i)
  {
    plBrushComponent* pBrush = additive[i];

    pBrush->SetShape(plBrushShape::Editable);
    pBrush->SetStoredMesh(baked[i]);
    pBrush->SetFaceData(plDataBuffer());
  }

  // 3. cutters have done their job
  for (const plComponentHandle& hCutter : cutters)
  {
    pManager->DeleteComponent(hCutter);
  }

  // 4. static brushes in a merging group become one brush per shared settings
  plDynamicArray<MergeBucket> buckets;

  for (plBrushComponent* pBrush : additive)
  {
    const plGameObject* pOwner = pBrush->GetOwner();

    if (!pBrush->GetMergeOnExport() || !pOwner->IsStatic() || pBrush->HasDamagedMesh())
      continue;

    const plBrushGroupComponent* pGroup = plBrushGroupComponent::FindGroup(pOwner);

    if (pGroup == nullptr || !pGroup->GetMergeOnExport())
      continue;

    MergeKey key;
    key.m_pGroup = pGroup;
    key.m_bCollision = pBrush->GetGenerateCollision();
    key.m_bNavmesh = pBrush->GetIncludeInNavmesh();
    key.m_bOccluder = pBrush->GetUseAsOccluder();
    key.m_fSmoothingDeg = pBrush->GetSmoothingAngle().GetDegree();
    key.m_Color = pBrush->GetColor();
    key.m_vCustomData = pBrush->GetCustomData();

    MergeBucket* pBucket = nullptr;
    for (MergeBucket& bucket : buckets)
    {
      if (bucket.m_Key == key)
      {
        pBucket = &bucket;
        break;
      }
    }

    if (pBucket == nullptr)
    {
      pBucket = &buckets.ExpandAndGetRef();
      pBucket->m_Key = key;
    }

    pBucket->m_Brushes.PushBack(pBrush);
  }

  plUInt32 uiMergedBrushes = 0;
  plUInt32 uiMergedMeshes = 0;

  for (const MergeBucket& bucket : buckets)
  {
    if (bucket.m_Brushes.GetCount() < 2)
      continue;

    const plGameObject* pGroupObject = bucket.m_Key.m_pGroup->GetOwner();
    const plTransform groupTransform = pGroupObject->GetGlobalTransform();

    plEditableMesh merged;
    plDynamicArray<plString> materials;
    plEditableMesh part;

    for (plBrushComponent* pBrush : bucket.m_Brushes)
    {
      pBrush->BuildEditableMesh(part);

      if (part.m_Faces.IsEmpty())
        continue;

      const plTransform brushTransform = pBrush->GetOwner()->GetGlobalTransform();

      // UVs are frozen in the brush's own projection space before the vertices move to the group
      part.ConvertToExplicitUVs(pBrush->GetTextureLock() ? nullptr : &brushTransform);
      part.TransformVertices(plTransform::MakeLocalTransform(groupTransform, brushTransform));

      // slots remap into one shared material list
      for (plBrushFace& face : part.m_Faces)
      {
        const plString sMaterial = GetMaterialId(pBrush, face.m_uiMaterialSlot);

        plUInt32 uiSlot = materials.IndexOf(sMaterial);
        if (uiSlot == plInvalidIndex)
        {
          uiSlot = materials.GetCount();
          materials.PushBack(sMaterial);
        }

        face.m_uiMaterialSlot = static_cast<plUInt8>(plMath::Min<plUInt32>(uiSlot, 255));

        // ids only need to be unique inside the merged brush
        face.m_uiFaceId = merged.m_uiNextFaceId++;
      }

      part.m_uiNextFaceId = merged.m_uiNextFaceId;
      merged.AppendMesh(part);
    }

    if (merged.m_Faces.IsEmpty())
      continue;

    // the merged brush lives on a new static child of the group
    plGameObjectDesc desc;
    desc.m_bDynamic = false;
    desc.m_hParent = pGroupObject->GetHandle();
    desc.m_sName.Assign("MergedBrushes");

    plGameObject* pMergedObject = nullptr;
    ref_world.CreateObject(desc, pMergedObject);

    plBrushComponent* pMergedBrush = nullptr;
    pManager->CreateComponent(pMergedObject, pMergedBrush);

    plDataBuffer blob;
    merged.ToDataBuffer(blob);

    pMergedBrush->SetShape(plBrushShape::Editable);
    pMergedBrush->SetStoredMesh(blob);
    pMergedBrush->SetGenerateCollision(bucket.m_Key.m_bCollision);
    pMergedBrush->SetIncludeInNavmesh(bucket.m_Key.m_bNavmesh);
    pMergedBrush->SetUseAsOccluder(bucket.m_Key.m_bOccluder);
    pMergedBrush->SetSmoothingAngle(plAngle::MakeFromDegree(bucket.m_Key.m_fSmoothingDeg));
    pMergedBrush->SetColor(bucket.m_Key.m_Color);
    pMergedBrush->SetCustomData(bucket.m_Key.m_vCustomData);
    pMergedBrush->SetMergeOnExport(false);

    for (plUInt32 uiSlot = 0; uiSlot < materials.GetCount(); ++uiSlot)
    {
      pMergedBrush->SetMaterial(uiSlot, plResourceManager::LoadResource<plMaterialResource>(materials[uiSlot]));
    }

    for (plBrushComponent* pBrush : bucket.m_Brushes)
    {
      pManager->DeleteComponent(pBrush->GetHandle());
    }

    uiMergedBrushes += bucket.m_Brushes.GetCount();
    ++uiMergedMeshes;
  }

  plLog::Info("Brushes baked for export: {} brushes, {} cutters removed, {} brushes merged into {} group meshes.", additive.GetCount(), cutters.GetCount(), uiMergedBrushes,
    uiMergedMeshes);
}


PL_STATICLINK_FILE(EnginePluginModeling, EnginePluginModeling_SceneExport_BakeBrushesExportModifier);
