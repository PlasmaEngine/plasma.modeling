#include <ModelingPlugin/ModelingPluginPCH.h>

#include <Core/Graphics/Geometry.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/IO/MemoryStream.h>
#include <ModelingPlugin/Modeling/EditableMesh.h>

plUInt32 plBrushFaceIds::MakeCutFaceId(plUInt32 uiCutterId, plUInt32 uiShell, plUInt32 uiPlane, bool bOutsidePiece)
{
  const plUInt32 key[4] = {uiCutterId, uiShell, uiPlane, bOutsidePiece ? 1u : 0u};
  return c_uiDerivedBit | (plHashingUtils::xxHash32(key, sizeof(key)) & ~c_uiDerivedBit);
}

void plEditableMesh::Clear()
{
  m_Vertices.Clear();
  m_Faces.Clear();
  m_Shells.Clear();
  m_uiNextFaceId = 1;
}

plUInt32 plEditableMesh::AddVertex(const plVec3& vPos)
{
  // vPos may point into m_Vertices itself, which the push can reallocate
  const plVec3 vCopy = vPos;
  m_Vertices.PushBack(vCopy);
  return m_Vertices.GetCount() - 1;
}

plUInt32 plEditableMesh::AddFace(plArrayPtr<const plUInt32> corners, plUInt32 uiFaceId, plUInt8 uiMaterialSlot)
{
  PL_ASSERT_DEV(corners.GetCount() >= 3, "A face needs at least 3 corners");

  plBrushFace& face = m_Faces.ExpandAndGetRef();
  face.m_uiFaceId = uiFaceId;
  face.m_uiMaterialSlot = uiMaterialSlot;
  face.m_Corners.SetCount(static_cast<plUInt16>(corners.GetCount()));

  for (plUInt32 i = 0; i < corners.GetCount(); ++i)
  {
    face.m_Corners[i] = corners[i];
  }

  if (!plBrushFaceIds::IsDerived(uiFaceId))
  {
    m_uiNextFaceId = plMath::Max(m_uiNextFaceId, uiFaceId + 1);
  }

  if (!m_Shells.IsEmpty())
  {
    m_Shells.PeekBack().m_uiNumFaces++;
  }

  return m_Faces.GetCount() - 1;
}

plUInt32 plEditableMesh::AddFace(plArrayPtr<const plVec3> positions, plUInt32 uiFaceId, plUInt8 uiMaterialSlot)
{
  plSmallArray<plUInt32, 8> corners;

  for (const plVec3& pos : positions)
  {
    corners.PushBack(AddVertex(pos));
  }

  return AddFace(plArrayPtr<const plUInt32>(corners.GetData(), corners.GetCount()), uiFaceId, uiMaterialSlot);
}

void plEditableMesh::BeginShell(bool bConvex, bool bSubtractive)
{
  plBrushShell& shell = m_Shells.ExpandAndGetRef();
  shell.m_uiFirstFace = m_Faces.GetCount();
  shell.m_uiNumFaces = 0;
  shell.m_bConvex = bConvex;
  shell.m_bSubtractive = bSubtractive;
}

plVec3 plEditableMesh::ComputeFaceNormal(plUInt32 uiFaceIndex) const
{
  const plBrushFace& face = m_Faces[uiFaceIndex];

  plVec3 vNormal = plVec3::MakeZero();

  for (plUInt32 i = 0; i < face.m_Corners.GetCount(); ++i)
  {
    const plVec3& cur = m_Vertices[face.m_Corners[i]];
    const plVec3& next = m_Vertices[face.m_Corners[(i + 1) % face.m_Corners.GetCount()]];

    vNormal.x += (cur.y - next.y) * (cur.z + next.z);
    vNormal.y += (cur.z - next.z) * (cur.x + next.x);
    vNormal.z += (cur.x - next.x) * (cur.y + next.y);
  }

  vNormal.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();
  return vNormal;
}

