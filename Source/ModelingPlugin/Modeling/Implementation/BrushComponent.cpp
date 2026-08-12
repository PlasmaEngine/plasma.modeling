#include <ModelingPlugin/ModelingPluginPCH.h>

#include <Core/Graphics/Geometry.h>
#include <Core/Interfaces/PhysicsWorldModule.h>
#include <Core/Messages/SetColorMessage.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <Foundation/IO/MemoryStream.h>
#include <ModelingPlugin/Modeling/BrushComponent.h>
#include <RendererCore/Material/MaterialResource.h>
#include <RendererCore/Meshes/CpuMeshResource.h>
#include <RendererCore/Meshes/MeshComponentBase.h>
#include <RendererCore/Meshes/MeshResource.h>
#include <RendererCore/Pipeline/RenderDataManager.h>
#include <RendererCore/Utils/WorldGeoExtractionUtil.h>

// Data/Base/Materials/Common/Pattern.plMaterialAsset — the default blockout material, same as greybox.
static const char* s_szDefaultBrushMaterial = "{ 1c47ee4c-0379-4280-85f5-b8cda61941d2 }";

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plBrushComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_ACCESSOR_PROPERTY("Shape", plBrushShape, GetShape, SetShape),
    PL_ARRAY_ACCESSOR_PROPERTY("Materials", Materials_GetCount, Materials_GetValue, Materials_SetValue, Materials_Insert, Materials_Remove)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Material")),
    PL_ACCESSOR_PROPERTY("Color", GetColor, SetColor)->AddAttributes(new plExposeColorAlphaAttribute()),
    PL_ACCESSOR_PROPERTY("CustomData", GetCustomData, SetCustomData)->AddAttributes(new plDefaultValueAttribute(plVec4(0, 1, 0, 1))),
    PL_ACCESSOR_PROPERTY("SizeNegX", GetSizeNegX, SetSizeNegX)->AddAttributes(new plDefaultValueAttribute(1.0f), new plGroupAttribute("Size", "Size")),
    PL_ACCESSOR_PROPERTY("SizePosX", GetSizePosX, SetSizePosX)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_ACCESSOR_PROPERTY("SizeNegY", GetSizeNegY, SetSizeNegY)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_ACCESSOR_PROPERTY("SizePosY", GetSizePosY, SetSizePosY)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_ACCESSOR_PROPERTY("SizeNegZ", GetSizeNegZ, SetSizeNegZ)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_ACCESSOR_PROPERTY("SizePosZ", GetSizePosZ, SetSizePosZ)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_ACCESSOR_PROPERTY("Detail", GetDetail, SetDetail)->AddAttributes(new plGroupAttribute("Misc"), new plDefaultValueAttribute(16), new plClampValueAttribute(1, 64)),
    PL_ACCESSOR_PROPERTY("Curvature", GetCurvature, SetCurvature)->AddAttributes(new plClampValueAttribute(plAngle::MakeFromDegree(-360), plAngle::MakeFromDegree(360))),
    PL_ACCESSOR_PROPERTY("Thickness", GetThickness, SetThickness)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, plVariant())),
    PL_ACCESSOR_PROPERTY("GenerateCollision", GetGenerateCollision, SetGenerateCollision)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_ACCESSOR_PROPERTY("IncludeInNavmesh", GetIncludeInNavmesh, SetIncludeInNavmesh)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_MEMBER_PROPERTY("UseAsOccluder", m_bUseAsOccluder)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_ACCESSOR_PROPERTY("FaceData", GetFaceData, SetFaceData)->AddAttributes(new plHiddenAttribute()),
    PL_ACCESSOR_PROPERTY("StoredMesh", GetStoredMesh, SetStoredMesh)->AddAttributes(new plHiddenAttribute()),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Construction"),
    new plNonUniformBoxManipulatorAttribute("SizeNegX", "SizePosX", "SizeNegY", "SizePosY", "SizeNegZ", "SizePosZ"),
  }
  PL_END_ATTRIBUTES;
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgExtractRenderData, OnMsgExtractRenderData),
    PL_MESSAGE_HANDLER(plMsgBuildStaticMesh, OnBuildStaticMesh),
    PL_MESSAGE_HANDLER(plMsgExtractGeometry, OnMsgExtractGeometry),
    PL_MESSAGE_HANDLER(plMsgExtractOccluderData, OnMsgExtractOccluderData),
    PL_MESSAGE_HANDLER(plMsgSetMeshMaterial, OnMsgSetMeshMaterial),
    PL_MESSAGE_HANDLER(plMsgSetColor, OnMsgSetColor),
    PL_MESSAGE_HANDLER(plMsgSetCustomData, OnMsgSetCustomData),
  }
  PL_END_MESSAGEHANDLERS;
}
PL_END_COMPONENT_TYPE;
// clang-format on

