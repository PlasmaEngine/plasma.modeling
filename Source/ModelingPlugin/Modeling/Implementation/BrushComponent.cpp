#include <ModelingPlugin/ModelingPluginPCH.h>

#include <Core/Graphics/Geometry.h>
#include <Core/Interfaces/PhysicsWorldModule.h>
#include <Core/Messages/SetColorMessage.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <Foundation/IO/MemoryStream.h>
#include <ModelingPlugin/Modeling/BrushComponent.h>
#include <ModelingPlugin/Modeling/BrushEvaluator.h>
#include <ModelingPlugin/Modeling/BrushWorldModule.h>
#include <ModelingPlugin/Modeling/MeshCSG.h>
#include <RendererCore/Material/MaterialResource.h>
#include <RendererCore/Meshes/CpuMeshResource.h>
#include <RendererCore/Meshes/MeshComponentBase.h>
#include <RendererCore/Meshes/MeshResource.h>
#include <RendererCore/Pipeline/RenderDataManager.h>
#include <RendererCore/Utils/WorldGeoExtractionUtil.h>

// Data/Base/Materials/Common/Pattern.plMaterialAsset — the default blockout material, same as greybox.
static const char* s_szDefaultBrushMaterial = "{ 1c47ee4c-0379-4280-85f5-b8cda61941d2 }";

// Data/Base/Materials/Common/PhysicsCollidersTransparent.plMaterialAsset — see-through, two-sided,
// fullbright: the cutter look in edit mode, tinted through the instance colour.
static const char* s_szCutterMaterial = "{ 785a756f-c69c-4f44-b31f-1743806fd4f5 }"; // Base/Materials/BaseMaterials/Transparent: tint and opacity from the instance colour

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plBrushComponent, 3, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ENUM_ACCESSOR_PROPERTY("Shape", plBrushShape, GetShape, SetShape),
    PL_ACCESSOR_PROPERTY("Subtractive", GetSubtractive, SetSubtractive),
    PL_ARRAY_ACCESSOR_PROPERTY("Materials", Materials_GetCount, Materials_GetValue, Materials_SetValue, Materials_Insert, Materials_Remove)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Material")),
    PL_ACCESSOR_PROPERTY("Color", GetColor, SetColor)->AddAttributes(new plExposeColorAlphaAttribute()),
    PL_ACCESSOR_PROPERTY("CustomData", GetCustomData, SetCustomData)->AddAttributes(new plDefaultValueAttribute(plVec4(0, 1, 0, 1))),
    PL_ACCESSOR_PROPERTY("SizeNegX", GetSizeNegX, SetSizeNegX)->AddAttributes(new plDefaultValueAttribute(1.0f), new plGroupAttribute("Size", "Size")),
    PL_ACCESSOR_PROPERTY("SizePosX", GetSizePosX, SetSizePosX)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_ACCESSOR_PROPERTY("SizeNegY", GetSizeNegY, SetSizeNegY)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_ACCESSOR_PROPERTY("SizePosY", GetSizePosY, SetSizePosY)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_ACCESSOR_PROPERTY("SizeNegZ", GetSizeNegZ, SetSizeNegZ)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_ACCESSOR_PROPERTY("SizePosZ", GetSizePosZ, SetSizePosZ)->AddAttributes(new plDefaultValueAttribute(1.0f)),
    PL_ACCESSOR_PROPERTY("Detail", GetDetail, SetDetail)->AddAttributes(new plGroupAttribute("Shape"), new plDefaultValueAttribute(16), new plClampValueAttribute(1, 64)),
    PL_ACCESSOR_PROPERTY("Curvature", GetCurvature, SetCurvature)->AddAttributes(new plClampValueAttribute(plAngle::MakeFromDegree(-360), plAngle::MakeFromDegree(360))),
    PL_ACCESSOR_PROPERTY("Thickness", GetThickness, SetThickness)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, plVariant())),
    PL_ACCESSOR_PROPERTY("SmoothingAngle", GetSmoothingAngle, SetSmoothingAngle)->AddAttributes(new plGroupAttribute("Surface"), new plDefaultValueAttribute(plAngle::MakeFromDegree(40.0f)), new plClampValueAttribute(plAngle::MakeFromDegree(0.0f), plAngle::MakeFromDegree(180.0f))),
    PL_ACCESSOR_PROPERTY("TextureLock", GetTextureLock, SetTextureLock)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_ACCESSOR_PROPERTY("GenerateCollision", GetGenerateCollision, SetGenerateCollision)->AddAttributes(new plGroupAttribute("Output"), new plDefaultValueAttribute(true)),
    PL_ACCESSOR_PROPERTY("IncludeInNavmesh", GetIncludeInNavmesh, SetIncludeInNavmesh)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_ACCESSOR_PROPERTY("UseAsOccluder", GetUseAsOccluder, SetUseAsOccluder)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_ACCESSOR_PROPERTY("MergeOnExport", GetMergeOnExport, SetMergeOnExport)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_ACCESSOR_PROPERTY("BrushId", GetBrushId, SetBrushId)->AddAttributes(new plHiddenAttribute()),
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

