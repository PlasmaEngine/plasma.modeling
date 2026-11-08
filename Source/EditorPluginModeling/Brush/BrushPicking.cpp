#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorPluginModeling/Brush/BrushPicking.h>
#include <ModelingPlugin/Modeling/BrushGenerators.h>
#include <ModelingPlugin/Modeling/MeshCSG.h>
#include <ToolsFoundation/Object/DocumentObjectBase.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

bool plBrushPicking::IsBrushComponent(const plDocumentObject* pObject)
{
  return pObject != nullptr && pObject->GetTypeAccessor().GetType()->GetTypeName() == "plBrushComponent";
}

plResult plBrushPicking::BuildMeshFromObject(const plDocumentObject* pComponent, plEditableMesh& out_mesh)
{
  out_mesh.Clear();

  if (!IsBrushComponent(pComponent))
    return PL_FAILURE;

  const auto& acc = pComponent->GetTypeAccessor();

  const plBrushShape::Enum shape = static_cast<plBrushShape::Enum>(acc.GetValue("Shape").ConvertTo<plInt32>());

  if (shape == plBrushShape::Editable)
  {
    const plDataBuffer stored = acc.GetValue("StoredMesh").ConvertTo<plDataBuffer>();

    // an empty stored mesh is a valid (empty) brush, a damaged one is not
    if (stored.IsEmpty())
      return PL_SUCCESS;

    return out_mesh.FromDataBuffer(stored);
  }

  plBrushGeneratorParams params;
  params.m_fSizeNegX = acc.GetValue("SizeNegX").ConvertTo<float>();
  params.m_fSizePosX = acc.GetValue("SizePosX").ConvertTo<float>();
  params.m_fSizeNegY = acc.GetValue("SizeNegY").ConvertTo<float>();
  params.m_fSizePosY = acc.GetValue("SizePosY").ConvertTo<float>();
  params.m_fSizeNegZ = acc.GetValue("SizeNegZ").ConvertTo<float>();
  params.m_fSizePosZ = acc.GetValue("SizePosZ").ConvertTo<float>();
  params.m_uiDetail = acc.GetValue("Detail").ConvertTo<plUInt32>();
  params.m_Curvature = acc.GetValue("Curvature").ConvertTo<plAngle>();
  params.m_fThickness = acc.GetValue("Thickness").ConvertTo<float>();

  plBrushGenerators::Generate(shape, params, out_mesh);
  return PL_SUCCESS;
}

void plBrushPicking::ReadFaceAttributes(const plDocumentObject* pComponent, plBrushFaceAttributeMap& out_attributes)
{
  plBrushFaceAttributeCodec::Parse(pComponent->GetTypeAccessor().GetValue("FaceData").ConvertTo<plDataBuffer>(), out_attributes);
}

plTransform plBrushPicking::GetComponentGlobalTransform(const plDocumentObject* pComponent)
{
  const plGameObjectDocument* pGameDoc = plDynamicCast<const plGameObjectDocument*>(pComponent->GetDocumentObjectManager()->GetDocument());

  if (pGameDoc == nullptr || pComponent->GetParent() == nullptr)
    return plTransform::MakeIdentity();

  return pGameDoc->GetGlobalTransform(pComponent->GetParent());
}

const plDocumentObject* plBrushPicking::FindCutterScope(const plDocumentObject* pComponent)
{
  const plDocumentObject* pRoot = pComponent->GetDocumentObjectManager()->GetRootObject();

  for (const plDocumentObject* pObject = pComponent->GetParent(); pObject != nullptr && pObject != pRoot; pObject = pObject->GetParent())
  {
    for (const plDocumentObject* pChild : pObject->GetChildren())
    {
      if (pChild->GetTypeAccessor().GetType()->GetTypeName() == "plBrushGroupComponent" &&
          pChild->GetTypeAccessor().GetValue("IsolateCutters").ConvertTo<bool>())
      {
        return pObject;
      }
    }
  }

  return nullptr;
}

namespace
{
  void CollectBrushesRecursive(const plDocumentObject* pObject, plDynamicArray<const plDocumentObject*>& out_brushes)
  {
    for (const plDocumentObject* pChild : pObject->GetChildren())
    {
      if (plBrushPicking::IsBrushComponent(pChild))
      {
        out_brushes.PushBack(pChild);
      }

      CollectBrushesRecursive(pChild, out_brushes);
    }
  }
} // namespace

void plBrushPicking::FindAllBrushComponents(const plDocumentObject* pAnyDocumentObject, plDynamicArray<const plDocumentObject*>& out_brushes)
{
  out_brushes.Clear();
  CollectBrushesRecursive(pAnyDocumentObject->GetDocumentObjectManager()->GetRootObject(), out_brushes);
}