plBrushComponent::plBrushComponent() = default;
plBrushComponent::~plBrushComponent() = default;

void plBrushComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  plStreamWriter& s = inout_stream.GetStream();

  s << m_Shape;
  s << m_Params.m_fSizeNegX;
  s << m_Params.m_fSizePosX;
  s << m_Params.m_fSizeNegY;
  s << m_Params.m_fSizePosY;
  s << m_Params.m_fSizeNegZ;
  s << m_Params.m_fSizePosZ;
  s << m_Params.m_uiDetail;
  s << m_Params.m_Curvature;
  s << m_Params.m_fThickness;
  s << m_Color;
  s << m_vCustomData;
  s << m_bGenerateCollision;
  s << m_bIncludeInNavmesh;
  s << m_bUseAsOccluder;

  s << m_Materials.GetCount();
  for (const auto& mat : m_Materials)
  {
    s << mat;
  }

  s.WriteArray(m_FaceData).AssertSuccess();
  s.WriteArray(m_StoredMesh).AssertSuccess();
}

void plBrushComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  plStreamReader& s = inout_stream.GetStream();

  s >> m_Shape;
  s >> m_Params.m_fSizeNegX;
  s >> m_Params.m_fSizePosX;
  s >> m_Params.m_fSizeNegY;
  s >> m_Params.m_fSizePosY;
  s >> m_Params.m_fSizeNegZ;
  s >> m_Params.m_fSizePosZ;
  s >> m_Params.m_uiDetail;
  s >> m_Params.m_Curvature;
  s >> m_Params.m_fThickness;
  s >> m_Color;
  s >> m_vCustomData;
  s >> m_bGenerateCollision;
  s >> m_bIncludeInNavmesh;
  s >> m_bUseAsOccluder;

  plUInt32 uiMaterials = 0;
  s >> uiMaterials;
  m_Materials.SetCount(static_cast<plUInt16>(uiMaterials));
  for (auto& mat : m_Materials)
  {
    s >> mat;
  }

  s.ReadArray(m_FaceData).AssertSuccess();
  s.ReadArray(m_StoredMesh).AssertSuccess();
}

void plBrushComponent::OnActivated()
{
  if (!m_hMesh.IsValid())
  {
    m_hMesh = GenerateMesh<plMeshResource>();
  }

  SUPER::OnActivated();
}

void plBrushComponent::OnDeactivated()
{
  plRenderDataManager* pRenderDataManager = GetWorld()->GetModule<plRenderDataManager>();
  pRenderDataManager->DeleteInstanceData(m_InstanceDataOffset);

  SUPER::OnDeactivated();
}

plResult plBrushComponent::GetLocalBounds(plBoundingBoxSphere& bounds, bool& bAlwaysVisible, plMsgUpdateLocalBounds& msg)
{
  if (m_hMesh.IsValid())
  {
    plResourceLock<plMeshResource> pMesh(m_hMesh, plResourceAcquireMode::AllowLoadingFallback);
    bounds = pMesh->GetBounds();

    if (m_bUseAsOccluder)
    {
      msg.AddBounds(bounds, GetOwner()->IsStatic() ? plDefaultSpatialDataCategories::OcclusionStatic : plDefaultSpatialDataCategories::OcclusionDynamic);
    }

    return PL_SUCCESS;
  }

  return PL_FAILURE;
}

