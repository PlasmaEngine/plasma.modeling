#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorPluginModeling/Brush/BrushPicking.h>
#include <ModelingPlugin/Modeling/BrushGenerators.h>
#include <ModelingPlugin/Modeling/MeshCSG.h>
#include <ToolsFoundation/Object/DocumentObjectBase.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

plResult plBrushPicking::BuildMeshFromObject(const plDocumentObject* pComponent, plEditableMesh& out_mesh)
{
  out_mesh.Clear();

  if (pComponent == nullptr)
    return PL_FAILURE;

  const auto& acc = pComponent->GetTypeAccessor();

  if (acc.GetType()->GetTypeName() != "plBrushComponent")
    return PL_FAILURE;

  const plBrushShape::Enum shape = static_cast<plBrushShape::Enum>(acc.GetValue("Shape").ConvertTo<plInt32>());

  if (shape == plBrushShape::Editable)
  {
    return out_mesh.FromDataBuffer(acc.GetValue("StoredMesh").ConvertTo<plDataBuffer>());
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

plTransform plBrushPicking::GetComponentGlobalTransform(const plDocumentObject* pComponent)
{
  const plGameObjectDocument* pGameDoc = plDynamicCast<const plGameObjectDocument*>(pComponent->GetDocumentObjectManager()->GetDocument());

  if (pGameDoc == nullptr || pComponent->GetParent() == nullptr)
    return plTransform::MakeIdentity();

  return pGameDoc->GetGlobalTransform(pComponent->GetParent());
}

namespace
{
  void CollectCuttersRecursive(const plDocumentObject* pObject, const plDocumentObject* pExclude, plDynamicArray<const plDocumentObject*>& out_cutters)
  {
    for (const plDocumentObject* pChild : pObject->GetChildren())
    {
      if (pChild != pExclude && pChild->GetTypeAccessor().GetType()->GetTypeName() == "plBrushComponent")
      {
        if (pChild->GetTypeAccessor().GetValue("Subtractive").ConvertTo<bool>())
        {
          out_cutters.PushBack(pChild);
        }
      }

      CollectCuttersRecursive(pChild, pExclude, out_cutters);
    }
  }
} // namespace

void plBrushPicking::FindCutterComponents(const plDocumentObject* pAnyDocumentObject, const plDocumentObject* pExclude, plDynamicArray<const plDocumentObject*>& out_cutters)
{
  out_cutters.Clear();

  const plDocumentObject* pRoot = pAnyDocumentObject->GetDocumentObjectManager()->GetRootObject();

  CollectCuttersRecursive(pRoot, pExclude, out_cutters);

  // same deterministic order as plBrushWorldModule::QueryCutters — cap face ids must match
  out_cutters.Sort([](const plDocumentObject* pA, const plDocumentObject* pB)
    {
      const plVec3 a = plBrushPicking::GetComponentGlobalTransform(pA).m_vPosition;
      const plVec3 b = plBrushPicking::GetComponentGlobalTransform(pB).m_vPosition;

      if (a.x != b.x)
        return a.x < b.x;
      if (a.y != b.y)
        return a.y < b.y;
      return a.z < b.z;
    });
}

plResult plBrushPicking::BuildEvaluatedMeshFromObject(const plDocumentObject* pComponent, plEditableMesh& out_mesh)
{
  if (BuildMeshFromObject(pComponent, out_mesh).Failed())
    return PL_FAILURE;

  const bool bSubtractive = pComponent->GetTypeAccessor().GetValue("Subtractive").ConvertTo<bool>();

  if (bSubtractive || out_mesh.m_Faces.IsEmpty())
    return PL_SUCCESS;

  plDynamicArray<const plDocumentObject*> cutters;
  FindCutterComponents(pComponent, pComponent, cutters);

  if (cutters.IsEmpty())
    return PL_SUCCESS;

  const plTransform targetTransform = GetComponentGlobalTransform(pComponent);

  plBoundingBox targetWorldBounds = out_mesh.ComputeBounds();
  targetWorldBounds.TransformFromOrigin(targetTransform.GetAsMat4());

  plEditableMesh cutterMesh, result;

  for (const plDocumentObject* pCutter : cutters)
  {
    if (BuildMeshFromObject(pCutter, cutterMesh).Failed() || cutterMesh.m_Faces.IsEmpty())
      continue;

    const plTransform cutterTransform = GetComponentGlobalTransform(pCutter);

    plBoundingBox cutterWorldBounds = cutterMesh.ComputeBounds();
    cutterWorldBounds.TransformFromOrigin(cutterTransform.GetAsMat4());

    if (!cutterWorldBounds.Overlaps(targetWorldBounds))
      continue;

    const plTransform cutterToTarget = plTransform::MakeLocalTransform(targetTransform, cutterTransform);

    if (plMeshCSG::SubtractCutter(out_mesh, cutterMesh, cutterToTarget, 0, result).Succeeded())
    {
      out_mesh = result;
    }

    if (out_mesh.m_Faces.IsEmpty())
      break;
  }

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

  for (plUInt32 uiFace = 0; uiFace < mesh.m_Faces.GetCount(); ++uiFace)
  {
    const plBrushFace& face = mesh.m_Faces[uiFace];
    const plVec3 vNormal = mesh.ComputeFaceNormal(uiFace);
    const plVec3& vAnchor = mesh.m_Vertices[face.m_Corners[0]];

    const float fPlaneDist = plMath::Abs(vNormal.Dot(vLocalPos - vAnchor));

    if (fPlaneDist >= fBestDistNoContainment)
      continue;

    uiBestFaceNoContainment = uiFace;
    fBestDistNoContainment = fPlaneDist;

    if (fPlaneDist >= fBestDist)
      continue;

    // containment: the point must lie inside every edge, testing in the face plane
    bool bInside = true;

    for (plUInt32 i = 0; i < face.m_Corners.GetCount(); ++i)
    {
      const plVec3& vEdgeStart = mesh.m_Vertices[face.m_Corners[i]];
      const plVec3& vEdgeEnd = mesh.m_Vertices[face.m_Corners[(i + 1) % face.m_Corners.GetCount()]];

      const plVec3 vEdge = vEdgeEnd - vEdgeStart;
      const plVec3 vToPoint = vLocalPos - vEdgeStart;

      // negative = outside this edge (corners wind CCW around the normal), with a small tolerance
      // so edge and corner drops still land on a face
      if (vNormal.Dot(vEdge.CrossRH(vToPoint)) < -0.001f)
      {
        bInside = false;
        break;
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
