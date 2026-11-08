#include <ModelingPlugin/ModelingPluginPCH.h>

#include <ModelingPlugin/Modeling/MeshCSG.h>
#include <ModelingPlugin/Modeling/MeshTopology.h>
#include <ModelingPlugin/Modeling/MeshValidator.h>

void plMeshValidator::Validate(const plEditableMesh& mesh, plMeshValidationReport& out_report, bool bCheckShellConvexity, float fPlanarityTolerance)
{
  out_report = plMeshValidationReport();

  out_report.m_bStructurallyValid = mesh.IsStructurallyValid();

  if (!out_report.m_bStructurallyValid)
    return;

  for (plUInt32 f = 0; f < mesh.m_Faces.GetCount(); ++f)
  {
    if (mesh.ComputeFaceArea(f) < 0.000001f)
    {
      ++out_report.m_uiDegenerateFaces;
      continue;
    }

    if (mesh.ComputeFacePlanarityError(f) > fPlanarityTolerance)
    {
      ++out_report.m_uiNonPlanarFaces;
      out_report.m_NonPlanarFaceIds.PushBack(mesh.m_Faces[f].m_uiFaceId);
    }

    // concave: a corner turning against the face normal
    const plBrushFace& face = mesh.m_Faces[f];
    const plVec3 vNormal = mesh.ComputeFaceNormal(f);
    const plUInt32 n = face.m_Corners.GetCount();

    for (plUInt32 i = 0; i < n; ++i)
    {
      const plVec3& a = mesh.m_Vertices[face.m_Corners[(i + n - 1) % n]];
      const plVec3& b = mesh.m_Vertices[face.m_Corners[i]];
      const plVec3& c = mesh.m_Vertices[face.m_Corners[(i + 1) % n]];

      if ((b - a).CrossRH(c - b).Dot(vNormal) < -0.000001f)
      {
        ++out_report.m_uiConcaveFaces;
        break;
      }
    }
  }

  plMeshTopology topology;
  topology.Build(mesh);

  // A boundary edge is only a hole if nothing runs back along it. Carving leaves T-junctions: one long
  // edge on one side, several short ones on the other, all "boundary" by index but closed in space.
  struct Segment
  {
    plVec3 m_vFrom = plVec3::MakeZero();
    plVec3 m_vTo = plVec3::MakeZero();
  };

  plDynamicArray<Segment> boundary;

  for (plUInt32 h = 0; h < topology.GetHalfEdgeCount(); ++h)
  {
    if (!topology.IsBoundary(h))
      continue;

    const auto& he = topology.GetHalfEdge(h);
    boundary.PushBack({mesh.m_Vertices[he.m_uiFrom], mesh.m_Vertices[he.m_uiTo]});
  }

  constexpr float fTolerance = 0.0005f;

  // Most boundary edges are closed by an exact reverse at the same positions: split vertices (UV seams of
  // imported meshes) make them boundary by index only. Those are paired by hash, without any search.
  auto positionKey = [](const plVec3& v)
  {
    const plInt64 key[3] = {static_cast<plInt64>(plMath::Round(v.x * 10000.0f)), static_cast<plInt64>(plMath::Round(v.y * 10000.0f)), static_cast<plInt64>(plMath::Round(v.z * 10000.0f))};
    return plHashingUtils::xxHash64(key, sizeof(key));
  };

  auto edgeKey = [&](const plVec3& vFrom, const plVec3& vTo)
  {
    const plUInt64 keys[2] = {positionKey(vFrom), positionKey(vTo)};
    return plHashingUtils::xxHash64(keys, sizeof(keys));
  };

  plHashTable<plUInt64, plUInt32> directedEdges;
  for (const Segment& seg : boundary)
  {
    directedEdges[edgeKey(seg.m_vFrom, seg.m_vTo)]++;
  }

  // The rest (T-junctions of carved faces, real holes) needs the coverage test against segments running
  // back along the same line; bucketing by line keeps that test local.
  auto lineKey = [](const plVec3& vFrom, const plVec3& vUnit)
  {
    plVec3 vDir = vUnit;
    if (vDir.x < -0.001f || (vDir.x <= 0.001f && (vDir.y < -0.001f || (vDir.y <= 0.001f && vDir.z < 0.0f))))
      vDir = -vDir;

    const plVec3 vClosest = vFrom - vDir * vFrom.Dot(vDir);
    const plInt64 key[6] = {static_cast<plInt64>(plMath::Round(vDir.x * 100.0f)), static_cast<plInt64>(plMath::Round(vDir.y * 100.0f)), static_cast<plInt64>(plMath::Round(vDir.z * 100.0f)),
      static_cast<plInt64>(plMath::Round(vClosest.x * 100.0f)), static_cast<plInt64>(plMath::Round(vClosest.y * 100.0f)), static_cast<plInt64>(plMath::Round(vClosest.z * 100.0f))};
    return plHashingUtils::xxHash64(key, sizeof(key));
  };

  plDynamicArray<const Segment*> unpaired;
  plHashTable<plUInt64, plDynamicArray<const Segment*>> byLine;

  for (const Segment& seg : boundary)
  {
    const plVec3 vDir = seg.m_vTo - seg.m_vFrom;
    const float fLength = vDir.GetLength();

    if (fLength < fTolerance || directedEdges.Contains(edgeKey(seg.m_vTo, seg.m_vFrom)))
      continue;

    unpaired.PushBack(&seg);
    byLine[lineKey(seg.m_vFrom, vDir / fLength)].PushBack(&seg);
  }

  for (const Segment* pSeg : unpaired)
  {
    const Segment& seg = *pSeg;
    const plVec3 vDir = seg.m_vTo - seg.m_vFrom;
    const float fLength = vDir.GetLength();

    const plVec3 vUnit = vDir / fLength;
    float fCovered = 0.0f;

    for (const Segment* pOther : byLine[lineKey(seg.m_vFrom, vUnit)])
    {
      const Segment& other = *pOther;

      // opposite direction, on the same line
      const plVec3 vOtherDir = other.m_vTo - other.m_vFrom;

      if (vOtherDir.Dot(vUnit) >= 0.0f)
        continue;

      const plVec3 vToFrom = other.m_vFrom - seg.m_vFrom;
      const plVec3 vToTo = other.m_vTo - seg.m_vFrom;

      if ((vToFrom - vUnit * vToFrom.Dot(vUnit)).GetLength() > fTolerance || (vToTo - vUnit * vToTo.Dot(vUnit)).GetLength() > fTolerance)
        continue;

      const float t0 = plMath::Clamp(vToFrom.Dot(vUnit), 0.0f, fLength);
      const float t1 = plMath::Clamp(vToTo.Dot(vUnit), 0.0f, fLength);
      fCovered += plMath::Abs(t1 - t0);
    }

    if (fCovered < fLength - fTolerance)
      ++out_report.m_uiOpenEdges;
  }

  out_report.m_uiNonManifoldEdges = topology.GetNonManifoldEdgeCount();

  if (bCheckShellConvexity)
  {
    for (const plBrushShell& shell : mesh.m_Shells)
    {
      if (!plMeshCSG::IsShellConvex(mesh, shell))
        ++out_report.m_uiNonConvexShells;
    }
  }
}


PL_STATICLINK_FILE(ModelingPlugin, ModelingPlugin_Modeling_Implementation_MeshValidator);