const char* plBrushComponent::GetDefaultMaterialId()
{
  return s_szDefaultBrushMaterial;
}

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

  // version 2
  s << m_bSubtractive;

  s << m_Materials.GetCount();
  for (const auto& mat : m_Materials)
  {
    s << mat;
  }

  s.WriteArray(m_FaceData).AssertSuccess();
  s.WriteArray(m_StoredMesh).AssertSuccess();

  // version 3
  s << m_uiBrushId;
  s << m_SmoothingAngle;
  s << m_bTextureLock;
  s << m_bMergeOnExport;
}

void plBrushComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  const plUInt32 uiVersion = inout_stream.GetComponentTypeVersion(GetStaticRTTI());
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

  if (uiVersion >= 2)
  {
    s >> m_bSubtractive;
  }

  plUInt32 uiMaterials = 0;
  s >> uiMaterials;
  m_Materials.SetCount(static_cast<plUInt16>(uiMaterials));
  for (auto& mat : m_Materials)
  {
    s >> mat;
  }

  s.ReadArray(m_FaceData).AssertSuccess();
  s.ReadArray(m_StoredMesh).AssertSuccess();

  if (uiVersion >= 3)
  {
    s >> m_uiBrushId;
    s >> m_SmoothingAngle;
    s >> m_bTextureLock;
    s >> m_bMergeOnExport;
  }

  plBrushFaceAttributeCodec::Parse(m_FaceData, m_FaceAttributes);
  ++m_uiAuthoredRevision;
}

void plBrushComponent::OnActivated()
{
  if (!m_hMesh.IsValid())
  {
    m_hMesh = GenerateMesh<plMeshResource>();
  }

  SUPER::OnActivated();

  GetWorld()->GetOrCreateModule<plBrushWorldModule>()->RegisterBrush(this);
}