void plBrushComponent::OnMsgExtractRenderData(plMsgExtractRenderData& msg) const
{
  if (!m_hMesh.IsValid())
    return;

  const bool bDynamic = GetOwner()->IsDynamic();
  auto hInstanceDataBuffer = msg.m_pRenderDataManager->GetOrCreateInstanceDataAndFill(*this, bDynamic, GetOwner()->GetGlobalTransform(), m_InstanceDataOffset, GetUniqueIdForRendering(), m_Color, m_vCustomData);

  plResourceLock<plMeshResource> pMesh(m_hMesh, plResourceAcquireMode::AllowLoadingFallback);
  plArrayPtr<const plMeshResourceDescriptor::SubMesh> parts = pMesh->GetSubMeshes();

  for (plUInt32 uiPartIndex = 0; uiPartIndex < parts.GetCount(); ++uiPartIndex)
  {
    const plUInt32 uiMaterialIndex = parts[uiPartIndex].m_uiMaterialIndex;

    plMaterialResourceHandle hMaterial = GetMaterial(uiMaterialIndex);
    if (!hMaterial.IsValid())
    {
      hMaterial = pMesh->GetMaterials()[uiMaterialIndex];
    }

    plMeshRenderData* pRenderData = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plMeshRenderData>(GetOwner());
    pRenderData->SetFallbackGlobalBounds(GetOwner()->GetGlobalBounds().GetBox());
    pRenderData->Fill(m_InstanceDataOffset, hInstanceDataBuffer, hMaterial, m_hMesh, uiMaterialIndex, uiPartIndex);

    bool bDontCacheYet = false;
    plRenderData::Category category = plMaterialResource::GetRenderDataCategory(hMaterial, &bDontCacheYet);

    msg.AddRenderData(pRenderData, category, bDontCacheYet ? plRenderData::Caching::Never : plRenderData::Caching::IfStatic);
  }
}

void plBrushComponent::SetShape(plEnum<plBrushShape> shape)
{
  m_Shape = shape;
  InvalidateMesh();
}

void plBrushComponent::SetColor(const plColor& color)
{
  m_Color = color;
  InvalidateCachedRenderData();
}

const plColor& plBrushComponent::GetColor() const
{
  return m_Color;
}

void plBrushComponent::SetCustomData(const plVec4& vData)
{
  m_vCustomData = vData;
  InvalidateCachedRenderData();
}

const plVec4& plBrushComponent::GetCustomData() const
{
  return m_vCustomData;
}

void plBrushComponent::SetSizeNegX(float f) { m_Params.m_fSizeNegX = f; InvalidateMesh(); }
void plBrushComponent::SetSizePosX(float f) { m_Params.m_fSizePosX = f; InvalidateMesh(); }
void plBrushComponent::SetSizeNegY(float f) { m_Params.m_fSizeNegY = f; InvalidateMesh(); }
void plBrushComponent::SetSizePosY(float f) { m_Params.m_fSizePosY = f; InvalidateMesh(); }
void plBrushComponent::SetSizeNegZ(float f) { m_Params.m_fSizeNegZ = f; InvalidateMesh(); }
void plBrushComponent::SetSizePosZ(float f) { m_Params.m_fSizePosZ = f; InvalidateMesh(); }

void plBrushComponent::SetDetail(plUInt32 uiDetail)
{
  m_Params.m_uiDetail = uiDetail;
  InvalidateMesh();
}

void plBrushComponent::SetCurvature(plAngle curvature)
{
  m_Params.m_Curvature = curvature;
  InvalidateMesh();
}

void plBrushComponent::SetThickness(float f)
{
  m_Params.m_fThickness = f;
  InvalidateMesh();
}

void plBrushComponent::SetGenerateCollision(bool b)
{
  m_bGenerateCollision = b;
}

void plBrushComponent::SetIncludeInNavmesh(bool b)
{
  m_bIncludeInNavmesh = b;
}

void plBrushComponent::SetFaceData(const plDataBuffer& data)
{
  m_FaceData = data;
  InvalidateMesh();
}

void plBrushComponent::SetStoredMesh(const plDataBuffer& data)
{
  m_StoredMesh = data;
  InvalidateMesh();
}

void plBrushComponent::SetMaterial(plUInt32 uiIndex, const plMaterialResourceHandle& hMaterial)
{
  if (uiIndex >= 64)
  {
    plLog::Error("Invalid material slot index on brush component.");
    return;
  }

  m_Materials.EnsureCount(static_cast<plUInt16>(uiIndex + 1));

  if (m_Materials[uiIndex] != hMaterial)
  {
    m_Materials[uiIndex] = hMaterial;

    // materials are baked into the mesh resource (per-slot submeshes + surfaces), so the mesh
    // identity changes with them
    InvalidateMesh();
  }
}