plVec3 plEditableMesh::ComputeFaceCenter(plUInt32 uiFaceIndex) const
{
  const plBrushFace& face = m_Faces[uiFaceIndex];

  plVec3 vCenter = plVec3::MakeZero();

  for (plUInt32 uiCorner : face.m_Corners)
  {
    vCenter += m_Vertices[uiCorner];
  }

  return vCenter / static_cast<float>(face.m_Corners.GetCount());
}

void plEditableMesh::AppendMesh(const plEditableMesh& other)
{
  const plUInt32 uiVertexOffset = m_Vertices.GetCount();
  const plUInt32 uiFaceOffset = m_Faces.GetCount();

  m_Vertices.PushBackRange(other.m_Vertices.GetArrayPtr());

  for (const plBrushFace& srcFace : other.m_Faces)
  {
    plBrushFace& face = m_Faces.ExpandAndGetRef();
    face = srcFace;

    for (plUInt32 i = 0; i < face.m_Corners.GetCount(); ++i)
    {
      face.m_Corners[i] += uiVertexOffset;
    }
  }

  for (const plBrushShell& srcShell : other.m_Shells)
  {
    plBrushShell& shell = m_Shells.ExpandAndGetRef();
    shell = srcShell;
    shell.m_uiFirstFace += uiFaceOffset;
  }

  m_uiNextFaceId = plMath::Max(m_uiNextFaceId, other.m_uiNextFaceId);
}

void plEditableMesh::ExtractShell(plUInt32 uiShellIndex, plEditableMesh& out_mesh) const
{
  out_mesh.Clear();

  const plBrushShell& shell = m_Shells[uiShellIndex];

  plHashTable<plUInt32, plUInt32> remap;

  out_mesh.BeginShell(shell.m_bConvex, shell.m_bSubtractive);

  for (plUInt32 uiFace = shell.m_uiFirstFace; uiFace < shell.m_uiFirstFace + shell.m_uiNumFaces; ++uiFace)
  {
    const plBrushFace& srcFace = m_Faces[uiFace];

    plSmallArray<plUInt32, 8> corners;

    for (plUInt32 uiCorner : srcFace.m_Corners)
    {
      plUInt32 uiNew = 0;

      if (!remap.TryGetValue(uiCorner, uiNew))
      {
        uiNew = out_mesh.AddVertex(m_Vertices[uiCorner]);
        remap[uiCorner] = uiNew;
      }

      corners.PushBack(uiNew);
    }

    const plUInt32 uiNewFace = out_mesh.AddFace(plArrayPtr<const plUInt32>(corners.GetData(), corners.GetCount()), srcFace.m_uiFaceId, srcFace.m_uiMaterialSlot);
    out_mesh.m_Faces[uiNewFace].m_UV = srcFace.m_UV;
    out_mesh.m_Faces[uiNewFace].m_uiFlags = srcFace.m_uiFlags;
    out_mesh.m_Faces[uiNewFace].m_CornerUVs = srcFace.m_CornerUVs;
  }

  out_mesh.m_uiNextFaceId = m_uiNextFaceId;
}

plBoundingBox plEditableMesh::ComputeBounds() const
{
  plBoundingBox bbox = plBoundingBox::MakeInvalid();

  for (const plVec3& v : m_Vertices)
  {
    bbox.ExpandToInclude(v);
  }

  return bbox;
}

plUInt32 plEditableMesh::GetUsedMaterialSlotCount() const
{
  plUInt32 uiMaxSlot = 0;

  for (const plBrushFace& face : m_Faces)
  {
    uiMaxSlot = plMath::Max<plUInt32>(uiMaxSlot, face.m_uiMaterialSlot);
  }

  return uiMaxSlot + 1;
}

