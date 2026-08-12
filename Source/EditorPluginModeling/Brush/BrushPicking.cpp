#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorPluginModeling/Brush/BrushPicking.h>
#include <ModelingPlugin/Modeling/BrushGenerators.h>
#include <ToolsFoundation/Object/DocumentObjectBase.h>

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
    return PL_FAILURE;

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

plUInt32 plBrushPicking::PickFaceAtLocalPos(const plEditableMesh& mesh, const plVec3& vLocalPos)
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

  // the drop position sits on the surface, so a contained face should exist; the nearest-plane
  // fallback covers numeric edge cases at corners
  const plUInt32 uiPicked = (uiBestFace != plInvalidIndex) ? uiBestFace : uiBestFaceNoContainment;

  return (uiPicked != plInvalidIndex) ? mesh.m_Faces[uiPicked].m_uiFaceId : plInvalidIndex;
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_BrushPicking);