plMaterialResourceHandle plBrushComponent::GetMaterial(plUInt32 uiIndex) const
{
  if (uiIndex >= m_Materials.GetCount())
    return plMaterialResourceHandle();

  return m_Materials[uiIndex];
}

plUInt32 plBrushComponent::Materials_GetCount() const
{
  return m_Materials.GetCount();
}

plString plBrushComponent::Materials_GetValue(plUInt32 uiIndex) const
{
  return GetMaterial(uiIndex).GetResourceID();
}

void plBrushComponent::Materials_SetValue(plUInt32 uiIndex, plString sValue)
{
  if (sValue.IsEmpty())
  {
    SetMaterial(uiIndex, plMaterialResourceHandle());
  }
  else
  {
    SetMaterial(uiIndex, plResourceManager::LoadResource<plMaterialResource>(sValue));
  }
}

void plBrushComponent::Materials_Insert(plUInt32 uiIndex, plString sValue)
{
  plMaterialResourceHandle hMat;

  if (!sValue.IsEmpty())
  {
    hMat = plResourceManager::LoadResource<plMaterialResource>(sValue);
  }

  m_Materials.InsertAt(static_cast<plUInt16>(uiIndex), hMat);
  InvalidateMesh();
}

void plBrushComponent::Materials_Remove(plUInt32 uiIndex)
{
  m_Materials.RemoveAtAndCopy(static_cast<plUInt16>(uiIndex));
  InvalidateMesh();
}

constexpr plUInt8 c_uiFaceDataVersion = 1;

void plBrushComponent::ParseFaceData(const plDataBuffer& data, plHashTable<plUInt32, plUInt8>& out_overrides)
{
  out_overrides.Clear();

  if (data.IsEmpty())
    return;

  plRawMemoryStreamReader reader(data.GetData(), data.GetCount());

  plUInt8 uiVersion = 0;
  reader >> uiVersion;

  if (uiVersion != c_uiFaceDataVersion)
    return;

  plUInt32 uiCount = 0;
  reader >> uiCount;

  out_overrides.Reserve(uiCount);

  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    plUInt32 uiFaceId = 0;
    plUInt8 uiSlot = 0;
    reader >> uiFaceId;
    reader >> uiSlot;
    out_overrides[uiFaceId] = uiSlot;
  }
}

void plBrushComponent::WriteFaceData(const plHashTable<plUInt32, plUInt8>& overrides, plDataBuffer& out_data)
{
  out_data.Clear();

  if (overrides.IsEmpty())
    return;

  plDefaultMemoryStreamStorage storage;
  plMemoryStreamWriter writer(&storage);

  writer << c_uiFaceDataVersion;
  writer << overrides.GetCount();

  for (auto it : overrides)
  {
    writer << it.Key();
    writer << it.Value();
  }

  out_data.SetCountUninitialized(storage.GetStorageSize32());

  plRawMemoryStreamWriter rawWriter(out_data.GetData(), out_data.GetCount());
  storage.CopyToStream(rawWriter).IgnoreResult();
}

void plBrushComponent::BuildEditableMesh(plEditableMesh& out_mesh) const
{
  if (m_Shape == plBrushShape::Editable)
  {
    if (out_mesh.FromDataBuffer(m_StoredMesh).Failed())
    {
      out_mesh.Clear();
    }
  }
  else
  {
    plBrushGenerators::Generate(m_Shape, m_Params, out_mesh);
  }

  // apply the per-face material overrides, keyed by semantic face id
  plHashTable<plUInt32, plUInt8> overrides;
  ParseFaceData(m_FaceData, overrides);

  if (!overrides.IsEmpty())
  {
    for (plBrushFace& face : out_mesh.m_Faces)
    {
      plUInt8 uiSlot = 0;
      if (overrides.TryGetValue(face.m_uiFaceId, uiSlot))
      {
        face.m_uiMaterialSlot = uiSlot;
      }
    }
  }
}