void plEditableMesh::GetWorldAlignedUVAxes(const plVec3& vNormal, plVec3& out_vUAxis, plVec3& out_vVAxis)
{
  const float fAbsX = plMath::Abs(vNormal.x);
  const float fAbsY = plMath::Abs(vNormal.y);
  const float fAbsZ = plMath::Abs(vNormal.z);

  if (fAbsZ >= fAbsX && fAbsZ >= fAbsY)
  {
    // floor / ceiling
    out_vUAxis = plVec3(1, 0, 0);
    out_vVAxis = plVec3(0, -1, 0);
  }
  else if (fAbsX >= fAbsY)
  {
    // wall facing +/- X
    out_vUAxis = plVec3(0, 1, 0);
    out_vVAxis = plVec3(0, 0, -1);
  }
  else
  {
    // wall facing +/- Y
    out_vUAxis = plVec3(1, 0, 0);
    out_vVAxis = plVec3(0, 0, -1);
  }
}

void plEditableMesh::GetFaceUVAxes(plUInt32 uiFaceIndex, const plVec3& vNormal, plVec3& out_vUAxis, plVec3& out_vVAxis) const
{
  const plBrushFace& face = m_Faces[uiFaceIndex];

  if (face.m_UV.m_bWorldAligned)
  {
    GetWorldAlignedUVAxes(vNormal, out_vUAxis, out_vVAxis);
  }
  else
  {
    out_vUAxis = face.m_UV.m_vUAxis;
    out_vVAxis = face.m_UV.m_vVAxis;
  }

  if (face.m_UV.m_Rotation != plAngle())
  {
    // rotate the projection axes around the face normal
    const plMat3 rot = plMat3::MakeAxisRotation(vNormal, face.m_UV.m_Rotation);
    out_vUAxis = rot * out_vUAxis;
    out_vVAxis = rot * out_vVAxis;
  }
}

plVec2 plEditableMesh::ComputeProjectedUV(const plBrushFaceUV& uv, const plVec3& vUAxis, const plVec3& vVAxis, const plVec3& vPos)
{
  // scale = texture repeats per world unit
  return plVec2(vPos.Dot(vUAxis) * uv.m_vScale.x + uv.m_vOffset.x, vPos.Dot(vVAxis) * uv.m_vScale.y + uv.m_vOffset.y);
}

namespace
{
  plUInt64 MakePositionKey(const plVec3& v)
  {
    // 0.1 mm cells: coincident corners of separate shells smooth together
    const plInt64 x = static_cast<plInt64>(plMath::Round(v.x * 10000.0f));
    const plInt64 y = static_cast<plInt64>(plMath::Round(v.y * 10000.0f));
    const plInt64 z = static_cast<plInt64>(plMath::Round(v.z * 10000.0f));
    const plInt64 key[3] = {x, y, z};
    return plHashingUtils::xxHash64(key, sizeof(key));
  }

  plVec3 ComputeNewellVector(const plEditableMesh& mesh, const plBrushFace& face)
  {
    plVec3 vNormal = plVec3::MakeZero();

    for (plUInt32 i = 0; i < face.m_Corners.GetCount(); ++i)
    {
      const plVec3& cur = mesh.m_Vertices[face.m_Corners[i]];
      const plVec3& next = mesh.m_Vertices[face.m_Corners[(i + 1) % face.m_Corners.GetCount()]];

      vNormal.x += (cur.y - next.y) * (cur.z + next.z);
      vNormal.y += (cur.z - next.z) * (cur.x + next.x);
      vNormal.z += (cur.x - next.x) * (cur.y + next.y);
    }

    return vNormal; // length = 2 * area
  }
} // namespace