void plBrushPicking::GatherCutters(const plDocumentObject* pTarget, const plBoundingBox& targetWorldBounds, plDynamicArray<plBrushCutterInput>& out_cutters, plDynamicArray<const plDocumentObject*>* out_pCutterObjects, bool bForRooms)
{
  out_cutters.Clear();

  if (out_pCutterObjects)
    out_pCutterObjects->Clear();

  if (!targetWorldBounds.IsValid())
    return;

  const plDocumentObject* pScope = FindCutterScope(pTarget);

  plDynamicArray<const plDocumentObject*> brushes;
  FindAllBrushComponents(pTarget, brushes);

  struct Candidate
  {
    const plDocumentObject* m_pObject;
    plUInt32 m_uiBrushId;
    plTransform m_Transform;
  };

  plHybridArray<Candidate, 8> candidates;

  for (const plDocumentObject* pBrush : brushes)
  {
    if (pBrush == pTarget || !pBrush->GetTypeAccessor().GetValue("Subtractive").ConvertTo<bool>())
      continue;

    if (FindCutterScope(pBrush) != pScope)
      continue;

    Candidate& c = candidates.ExpandAndGetRef();
    c.m_pObject = pBrush;
    c.m_uiBrushId = pBrush->GetTypeAccessor().GetValue("BrushId").ConvertTo<plUInt32>();
    c.m_Transform = GetComponentGlobalTransform(pBrush);
  }

  candidates.Sort([](const Candidate& a, const Candidate& b)
    { return plBrushEvaluator::CutterLess(a.m_uiBrushId, a.m_Transform.m_vPosition, b.m_uiBrushId, b.m_Transform.m_vPosition); });

  plDynamicArray<plBrushCutterInput> all;
  plDynamicArray<const plDocumentObject*> allObjects;

  for (const Candidate& c : candidates)
  {
    plBrushCutterInput input;

    if (BuildMeshFromObject(c.m_pObject, input.m_Mesh).Failed() || input.m_Mesh.m_Faces.IsEmpty())
      continue;

    input.m_GlobalTransform = c.m_Transform;
    input.m_uiBrushId = c.m_uiBrushId;
    input.m_WorldBounds = input.m_Mesh.ComputeBounds();
    input.m_WorldBounds.TransformFromOrigin(c.m_Transform.GetAsMat4());

    all.PushBack(std::move(input));
    allObjects.PushBack(c.m_pObject);
  }

  // a room's passage ends where another cutter begins: rooms also see the cutters next to their own
  plBoundingBox reach = targetWorldBounds;

  if (bForRooms)
  {
    for (const plBrushCutterInput& input : all)
    {
      if (input.m_WorldBounds.Overlaps(targetWorldBounds))
        reach.ExpandToInclude(input.m_WorldBounds);
    }
  }

  for (plUInt32 i = 0; i < all.GetCount(); ++i)
  {
    if (!all[i].m_WorldBounds.Overlaps(reach))
      continue;

    out_cutters.PushBack(std::move(all[i]));

    if (out_pCutterObjects)
      out_pCutterObjects->PushBack(allObjects[i]);
  }
}

void plBrushPicking::GatherRooms(const plDocumentObject* pTarget, plArrayPtr<const plBrushCutterInput> cutters, plDynamicArray<plBrushRoomInput>& out_rooms)
{
  out_rooms.Clear();

  if (cutters.IsEmpty())
    return;

  const plDocumentObject* pScope = FindCutterScope(pTarget);

  plDynamicArray<const plDocumentObject*> brushes;
  FindAllBrushComponents(pTarget, brushes);

  plEditableMesh mesh;

  for (const plDocumentObject* pBrush : brushes)
  {
    if (pBrush == pTarget || pBrush->GetTypeAccessor().GetValue("Subtractive").ConvertTo<bool>() || FindCutterScope(pBrush) != pScope)
      continue;

    if (BuildMeshFromObject(pBrush, mesh).Failed() || !plMeshCSG::HasInvertedShell(mesh))
      continue;

    const plTransform transform = GetComponentGlobalTransform(pBrush);

    // the engine compares against the rendered bounds, which for a room are its own shell's
    plBoundingBox bounds = mesh.ComputeBounds();
    bounds.TransformFromOrigin(transform.GetAsMat4());

    bool bReached = false;

    for (const plBrushCutterInput& cutter : cutters)
    {
      bReached |= cutter.m_WorldBounds.Overlaps(bounds);
    }

    if (!bReached)
      continue;

    plBrushRoomInput& input = out_rooms.ExpandAndGetRef();
    plBrushEvaluator::ExtractRoomShells(mesh, input.m_Mesh);
    input.m_GlobalTransform = transform;
    input.m_uiBrushId = pBrush->GetTypeAccessor().GetValue("BrushId").ConvertTo<plUInt32>();
  }
}