void plBrushComponent::GenerateMeshName(plStringBuilder& out_sName) const
{
  // The mesh identity is a content hash: identical brushes (duplicates) share one GPU mesh, and any
  // change to shape, parameters, face overrides or the material list produces a new resource.
  plEditableMesh mesh;
  BuildEditableMesh(mesh);

  plDefaultMemoryStreamStorage storage;
  plMemoryStreamWriter writer(&storage);
  mesh.Serialize(writer);

  for (const auto& mat : m_Materials)
  {
    writer << mat.GetResourceID();
  }

  plHybridArray<plUInt8, 1024 * 4> bytes;
  bytes.SetCountUninitialized(storage.GetStorageSize32());

  plRawMemoryStreamWriter rawWriter(bytes.GetData(), bytes.GetCount());
  storage.CopyToStream(rawWriter).IgnoreResult();

  const plUInt64 uiHash = plHashingUtils::xxHash64(bytes.GetData(), bytes.GetCount());
  out_sName.SetFormat("BrushMesh_{0}", plArgU(uiHash, 16, true, 16));
}

const char* plBrushComponent::GetMaterialIdOrDefault(plUInt32 uiSlot, plStringBuilder& ref_sTemp) const
{
  if (uiSlot < m_Materials.GetCount() && m_Materials[uiSlot].IsValid())
  {
    ref_sTemp = m_Materials[uiSlot].GetResourceID();
    return ref_sTemp.GetData();
  }

  return s_szDefaultBrushMaterial;
}

plMaterialResourceHandle plBrushComponent::ResolveMaterial(plUInt32 uiSlot) const
{
  plMaterialResourceHandle hMat = GetMaterial(uiSlot);

  if (!hMat.IsValid())
  {
    hMat = plResourceManager::LoadResource<plMaterialResource>(s_szDefaultBrushMaterial);
  }

  return hMat;
}

void plBrushComponent::GenerateMeshResourceDescriptor(plMeshResourceDescriptor& desc, const plEditableMesh& mesh) const
{
  plGeometry geom;
  plDynamicArray<plBrushSubMeshRange> ranges;
  mesh.BuildRenderGeometry(geom, ranges, m_Color);

  geom.ComputeTangents();

  plStringBuilder sTemp;
  const plUInt32 uiNumSlots = mesh.GetUsedMaterialSlotCount();

  for (plUInt32 uiSlot = 0; uiSlot < uiNumSlots; ++uiSlot)
  {
    desc.SetMaterial(uiSlot, GetMaterialIdOrDefault(uiSlot, sTemp));
  }

  desc.MeshBufferDesc().AddCommonStreams();
  desc.MeshBufferDesc().AllocateStreamsFromGeometry(geom, plGALPrimitiveTopology::Triangles);

  for (const plBrushSubMeshRange& range : ranges)
  {
    desc.AddSubMesh(range.m_uiNumTriangles, range.m_uiFirstTriangle, range.m_uiMaterialSlot);
  }

  desc.ComputeBounds();
}

template <typename ResourceType>
plTypedResourceHandle<ResourceType> plBrushComponent::GenerateMesh() const
{
  plStringBuilder sResourceName;
  GenerateMeshName(sResourceName);

  plTypedResourceHandle<ResourceType> hResource = plResourceManager::GetExistingResource<ResourceType>(sResourceName);
  if (hResource.IsValid())
    return hResource;

  plEditableMesh mesh;
  BuildEditableMesh(mesh);

  plMeshResourceDescriptor desc;
  GenerateMeshResourceDescriptor(desc, mesh);

  return plResourceManager::GetOrCreateResource<ResourceType>(sResourceName, std::move(desc), sResourceName);
}