void plEditableMesh::ConvertToExplicitUVs(const plTransform* pUVSpace)
{
  for (plUInt32 uiFace = 0; uiFace < m_Faces.GetCount(); ++uiFace)
  {
    plBrushFace& face = m_Faces[uiFace];

    if ((face.m_uiFlags & plBrushFaceFlags::ExplicitUV) != 0 && face.m_CornerUVs.GetCount() == face.m_Corners.GetCount())
      continue;

    const plVec3 vNormal = ComputeFaceNormal(uiFace);

    plVec3 vU, vV;
    GetFaceUVAxes(uiFace, vNormal, vU, vV);

    if (pUVSpace && face.m_UV.m_bWorldAligned)
    {
      const plVec3 vWorldNormal = pUVSpace->TransformDirection(vNormal).GetNormalized();
      GetWorldAlignedUVAxes(vWorldNormal, vU, vV);

      if (face.m_UV.m_Rotation != plAngle())
      {
        const plMat3 rot = plMat3::MakeAxisRotation(vWorldNormal, face.m_UV.m_Rotation);
        vU = rot * vU;
        vV = rot * vV;
      }
    }

    face.m_CornerUVs.SetCount(face.m_Corners.GetCount());

    for (plUInt32 i = 0; i < face.m_Corners.GetCount(); ++i)
    {
      const plVec3& vPos = m_Vertices[face.m_Corners[i]];
      face.m_CornerUVs[i] = ComputeProjectedUV(face.m_UV, vU, vV, pUVSpace ? pUVSpace->TransformPosition(vPos) : vPos);
    }

    face.m_uiFlags |= plBrushFaceFlags::ExplicitUV;
  }
}

void plEditableMesh::TransformVertices(const plTransform& transform)
{
  for (plVec3& v : m_Vertices)
  {
    v = transform.TransformPosition(v);
  }

  // an odd number of negative scale axes mirrors the mesh
  if (transform.m_vScale.x * transform.m_vScale.y * transform.m_vScale.z < 0.0f)
  {
    for (plBrushFace& face : m_Faces)
    {
      const plUInt32 n = face.m_Corners.GetCount();

      for (plUInt32 i = 0; i < n / 2; ++i)
      {
        plMath::Swap(face.m_Corners[i], face.m_Corners[n - 1 - i]);

        if (face.m_CornerUVs.GetCount() == n)
          plMath::Swap(face.m_CornerUVs[i], face.m_CornerUVs[n - 1 - i]);
      }
    }
  }
}

void plEditableMesh::TriangulateFace(plUInt32 uiFaceIndex, plDynamicArray<plUInt32>& out_cornerTriangles) const
{
  out_cornerTriangles.Clear();

  const plBrushFace& face = m_Faces[uiFaceIndex];
  const plUInt32 uiCount = face.m_Corners.GetCount();

  if (uiCount < 3)
    return;

  if (uiCount == 3)
  {
    out_cornerTriangles.PushBack(0);
    out_cornerTriangles.PushBack(1);
    out_cornerTriangles.PushBack(2);
    return;
  }

  // 2D coordinates in the face plane, oriented so the loop is counter-clockwise
  const plVec3 vNormal = ComputeFaceNormal(uiFaceIndex);
  plVec3 vTangent = vNormal.GetOrthogonalVector();
  vTangent.NormalizeIfNotZero(plVec3(1, 0, 0)).IgnoreResult();
  const plVec3 vBitangent = vNormal.CrossRH(vTangent);

  plHybridArray<plVec2, 16> points;
  points.SetCountUninitialized(uiCount);

  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    const plVec3& v = m_Vertices[face.m_Corners[i]];
    points[i].Set(v.Dot(vTangent), v.Dot(vBitangent));
  }

  auto cross2 = [](const plVec2& a, const plVec2& b, const plVec2& c)
  { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); };

  plHybridArray<plUInt32, 16> remaining;
  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    remaining.PushBack(i);
  }

  plUInt32 uiGuard = uiCount * uiCount;

  while (remaining.GetCount() > 3 && uiGuard-- > 0)
  {
    const plUInt32 n = remaining.GetCount();
    bool bClipped = false;

    for (plUInt32 i = 0; i < n; ++i)
    {
      const plUInt32 a = remaining[(i + n - 1) % n];
      const plUInt32 b = remaining[i];
      const plUInt32 c = remaining[(i + 1) % n];

      // reflex or degenerate corner: not an ear
      if (cross2(points[a], points[b], points[c]) <= 1e-9f)
        continue;

      bool bContainsOther = false;

      for (plUInt32 j = 0; j < n; ++j)
      {
        const plUInt32 p = remaining[j];

        if (p == a || p == b || p == c)
          continue;

        if (cross2(points[a], points[b], points[p]) >= 0.0f && cross2(points[b], points[c], points[p]) >= 0.0f && cross2(points[c], points[a], points[p]) >= 0.0f)
        {
          bContainsOther = true;
          break;
        }
      }

      if (bContainsOther)
        continue;

      out_cornerTriangles.PushBack(a);
      out_cornerTriangles.PushBack(b);
      out_cornerTriangles.PushBack(c);
      remaining.RemoveAtAndCopy(i);
      bClipped = true;
      break;
    }

    if (!bClipped)
      break;
  }

  if (remaining.GetCount() == 3)
  {
    out_cornerTriangles.PushBack(remaining[0]);
    out_cornerTriangles.PushBack(remaining[1]);
    out_cornerTriangles.PushBack(remaining[2]);
    return;
  }

  // self-intersecting or collinear loop: a fan over what is left is the least-bad output
  for (plUInt32 i = 2; i < remaining.GetCount(); ++i)
  {
    out_cornerTriangles.PushBack(remaining[0]);
    out_cornerTriangles.PushBack(remaining[i - 1]);
    out_cornerTriangles.PushBack(remaining[i]);
  }
}