void plBrushPicking::CarveMesh(const plDocumentObject* pComponent, plEditableMesh& inout_mesh, plArrayPtr<const plBrushCutterInput> cutters, plDynamicArray<plUInt32>* out_pRefusedCutters)
{
  plDynamicArray<plBrushRoomInput> rooms;
  plBrushRoomContext roomContext;
  roomContext.m_uiTargetBrushId = pComponent->GetTypeAccessor().GetValue("BrushId").ConvertTo<plUInt32>();

  if (plMeshCSG::HasInvertedShell(inout_mesh))
  {
    GatherRooms(pComponent, cutters, rooms);
    roomContext.m_OtherRooms = rooms.GetArrayPtr();
  }

  plBrushEvaluator::Carve(inout_mesh, GetComponentGlobalTransform(pComponent), cutters, out_pRefusedCutters, &roomContext);
}

plResult plBrushPicking::BuildEvaluatedMeshFromObject(const plDocumentObject* pComponent, plEditableMesh& out_mesh, plDynamicArray<plUInt32>* out_pRefusedCutters)
{
  if (BuildMeshFromObject(pComponent, out_mesh).Failed())
    return PL_FAILURE;

  plBrushFaceAttributeMap attributes;
  ReadFaceAttributes(pComponent, attributes);

  const bool bSubtractive = pComponent->GetTypeAccessor().GetValue("Subtractive").ConvertTo<bool>();
  const plTransform targetTransform = GetComponentGlobalTransform(pComponent);

  if (!bSubtractive && !out_mesh.m_Faces.IsEmpty())
  {
    plBoundingBox targetWorldBounds = out_mesh.ComputeBounds();
    targetWorldBounds.TransformFromOrigin(targetTransform.GetAsMat4());

    plDynamicArray<plBrushCutterInput> cutters;
    GatherCutters(pComponent, targetWorldBounds, cutters, nullptr, plMeshCSG::HasInvertedShell(out_mesh));

    CarveMesh(pComponent, out_mesh, cutters, out_pRefusedCutters);
  }

  plBrushFaceAttributeCodec::Apply(attributes, out_mesh);
  return PL_SUCCESS;
}

plUInt32 plBrushPicking::PickFaceAtLocalPos(const plEditableMesh& mesh, const plVec3& vLocalPos)
{
  const plInt32 iFace = PickFaceIndexAtLocalPos(mesh, vLocalPos);

  return (iFace >= 0) ? mesh.m_Faces[iFace].m_uiFaceId : plInvalidIndex;
}

plInt32 plBrushPicking::PickFaceIndexAtLocalPos(const plEditableMesh& mesh, const plVec3& vLocalPos)
{
  plUInt32 uiBestFace = plInvalidIndex;
  float fBestDist = plMath::MaxValue<float>();

  plUInt32 uiBestFaceNoContainment = plInvalidIndex;
  float fBestDistNoContainment = plMath::MaxValue<float>();

  plDynamicArray<plUInt32> tris;

  for (plUInt32 uiFace = 0; uiFace < mesh.m_Faces.GetCount(); ++uiFace)
  {
    const plBrushFace& face = mesh.m_Faces[uiFace];

    if ((face.m_uiFlags & plBrushFaceFlags::Internal) != 0)
      continue;

    const plVec3 vNormal = mesh.ComputeFaceNormal(uiFace);
    const plVec3 vCenter = mesh.ComputeFaceCenter(uiFace);

    const float fPlaneDist = plMath::Abs(vNormal.Dot(vLocalPos - vCenter));

    if (fPlaneDist >= fBestDistNoContainment && fPlaneDist >= fBestDist)
      continue;

    if (fPlaneDist < fBestDistNoContainment)
    {
      uiBestFaceNoContainment = uiFace;
      fBestDistNoContainment = fPlaneDist;
    }

    if (fPlaneDist >= fBestDist)
      continue;

    // containment against the face's triangles, so concave faces pick correctly; a small tolerance
    // keeps edge and corner hits on a face
    const plVec3 vOnPlane = vLocalPos - vNormal * vNormal.Dot(vLocalPos - vCenter);
    mesh.TriangulateFace(uiFace, tris);

    bool bInside = false;

    for (plUInt32 t = 0; t + 2 < tris.GetCount() && !bInside; t += 3)
    {
      const plVec3& a = mesh.m_Vertices[face.m_Corners[tris[t]]];
      const plVec3& b = mesh.m_Vertices[face.m_Corners[tris[t + 1]]];
      const plVec3& c = mesh.m_Vertices[face.m_Corners[tris[t + 2]]];

      if (vNormal.Dot((b - a).CrossRH(vOnPlane - a)) >= -0.001f && vNormal.Dot((c - b).CrossRH(vOnPlane - b)) >= -0.001f &&
          vNormal.Dot((a - c).CrossRH(vOnPlane - c)) >= -0.001f)
      {
        bInside = true;
      }
    }

    if (bInside)
    {
      uiBestFace = uiFace;
      fBestDist = fPlaneDist;
    }
  }

  // the pick position sits on the surface, so a contained face should exist; the nearest-plane
  // fallback covers numeric edge cases at corners
  const plUInt32 uiPicked = (uiBestFace != plInvalidIndex) ? uiBestFace : uiBestFaceNoContainment;

  return (uiPicked != plInvalidIndex) ? static_cast<plInt32>(uiPicked) : -1;
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_BrushPicking);