void plBrushComponent::OnDeactivated()
{
  if (plBrushWorldModule* pModule = GetWorld()->GetModule<plBrushWorldModule>())
  {
    pModule->DeregisterBrush(this);
  }

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

  // cutters are an authoring aid: visible while editing, gone the moment the world simulates
  // (play mode, exported game)
  if (m_bSubtractive && GetWorld()->GetWorldSimulationEnabled())
    return;

  const bool bDynamic = GetOwner()->IsDynamic();

  // cutters: see-through amber, red when they cannot cut (not convex)
  const plColor instanceColor = m_bSubtractive ? (m_bCutterConvex ? plColor(1.0f, 0.62f, 0.15f, 0.35f) : plColor(1.0f, 0.15f, 0.1f, 0.5f)) : m_Color;
  auto hInstanceDataBuffer = msg.m_pRenderDataManager->GetOrCreateInstanceDataAndFill(*this, bDynamic, GetOwner()->GetGlobalTransform(), m_InstanceDataOffset, GetUniqueIdForRendering(), instanceColor, m_vCustomData);

  plResourceLock<plMeshResource> pMesh(m_hMesh, plResourceAcquireMode::AllowLoadingFallback);
  plArrayPtr<const plMeshResourceDescriptor::SubMesh> parts = pMesh->GetSubMeshes();

  static plMaterialResourceHandle s_hCutterMaterial;
  if (m_bSubtractive && !s_hCutterMaterial.IsValid())
  {
    s_hCutterMaterial = plResourceManager::LoadResource<plMaterialResource>(s_szCutterMaterial);
  }

  for (plUInt32 uiPartIndex = 0; uiPartIndex < parts.GetCount(); ++uiPartIndex)
  {
    const plUInt32 uiMaterialIndex = parts[uiPartIndex].m_uiMaterialIndex;

    plMaterialResourceHandle hMaterial = m_bSubtractive ? s_hCutterMaterial : GetMaterial(uiMaterialIndex);
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
  OnAuthoredChange();
}

void plBrushComponent::SetSubtractive(bool b)
{
  if (m_bSubtractive == b)
    return;

  m_bSubtractive = b;

  // the world module's change detection re-carves affected additive brushes next frame
  OnAuthoredChange();
}

void plBrushComponent::SetUseAsOccluder(bool b)
{
  m_bUseAsOccluder = b;
  m_pOccluderObject = nullptr;
  TriggerLocalBoundsUpdate();
}

void plBrushComponent::SetSmoothingAngle(plAngle angle)
{
  m_SmoothingAngle = angle;
  InvalidateMesh();
}

void plBrushComponent::SetTextureLock(bool b)
{
  m_bTextureLock = b;
  InvalidateMesh();
}

void plBrushComponent::SetBrushId(plUInt32 uiId)
{
  if (m_uiBrushId == uiId)
    return;

  m_uiBrushId = uiId;

  // a cutter's id keys the faces it cuts and its evaluation order
  OnAuthoredChange();
}

void plBrushComponent::OnAuthoredChange()
{
  ++m_uiAuthoredRevision;
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

void plBrushComponent::SetSizeNegX(float f) { m_Params.m_fSizeNegX = f; OnAuthoredChange(); }
void plBrushComponent::SetSizePosX(float f) { m_Params.m_fSizePosX = f; OnAuthoredChange(); }
void plBrushComponent::SetSizeNegY(float f) { m_Params.m_fSizeNegY = f; OnAuthoredChange(); }
void plBrushComponent::SetSizePosY(float f) { m_Params.m_fSizePosY = f; OnAuthoredChange(); }
void plBrushComponent::SetSizeNegZ(float f) { m_Params.m_fSizeNegZ = f; OnAuthoredChange(); }
void plBrushComponent::SetSizePosZ(float f) { m_Params.m_fSizePosZ = f; OnAuthoredChange(); }

void plBrushComponent::SetDetail(plUInt32 uiDetail)
{
  m_Params.m_uiDetail = uiDetail;
  OnAuthoredChange();
}

void plBrushComponent::SetCurvature(plAngle curvature)
{
  m_Params.m_Curvature = curvature;
  OnAuthoredChange();
}

void plBrushComponent::SetThickness(float f)
{
  m_Params.m_fThickness = f;
  OnAuthoredChange();
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
  plBrushFaceAttributeCodec::Parse(m_FaceData, m_FaceAttributes);

  // attributes do not change what a cutter carves, so the world module need not hear about them
  InvalidateMesh();
}

void plBrushComponent::SetStoredMesh(const plDataBuffer& data)
{
  m_StoredMesh = data;
  OnAuthoredChange();
}

void plBrushComponent::SetMaterial(plUInt32 uiIndex, const plMaterialResourceHandle& hMaterial)
{
  // face slots are 8 bit
  if (uiIndex > 255)
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

void plBrushComponent::BuildEditableMesh(plEditableMesh& out_mesh) const
{
  if (m_Shape == plBrushShape::Editable)
  {
    const bool bLoaded = out_mesh.FromDataBuffer(m_StoredMesh).Succeeded();
    const bool bDamaged = !bLoaded && !m_StoredMesh.IsEmpty();

    if (bDamaged && !m_bDamagedMesh)
    {
      plLog::Error("Brush '{}': the stored mesh is damaged and was not loaded. The brush renders nothing until it is re-authored.", GetOwner() ? GetOwner()->GetName() : "");
    }

    m_bDamagedMesh = bDamaged;
  }
  else
  {
    m_bDamagedMesh = false;
    plBrushGenerators::Generate(m_Shape, m_Params, out_mesh);
  }
}

void plBrushComponent::BuildEvaluatedMesh(plEditableMesh& out_mesh) const
{
  BuildEditableMesh(out_mesh);

  plHybridArray<plBrushCutterInput, 4> cutters;
  plHybridArray<plBrushRoomInput, 4> rooms;
  plBrushRoomContext roomContext;
  roomContext.m_uiTargetBrushId = m_uiBrushId;

  // a cutter never gets carved itself
  if (!m_bSubtractive && !out_mesh.m_Faces.IsEmpty())
  {
    if (const plBrushWorldModule* pModule = GetWorld()->GetModule<plBrushWorldModule>())
    {
      // the base mesh bounds decide which cutters matter; a room's passages stay inside its cutters
      plBoundingBox worldBounds = out_mesh.ComputeBounds();
      worldBounds.TransformFromOrigin(GetOwner()->GetGlobalTransform().GetAsMat4());

      const bool bHasRooms = plMeshCSG::HasInvertedShell(out_mesh);
      pModule->GatherCutters(this, worldBounds, cutters, bHasRooms);

      if (bHasRooms)
      {
        pModule->GatherRooms(this, cutters, rooms);
        roomContext.m_OtherRooms = rooms.GetArrayPtr();
      }
    }
  }

  plBrushEvaluator::Evaluate(out_mesh, GetOwner()->GetGlobalTransform(), cutters, m_FaceAttributes, nullptr, &roomContext);
}

const plTransform* plBrushComponent::GetUVSpace(plTransform& ref_storage) const
{
  if (m_bTextureLock)
    return nullptr;

  ref_storage = GetOwner()->GetGlobalTransform();
  return &ref_storage;
}

void plBrushComponent::GenerateMeshName(const plEditableMesh& mesh, plStringBuilder& out_sName) const
{
  // The mesh identity is a content hash of the EVALUATED mesh: identical brushes (duplicates) share
  // one GPU mesh, and any change to shape, parameters, overlapping cutters, face overrides, shading or
  // the material list produces a new resource.
  plDefaultMemoryStreamStorage storage;
  plMemoryStreamWriter writer(&storage);
  mesh.Serialize(writer);

  for (const auto& mat : m_Materials)
  {
    writer << mat.GetResourceID();
  }

  writer << m_SmoothingAngle;

  // world-space UVs depend on where the brush stands
  plTransform uvSpace;
  if (GetUVSpace(uvSpace) != nullptr)
  {
    writer << uvSpace.m_vPosition;
    writer << uvSpace.m_qRotation;
    writer << uvSpace.m_vScale;
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
  plTransform uvSpace;
  mesh.BuildRenderGeometry(geom, ranges, m_SmoothingAngle, GetUVSpace(uvSpace));

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
  plEditableMesh mesh;
  BuildEvaluatedMesh(mesh);

  if (m_bSubtractive)
  {
    // any closed shape cuts: non-convex cutters are split into convex pieces when they are evaluated
    plEditableMesh pieces;
    const bool bConvex = mesh.m_Faces.IsEmpty() || plMeshCSG::MakeConvexCutter(mesh, pieces).Succeeded();

    if (!bConvex && m_bCutterConvex)
    {
      plLog::Warning("Brush cutter '{}' cannot be split into convex pieces (is it closed?), so it cuts nothing.", GetOwner()->GetName());
    }

    m_bCutterConvex = bConvex;
  }

  plStringBuilder sResourceName;
  GenerateMeshName(mesh, sResourceName);

  plTypedResourceHandle<ResourceType> hResource = plResourceManager::GetExistingResource<ResourceType>(sResourceName);
  if (hResource.IsValid())
    return hResource;

  plMeshResourceDescriptor desc;
  GenerateMeshResourceDescriptor(desc, mesh);

  return plResourceManager::GetOrCreateResource<ResourceType>(sResourceName, std::move(desc), sResourceName);
}

void plBrushComponent::OnBuildStaticMesh(plMsgBuildStaticMesh& msg) const
{
  // a cutter is negative space — its collision is the holes it leaves in others
  if (!m_bGenerateCollision || m_bSubtractive)
    return;

  plEditableMesh mesh;
  BuildEvaluatedMesh(mesh);

  plGeometry geom;
  plDynamicArray<plBrushSubMeshRange> ranges;
  mesh.BuildRenderGeometry(geom, ranges);

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
  if (m_bSubtractive)
    return;

  if (msg.m_Mode == plWorldGeoExtractionUtil::ExtractionMode::CollisionMesh && (m_bGenerateCollision == false || GetOwner()->IsDynamic()))
    return;

  if (msg.m_Mode == plWorldGeoExtractionUtil::ExtractionMode::NavMeshGeneration && (m_bIncludeInNavmesh == false || GetOwner()->IsDynamic()))
    return;

  msg.AddMeshObject(GetOwner()->GetGlobalTransform(), GenerateMesh<plCpuMeshResource>());
}

void plBrushComponent::OnMsgExtractOccluderData(plMsgExtractOccluderData& msg) const
{
  if (!IsActiveAndInitialized() || !m_bUseAsOccluder || m_bSubtractive)
    return;

  if (m_pOccluderObject == nullptr)
  {
    plEditableMesh mesh;
    BuildEvaluatedMesh(mesh);

    plStringBuilder sResourceName;
    GenerateMeshName(mesh, sResourceName);
    sResourceName.Append("_Occluder");

    m_pOccluderObject = plRasterizerObject::GetObject(sResourceName);

    if (m_pOccluderObject == nullptr)
    {
      plGeometry geom;
      plDynamicArray<plBrushSubMeshRange> ranges;
      mesh.BuildRenderGeometry(geom, ranges);

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
  // property changes arrive in bursts (a drag in the editor undoes and redoes several properties per mouse
  // move), so the mesh is regenerated once, by the world module's next update
  if (m_hMesh.IsValid())
  {
    m_bMeshDirty = true;
  }
}

void plBrushComponent::InvalidateBrushMesh()
{
  if (m_hMesh.IsValid())
  {
    RegenerateMesh();
  }
}

void plBrushComponent::RebuildMeshIfDirty()
{
  if (m_bMeshDirty)
  {
    RegenerateMesh();
  }
}

void plBrushComponent::RegenerateMesh()
{
  m_bMeshDirty = false;
  m_pOccluderObject = nullptr;

  m_hMesh.Invalidate();
  m_hMesh = GenerateMesh<plMeshResource>();

  // static brushes cache their render data, which still points at the old mesh
  InvalidateCachedRenderData();
  TriggerLocalBoundsUpdate();
}


PL_STATICLINK_FILE(ModelingPlugin, ModelingPlugin_Modeling_Implementation_BrushComponent);