float plEditableMesh::ComputeFacePlanarityError(plUInt32 uiFaceIndex) const
{
  const plBrushFace& face = m_Faces[uiFaceIndex];
  const plVec3 vNormal = ComputeFaceNormal(uiFaceIndex);
  const plVec3 vCenter = ComputeFaceCenter(uiFaceIndex);

  float fMaxError = 0.0f;

  for (plUInt32 uiCorner : face.m_Corners)
  {
    fMaxError = plMath::Max(fMaxError, plMath::Abs(vNormal.Dot(m_Vertices[uiCorner] - vCenter)));
  }

  return fMaxError;
}

float plEditableMesh::ComputeFaceArea(plUInt32 uiFaceIndex) const
{
  return ComputeNewellVector(*this, m_Faces[uiFaceIndex]).GetLength() * 0.5f;
}

plUInt32 plEditableMesh::FindShellOfFace(plUInt32 uiFaceIndex) const
{
  for (plUInt32 uiShell = 0; uiShell < m_Shells.GetCount(); ++uiShell)
  {
    const plBrushShell& shell = m_Shells[uiShell];

    if (uiFaceIndex >= shell.m_uiFirstFace && uiFaceIndex < shell.m_uiFirstFace + shell.m_uiNumFaces)
      return uiShell;
  }

  return plInvalidIndex;
}

bool plEditableMesh::IsStructurallyValid() const
{
  const plUInt32 uiNumVertices = m_Vertices.GetCount();

  for (const plBrushFace& face : m_Faces)
  {
    if (face.m_Corners.GetCount() < 3)
      return false;

    for (plUInt32 uiCorner : face.m_Corners)
    {
      if (uiCorner >= uiNumVertices)
        return false;
    }

    if ((face.m_uiFlags & plBrushFaceFlags::ExplicitUV) != 0 && face.m_CornerUVs.GetCount() != face.m_Corners.GetCount())
      return false;
  }

  plUInt32 uiNextFace = 0;

  for (const plBrushShell& shell : m_Shells)
  {
    if (shell.m_uiFirstFace != uiNextFace || shell.m_uiFirstFace + shell.m_uiNumFaces > m_Faces.GetCount())
      return false;

    uiNextFace += shell.m_uiNumFaces;
  }

  if (!m_Shells.IsEmpty() && uiNextFace != m_Faces.GetCount())
    return false;

  for (const plVec3& v : m_Vertices)
  {
    if (!v.IsValid())
      return false;
  }

  return true;
}