void plBrushComponent::OnBuildStaticMesh(plMsgBuildStaticMesh& msg) const
{
  if (!m_bGenerateCollision)
    return;

  plEditableMesh mesh;
  BuildEditableMesh(mesh);

  plGeometry geom;
  plDynamicArray<plBrushSubMeshRange> ranges;
  mesh.BuildRenderGeometry(geom, ranges, m_Color);

  auto* pDesc = msg.m_pStaticMeshDescription;
  const plTransform t = GetOwner()->GetGlobalTransform();

  const plUInt32 uiVertexOffset = pDesc->m_Vertices.GetCount();

  for (const auto& vert : geom.GetVertices())
  {
    pDesc->m_Vertices.PushBack(t * vert.m_vPosition);
  }

  // one collision submesh per material slot, so each face's material decides its physics surface
  for (const plBrushSubMeshRange& range : ranges)
  {
    auto& subMesh = pDesc->m_SubMeshes.ExpandAndGetRef();
    subMesh.m_uiFirstTriangle = pDesc->m_Triangles.GetCount();

    for (plUInt32 uiTri = range.m_uiFirstTriangle; uiTri < range.m_uiFirstTriangle + range.m_uiNumTriangles; ++uiTri)
    {
      const auto& poly = geom.GetPolygons()[uiTri];

      auto& tri = pDesc->m_Triangles.ExpandAndGetRef();
      tri.m_uiVertexIndices[0] = uiVertexOffset + poly.m_Vertices[0];
      tri.m_uiVertexIndices[1] = uiVertexOffset + poly.m_Vertices[1];
      tri.m_uiVertexIndices[2] = uiVertexOffset + poly.m_Vertices[2];
    }

    subMesh.m_uiNumTriangles = range.m_uiNumTriangles;

    plMaterialResourceHandle hMaterial = ResolveMaterial(range.m_uiMaterialSlot);

    if (hMaterial.IsValid())
    {
      plResourceLock<plMaterialResource> pMaterial(hMaterial, plResourceAcquireMode::BlockTillLoaded_NeverFail);

      if (pMaterial.GetAcquireResult() == plResourceAcquireResult::Final)
      {
        const plString surface = pMaterial->GetSurface().GetString();

        if (!surface.IsEmpty())
        {
          plUInt32 idx = pDesc->m_Surfaces.IndexOf(surface);
          if (idx == plInvalidIndex)
          {
            idx = pDesc->m_Surfaces.GetCount();
            pDesc->m_Surfaces.PushBack(surface);
          }

          subMesh.m_uiSurfaceIndex = static_cast<plUInt16>(idx);
        }
      }
    }
  }
}

void plBrushComponent::OnMsgExtractGeometry(plMsgExtractGeometry& msg) const
{
  if (msg.m_Mode == plWorldGeoExtractionUtil::ExtractionMode::CollisionMesh && (m_bGenerateCollision == false || GetOwner()->IsDynamic()))
    return;

  if (msg.m_Mode == plWorldGeoExtractionUtil::ExtractionMode::NavMeshGeneration && (m_bIncludeInNavmesh == false || GetOwner()->IsDynamic()))
    return;

  msg.AddMeshObject(GetOwner()->GetGlobalTransform(), GenerateMesh<plCpuMeshResource>());
}

void plBrushComponent::OnMsgExtractOccluderData(plMsgExtractOccluderData& msg) const
{
  if (!IsActiveAndInitialized() || !m_bUseAsOccluder)
    return;

  if (m_pOccluderObject == nullptr)
  {
    plStringBuilder sResourceName;
    GenerateMeshName(sResourceName);
    sResourceName.Append("_Occluder");

    m_pOccluderObject = plRasterizerObject::GetObject(sResourceName);

    if (m_pOccluderObject == nullptr)
    {
      plEditableMesh mesh;
      BuildEditableMesh(mesh);

      plGeometry geom;
      plDynamicArray<plBrushSubMeshRange> ranges;
      mesh.BuildRenderGeometry(geom, ranges, m_Color);

      m_pOccluderObject = plRasterizerObject::CreateMesh(sResourceName, geom);
    }
  }

  msg.AddOccluder(m_pOccluderObject.Borrow(), GetOwner()->GetGlobalTransform());
}

void plBrushComponent::OnMsgSetMeshMaterial(plMsgSetMeshMaterial& ref_msg)
{
  SetMaterial(ref_msg.m_uiMaterialSlot, ref_msg.m_hMaterial);
}

void plBrushComponent::OnMsgSetColor(plMsgSetColor& ref_msg)
{
  ref_msg.ModifyColor(m_Color);
  InvalidateCachedRenderData();
}

void plBrushComponent::OnMsgSetCustomData(plMsgSetCustomData& ref_msg)
{
  m_vCustomData.Set(ref_msg.m_fData0, ref_msg.m_fData1, ref_msg.m_fData2, ref_msg.m_fData3);
  InvalidateCachedRenderData();
}

void plBrushComponent::InvalidateMesh()
{
  m_pOccluderObject = nullptr;

  if (m_hMesh.IsValid())
  {
    m_hMesh.Invalidate();

    m_hMesh = GenerateMesh<plMeshResource>();

    TriggerLocalBoundsUpdate();
  }
}


PL_STATICLINK_FILE(ModelingPlugin, ModelingPlugin_Modeling_Implementation_BrushComponent);