void plEditableMesh::BuildRenderGeometry(plGeometry& out_geom, plDynamicArray<plBrushSubMeshRange>& out_ranges, plAngle smoothingAngle, const plTransform* pUVSpace) const
{
  out_ranges.Clear();

  const plUInt32 uiNumFaces = m_Faces.GetCount();

  // per-face normals, plus area-weighted ones for smoothing
  plDynamicArray<plVec3> faceNormals, faceWeighted;
  faceNormals.SetCountUninitialized(uiNumFaces);
  faceWeighted.SetCountUninitialized(uiNumFaces);

  for (plUInt32 uiFace = 0; uiFace < uiNumFaces; ++uiFace)
  {
    faceWeighted[uiFace] = ComputeNewellVector(*this, m_Faces[uiFace]);
    faceNormals[uiFace] = faceWeighted[uiFace];
    faceNormals[uiFace].NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();
  }

  // position -> faces touching it, only needed when smoothing is on
  const bool bSmooth = smoothingAngle.GetRadian() > 0.0001f;
  const float fCosThreshold = plMath::Cos(smoothingAngle);
  plHashTable<plUInt64, plHybridArray<plUInt32, 6>> facesAtPosition;

  if (bSmooth)
  {
    for (plUInt32 uiFace = 0; uiFace < uiNumFaces; ++uiFace)
    {
      if ((m_Faces[uiFace].m_uiFlags & (plBrushFaceFlags::HardEdges | plBrushFaceFlags::Internal)) != 0)
        continue;

      for (plUInt32 uiCorner : m_Faces[uiFace].m_Corners)
      {
        auto& list = facesAtPosition[MakePositionKey(m_Vertices[uiCorner])];

        if (!list.Contains(uiFace))
          list.PushBack(uiFace);
      }
    }
  }

  const plUInt32 uiNumSlots = GetUsedMaterialSlotCount();
  plUInt32 uiTriangle = 0;

  plDynamicArray<plUInt32> cornerTriangles;
  plHybridArray<plUInt32, 16> geomIndices;

  for (plUInt32 uiSlot = 0; uiSlot < uiNumSlots; ++uiSlot)
  {
    const plUInt32 uiFirstTriangle = uiTriangle;

    for (plUInt32 uiFace = 0; uiFace < uiNumFaces; ++uiFace)
    {
      const plBrushFace& face = m_Faces[uiFace];

      if (face.m_uiMaterialSlot != uiSlot || (face.m_uiFlags & plBrushFaceFlags::Internal) != 0)
        continue;

      const plVec3 vNormal = faceNormals[uiFace];
      const bool bFaceSmooth = bSmooth && (face.m_uiFlags & plBrushFaceFlags::HardEdges) == 0;
      const bool bExplicitUV = (face.m_uiFlags & plBrushFaceFlags::ExplicitUV) != 0 && face.m_CornerUVs.GetCount() == face.m_Corners.GetCount();

      plVec3 vUAxis, vVAxis;
      GetFaceUVAxes(uiFace, vNormal, vUAxis, vVAxis);

      geomIndices.Clear();

      for (plUInt32 i = 0; i < face.m_Corners.GetCount(); ++i)
      {
        const plVec3& vPos = m_Vertices[face.m_Corners[i]];

        plVec3 vCornerNormal = vNormal;

        if (bFaceSmooth)
        {
          if (const auto* pFaces = facesAtPosition.GetValue(MakePositionKey(vPos)))
          {
            plVec3 vSum = plVec3::MakeZero();

            for (plUInt32 uiOther : *pFaces)
            {
              if (faceNormals[uiOther].Dot(vNormal) >= fCosThreshold)
              {
                vSum += faceWeighted[uiOther];
              }
            }

            if (vSum.NormalizeIfNotZero(vNormal).Succeeded())
            {
              vCornerNormal = vSum;
            }
          }
        }

        plVec2 vTexCoord;

        if (bExplicitUV)
        {
          vTexCoord = face.m_CornerUVs[i];
        }
        else
        {
          const plVec3 vUVPos = pUVSpace ? pUVSpace->TransformPosition(vPos) : vPos;
          plVec3 vU = vUAxis, vV = vVAxis;

          if (pUVSpace && face.m_UV.m_bWorldAligned)
          {
            // world-space tiling: pick the projection from the world normal
            GetWorldAlignedUVAxes(pUVSpace->TransformDirection(vNormal).GetNormalized(), vU, vV);

            if (face.m_UV.m_Rotation != plAngle())
            {
              const plMat3 rot = plMat3::MakeAxisRotation(pUVSpace->TransformDirection(vNormal).GetNormalized(), face.m_UV.m_Rotation);
              vU = rot * vU;
              vV = rot * vV;
            }
          }

          vTexCoord = ComputeProjectedUV(face.m_UV, vU, vV, vUVPos);
        }

        geomIndices.PushBack(out_geom.AddVertex(vPos, vCornerNormal, vTexCoord, plColor::White));
      }

      TriangulateFace(uiFace, cornerTriangles);

      for (plUInt32 t = 0; t + 2 < cornerTriangles.GetCount(); t += 3)
      {
        plUInt32 tri[3] = {geomIndices[cornerTriangles[t]], geomIndices[cornerTriangles[t + 1]], geomIndices[cornerTriangles[t + 2]]};
        out_geom.AddPolygon(plArrayPtr<plUInt32>(tri, 3), false);
        ++uiTriangle;
      }
    }

    if (uiTriangle > uiFirstTriangle)
    {
      plBrushSubMeshRange& range = out_ranges.ExpandAndGetRef();
      range.m_uiFirstTriangle = uiFirstTriangle;
      range.m_uiNumTriangles = uiTriangle - uiFirstTriangle;
      range.m_uiMaterialSlot = static_cast<plUInt8>(uiSlot);
    }
  }
}

// Versioned, append-only. The blob travels through the document mirror and undo, so old data must
// always load. v2: 16-bit corner counts, face flags, explicit per-corner UVs.
constexpr plUInt8 c_uiMeshFormatVersion = 2;

// sanity limits for untrusted blobs — far above anything a brush reaches
constexpr plUInt32 c_uiMaxVertices = 4 * 1024 * 1024;
constexpr plUInt32 c_uiMaxFaces = 4 * 1024 * 1024;

void plEditableMesh::Serialize(plStreamWriter& inout_stream) const
{
  inout_stream << c_uiMeshFormatVersion;

  inout_stream << m_Vertices.GetCount();
  for (const plVec3& v : m_Vertices)
  {
    inout_stream << v;
  }

  inout_stream << m_Faces.GetCount();
  for (const plBrushFace& face : m_Faces)
  {
    const bool bExplicitUV = (face.m_uiFlags & plBrushFaceFlags::ExplicitUV) != 0 && face.m_CornerUVs.GetCount() == face.m_Corners.GetCount();

    inout_stream << face.m_uiFaceId;
    inout_stream << face.m_uiMaterialSlot;
    inout_stream << static_cast<plUInt8>(bExplicitUV ? face.m_uiFlags : (face.m_uiFlags & ~plBrushFaceFlags::ExplicitUV));
    inout_stream << static_cast<plUInt16>(face.m_Corners.GetCount());

    for (plUInt32 uiCorner : face.m_Corners)
    {
      inout_stream << uiCorner;
    }

    inout_stream << face.m_UV.m_vUAxis;
    inout_stream << face.m_UV.m_vVAxis;
    inout_stream << face.m_UV.m_vOffset;
    inout_stream << face.m_UV.m_vScale;
    inout_stream << face.m_UV.m_Rotation;
    inout_stream << face.m_UV.m_bWorldAligned;

    if (bExplicitUV)
    {
      for (const plVec2& uv : face.m_CornerUVs)
      {
        inout_stream << uv;
      }
    }
  }

  inout_stream << m_Shells.GetCount();
  for (const plBrushShell& shell : m_Shells)
  {
    inout_stream << shell.m_uiFirstFace;
    inout_stream << shell.m_uiNumFaces;
    inout_stream << shell.m_bConvex;
    inout_stream << shell.m_bSubtractive;
  }

  inout_stream << m_uiNextFaceId;
}

plResult plEditableMesh::Deserialize(plStreamReader& inout_stream)
{
  Clear();

  plUInt8 uiVersion = 0;
  if (inout_stream.ReadBytes(&uiVersion, 1) != 1 || uiVersion == 0 || uiVersion > c_uiMeshFormatVersion)
    return PL_FAILURE;

  // every read is checked: a truncated blob must fail, never leave garbage behind
  bool bOk = true;
  auto read = [&](auto& value)
  {
    if (bOk && inout_stream.ReadBytes(&value, sizeof(value)) != sizeof(value))
    {
      bOk = false;
    }
  };

  auto readBool = [&](bool& ref_bValue)
  {
    plUInt8 uiValue = 0;
    read(uiValue);
    ref_bValue = uiValue != 0;
  };

  auto fail = [this]()
  {
    Clear();
    return PL_FAILURE;
  };

  plUInt32 uiCount = 0;

  read(uiCount);
  if (!bOk || uiCount > c_uiMaxVertices)
    return fail();

  m_Vertices.SetCountUninitialized(uiCount);
  for (plVec3& v : m_Vertices)
  {
    read(v);
  }

  read(uiCount);
  if (!bOk || uiCount > c_uiMaxFaces)
    return fail();

  m_Faces.SetCount(uiCount);
  for (plBrushFace& face : m_Faces)
  {
    read(face.m_uiFaceId);
    read(face.m_uiMaterialSlot);

    plUInt32 uiCorners = 0;

    if (uiVersion >= 2)
    {
      read(face.m_uiFlags);

      plUInt16 uiCorners16 = 0;
      read(uiCorners16);
      uiCorners = uiCorners16;
    }
    else
    {
      plUInt8 uiCorners8 = 0;
      read(uiCorners8);
      uiCorners = uiCorners8;
    }

    if (!bOk || uiCorners < 3)
      return fail();

    face.m_Corners.SetCount(static_cast<plUInt16>(uiCorners));

    for (plUInt32 i = 0; i < uiCorners; ++i)
    {
      read(face.m_Corners[i]);
    }

    read(face.m_UV.m_vUAxis);
    read(face.m_UV.m_vVAxis);
    read(face.m_UV.m_vOffset);
    read(face.m_UV.m_vScale);

    float fRotationRad = 0.0f;
    read(fRotationRad);
    face.m_UV.m_Rotation = plAngle::MakeFromRadian(fRotationRad);

    readBool(face.m_UV.m_bWorldAligned);

    if ((face.m_uiFlags & plBrushFaceFlags::ExplicitUV) != 0)
    {
      face.m_CornerUVs.SetCount(static_cast<plUInt16>(uiCorners));

      for (plVec2& uv : face.m_CornerUVs)
      {
        read(uv);
      }
    }

    if (!bOk)
      return fail();
  }

  read(uiCount);
  if (!bOk || uiCount > c_uiMaxFaces)
    return fail();

  m_Shells.SetCount(uiCount);
  for (plBrushShell& shell : m_Shells)
  {
    read(shell.m_uiFirstFace);
    read(shell.m_uiNumFaces);
    readBool(shell.m_bConvex);
    readBool(shell.m_bSubtractive);
  }

  read(m_uiNextFaceId);

  if (!bOk || !IsStructurallyValid())
    return fail();

  return PL_SUCCESS;
}

void plEditableMesh::ToDataBuffer(plDataBuffer& out_buffer) const
{
  // written straight into the buffer, reserved up front: editor drags write large meshes per mouse move
  out_buffer.Clear();
  out_buffer.Reserve(m_Vertices.GetCount() * sizeof(plVec3) + m_Faces.GetCount() * 72 + m_Shells.GetCount() * 10 + 64);

  plMemoryStreamContainerWrapperStorage<plDataBuffer> storage(&out_buffer);
  plMemoryStreamWriter writer(&storage);

  Serialize(writer);
}

plResult plEditableMesh::FromDataBuffer(const plDataBuffer& buffer)
{
  if (buffer.IsEmpty())
  {
    Clear();
    return PL_FAILURE;
  }

  plRawMemoryStreamReader reader(buffer.GetData(), buffer.GetCount());
  return Deserialize(reader);
}


PL_STATICLINK_FILE(ModelingPlugin, ModelingPlugin_Modeling_Implementation_EditableMesh);
