#include <ModelingPlugin/ModelingPluginPCH.h>

#include <Foundation/Algorithm/HashingUtils.h>
#include <ModelingPlugin/Modeling/MeshOps.h>
#include <ModelingPlugin/Modeling/MeshTopology.h>

namespace
{
  /// Faces edited in place: a parallel shell index per face, applied with RebuildShells at the end.
  struct FaceEditor
  {
    plEditableMesh& m_Mesh;
    plDynamicArray<plUInt32> m_FaceShells;

    explicit FaceEditor(plEditableMesh& ref_mesh)
      : m_Mesh(ref_mesh)
    {
      plMeshOps::GetFaceShells(m_Mesh, m_FaceShells);
    }

    /// A new face carrying the material, UV mapping and flags of pSource (copied before the append).
    plUInt32 AddFace(plArrayPtr<const plUInt32> corners, const plBrushFace& source, plUInt32 uiShell, plUInt32 uiFaceId)
    {
      const plBrushFace sourceCopy = source;

      plBrushFace& face = m_Mesh.m_Faces.ExpandAndGetRef();
      face.m_uiFaceId = uiFaceId;
      face.m_uiMaterialSlot = sourceCopy.m_uiMaterialSlot;
      face.m_uiFlags = sourceCopy.m_uiFlags & ~(plBrushFaceFlags::ExplicitUV | plBrushFaceFlags::Internal);
      face.m_UV = sourceCopy.m_UV;
      face.m_Corners.SetCount(static_cast<plUInt16>(corners.GetCount()));

      for (plUInt32 i = 0; i < corners.GetCount(); ++i)
        face.m_Corners[i] = corners[i];

      m_FaceShells.PushBack(uiShell);
      return m_Mesh.m_Faces.GetCount() - 1;
    }

    plUInt32 NewFaceId() { return m_Mesh.m_uiNextFaceId++; }

    void RemoveFaces(const plHashSet<plUInt32>& faceIndices)
    {
      plDynamicArray<plBrushFace> faces;
      plDynamicArray<plUInt32> shells;

      for (plUInt32 i = 0; i < m_Mesh.m_Faces.GetCount(); ++i)
      {
        if (faceIndices.Contains(i))
          continue;

        faces.PushBack(std::move(m_Mesh.m_Faces[i]));
        shells.PushBack(m_FaceShells[i]);
      }

      m_Mesh.m_Faces.Swap(faces);
      m_FaceShells.Swap(shells);
    }

    void Finish()
    {
      plMeshOps::RebuildShells(m_Mesh, m_FaceShells);
      plMeshOps::RemoveUnusedVertices(m_Mesh);

      for (plBrushShell& shell : m_Mesh.m_Shells)
      {
        shell.m_bConvex = false; // informational; CSG re-checks convexity itself
      }
    }
  };

  plVec3 FaceNormal(const plEditableMesh& mesh, plUInt32 uiFace)
  {
    return mesh.ComputeFaceNormal(uiFace);
  }

  plVec3 SafeNormalize(const plVec3& v, const plVec3& vFallback)
  {
    plVec3 r = v;
    r.NormalizeIfNotZero(vFallback).IgnoreResult();
    return r;
  }

  void ReplaceCorner(plBrushFace& ref_face, plUInt32 uiOld, plArrayPtr<const plUInt32> replacement)
  {
    for (plUInt32 i = 0; i < ref_face.m_Corners.GetCount(); ++i)
    {
      if (ref_face.m_Corners[i] != uiOld)
        continue;

      ref_face.m_Corners.RemoveAtAndCopy(static_cast<plUInt16>(i));

      for (plUInt32 r = 0; r < replacement.GetCount(); ++r)
      {
        ref_face.m_Corners.InsertAt(static_cast<plUInt16>(i + r), replacement[r]);
      }

      return;
    }
  }

  /// Inserts the points between a and b (in a -> b order) into every face that has the edge a->b or b->a.
  void InsertOnEdge(plEditableMesh& ref_mesh, plUInt32 a, plUInt32 b, plArrayPtr<const plUInt32> pointsAtoB, const plHashSet<plUInt32>* pSkipFaces)
  {
    for (plUInt32 uiFace = 0; uiFace < ref_mesh.m_Faces.GetCount(); ++uiFace)
    {
      if (pSkipFaces && pSkipFaces->Contains(uiFace))
        continue;

      plBrushFace& face = ref_mesh.m_Faces[uiFace];
      const plUInt32 n = face.m_Corners.GetCount();

      for (plUInt32 i = 0; i < n; ++i)
      {
        const plUInt32 cur = face.m_Corners[i];
        const plUInt32 next = face.m_Corners[(i + 1) % n];

        if (cur == a && next == b)
        {
          for (plUInt32 p = 0; p < pointsAtoB.GetCount(); ++p)
            face.m_Corners.InsertAt(static_cast<plUInt16>(i + 1 + p), pointsAtoB[p]);
          break;
        }

        if (cur == b && next == a)
        {
          for (plUInt32 p = 0; p < pointsAtoB.GetCount(); ++p)
            face.m_Corners.InsertAt(static_cast<plUInt16>(i + 1 + p), pointsAtoB[pointsAtoB.GetCount() - 1 - p]);
          break;
        }
      }

      // explicit UVs no longer line up with the corners
      if (face.m_Corners.GetCount() != n)
      {
        face.m_uiFlags &= ~plBrushFaceFlags::ExplicitUV;
        face.m_CornerUVs.Clear();
      }
    }
  }

  plStatus NothingSelected(const char* szWhat)
  {
    return plStatus(plFmt("{}: nothing selected.", szWhat));
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// bookkeeping

void plMeshOps::GetFaceShells(const plEditableMesh& mesh, plDynamicArray<plUInt32>& out_faceShells)
{
  out_faceShells.Clear();
  out_faceShells.SetCount(mesh.m_Faces.GetCount(), 0);

  for (plUInt32 uiShell = 0; uiShell < mesh.m_Shells.GetCount(); ++uiShell)
  {
    const plBrushShell& shell = mesh.m_Shells[uiShell];

    for (plUInt32 f = shell.m_uiFirstFace; f < shell.m_uiFirstFace + shell.m_uiNumFaces && f < mesh.m_Faces.GetCount(); ++f)
    {
      out_faceShells[f] = uiShell;
    }
  }
}

void plMeshOps::RebuildShells(plEditableMesh& inout_mesh, plArrayPtr<const plUInt32> faceShells)
{
  const plUInt32 uiOldShells = plMath::Max<plUInt32>(1, inout_mesh.m_Shells.GetCount());

  plUInt32 uiMaxShell = 0;
  for (plUInt32 s : faceShells)
    uiMaxShell = plMath::Max(uiMaxShell, s);

  const plUInt32 uiShellCount = plMath::Max(uiOldShells, uiMaxShell + 1);

  plDynamicArray<plBrushFace> faces;
  faces.Reserve(inout_mesh.m_Faces.GetCount());

  plDynamicArray<plBrushShell> shells;

  for (plUInt32 uiShell = 0; uiShell < uiShellCount; ++uiShell)
  {
    const plUInt32 uiFirst = faces.GetCount();

    for (plUInt32 f = 0; f < inout_mesh.m_Faces.GetCount(); ++f)
    {
      if (faceShells[f] == uiShell)
        faces.PushBack(inout_mesh.m_Faces[f]);
    }

    if (faces.GetCount() == uiFirst)
      continue;

    plBrushShell& shell = shells.ExpandAndGetRef();
    shell.m_uiFirstFace = uiFirst;
    shell.m_uiNumFaces = faces.GetCount() - uiFirst;

    if (uiShell < inout_mesh.m_Shells.GetCount())
    {
      shell.m_bConvex = inout_mesh.m_Shells[uiShell].m_bConvex;
      shell.m_bSubtractive = inout_mesh.m_Shells[uiShell].m_bSubtractive;
    }
  }

  inout_mesh.m_Faces.Swap(faces);
  inout_mesh.m_Shells.Swap(shells);
}

void plMeshOps::RemoveUnusedVertices(plEditableMesh& inout_mesh)
{
  plDynamicArray<plUInt32> remap;
  remap.SetCount(inout_mesh.m_Vertices.GetCount(), plInvalidIndex);

  plDynamicArray<plVec3> vertices;

  for (plBrushFace& face : inout_mesh.m_Faces)
  {
    for (plUInt32& uiCorner : face.m_Corners)
    {
      if (remap[uiCorner] == plInvalidIndex)
      {
        remap[uiCorner] = vertices.GetCount();
        vertices.PushBack(inout_mesh.m_Vertices[uiCorner]);
      }

      uiCorner = remap[uiCorner];
    }
  }

  inout_mesh.m_Vertices.Swap(vertices);
}

void plMeshOps::WeldByDistance(plEditableMesh& inout_mesh, float fDistance)
{
  const float fInvCell = 1.0f / plMath::Max(fDistance, 0.000001f);

  plHashTable<plUInt64, plUInt32> cells;
  plDynamicArray<plUInt32> remap;
  remap.SetCountUninitialized(inout_mesh.m_Vertices.GetCount());

  for (plUInt32 v = 0; v < inout_mesh.m_Vertices.GetCount(); ++v)
  {
    const plVec3& p = inout_mesh.m_Vertices[v];
    const plInt64 key[3] = {static_cast<plInt64>(plMath::Round(p.x * fInvCell)), static_cast<plInt64>(plMath::Round(p.y * fInvCell)), static_cast<plInt64>(plMath::Round(p.z * fInvCell))};
    const plUInt64 uiKey = plHashingUtils::xxHash64(key, sizeof(key));

    plUInt32 uiExisting = 0;
    if (cells.TryGetValue(uiKey, uiExisting))
    {
      remap[v] = uiExisting;
    }
    else
    {
      cells.Insert(uiKey, v);
      remap[v] = v;
    }
  }

  plDynamicArray<plUInt32> faceShells;
  GetFaceShells(inout_mesh, faceShells);

  plDynamicArray<plBrushFace> faces;
  plDynamicArray<plUInt32> shells;

  for (plUInt32 f = 0; f < inout_mesh.m_Faces.GetCount(); ++f)
  {
    plBrushFace face = inout_mesh.m_Faces[f];
    const bool bExplicit = (face.m_uiFlags & plBrushFaceFlags::ExplicitUV) != 0 && face.m_CornerUVs.GetCount() == face.m_Corners.GetCount();

    plSmallArray<plUInt32, 8> corners;
    plSmallArray<plVec2, 8> uvs;

    for (plUInt32 i = 0; i < face.m_Corners.GetCount(); ++i)
    {
      const plUInt32 c = remap[face.m_Corners[i]];

      if (!corners.IsEmpty() && corners.PeekBack() == c)
        continue;

      corners.PushBack(c);
      if (bExplicit)
        uvs.PushBack(face.m_CornerUVs[i]);
    }

    while (corners.GetCount() > 1 && corners[0] == corners.PeekBack())
    {
      corners.PopBack();
      if (bExplicit)
        uvs.PopBack();
    }

    if (corners.GetCount() < 3)
      continue;

    face.m_Corners = corners;
    face.m_CornerUVs = uvs;

    if (!bExplicit)
      face.m_uiFlags &= ~plBrushFaceFlags::ExplicitUV;

    faces.PushBack(face);
    shells.PushBack(faceShells[f]);
  }

  inout_mesh.m_Faces.Swap(faces);
  RebuildShells(inout_mesh, shells);
  RemoveUnusedVertices(inout_mesh);
}

void plMeshOps::GatherFaceIndices(const plEditableMesh& mesh, const plHashSet<plUInt32>& faceIds, plDynamicArray<plUInt32>& out_faceIndices)
{
  out_faceIndices.Clear();

  for (plUInt32 f = 0; f < mesh.m_Faces.GetCount(); ++f)
  {
    if (faceIds.Contains(mesh.m_Faces[f].m_uiFaceId))
      out_faceIndices.PushBack(f);
  }
}

namespace
{
  plUInt32 FindRoot(plDynamicArray<plUInt32>& ref_parents, plUInt32 i)
  {
    while (ref_parents[i] != i)
    {
      ref_parents[i] = ref_parents[ref_parents[i]];
      i = ref_parents[i];
    }

    return i;
  }

  void Unite(plDynamicArray<plUInt32>& ref_parents, plUInt32 a, plUInt32 b)
  {
    a = FindRoot(ref_parents, a);
    b = FindRoot(ref_parents, b);

    if (a != b)
      ref_parents[plMath::Max(a, b)] = plMath::Min(a, b);
  }

  plUInt64 MakeCornerKey(const plVec3& v)
  {
    // 0.1 mm cells, as for smoothing: corners of separate CSG shells coincide exactly or not at all
    const plInt64 key[3] = {static_cast<plInt64>(plMath::Round(v.x * 10000.0f)), static_cast<plInt64>(plMath::Round(v.y * 10000.0f)), static_cast<plInt64>(plMath::Round(v.z * 10000.0f))};
    return plHashingUtils::xxHash64(key, sizeof(key));
  }

  /// Whether a corner of face a lies on an edge of face b: a T-junction, where CSG pieces meet without
  /// sharing a corner.
  bool HasCornerOnEdge(const plEditableMesh& mesh, const plBrushFace& a, const plBrushFace& b)
  {
    constexpr float fTolerance = 0.001f;

    for (plUInt32 uiCorner : a.m_Corners)
    {
      const plVec3& p = mesh.m_Vertices[uiCorner];

      for (plUInt32 i = 0; i < b.m_Corners.GetCount(); ++i)
      {
        const plVec3& e0 = mesh.m_Vertices[b.m_Corners[i]];
        const plVec3 vEdge = mesh.m_Vertices[b.m_Corners[(i + 1) % b.m_Corners.GetCount()]] - e0;
        const float fLenSq = vEdge.GetLengthSquared();

        if (fLenSq <= 0.0f)
          continue;

        const float t = plMath::Clamp((p - e0).Dot(vEdge) / fLenSq, 0.0f, 1.0f);

        if ((e0 + vEdge * t - p).GetLengthSquared() < fTolerance * fTolerance)
          return true;
      }
    }

    return false;
  }
} // namespace

plUInt32 plMeshOps::ComputeSurfaces(const plEditableMesh& mesh, plDynamicArray<plUInt32>& out_surfaceOfFace)
{
  const plUInt32 uiFaceCount = mesh.m_Faces.GetCount();

  plDynamicArray<plUInt32> parents;
  parents.SetCountUninitialized(uiFaceCount);
  for (plUInt32 f = 0; f < uiFaceCount; ++f)
    parents[f] = f;

  // candidates for one surface: same plane (1/512 normal steps, 1 mm offsets) and same material slot
  plHashTable<plUInt64, plDynamicArray<plUInt32>> planes;

  for (plUInt32 f = 0; f < uiFaceCount; ++f)
  {
    const plBrushFace& face = mesh.m_Faces[f];

    if ((face.m_uiFlags & plBrushFaceFlags::Internal) != 0)
      continue;

    const plVec3 vNormal = mesh.ComputeFaceNormal(f);
    const float fOffset = vNormal.Dot(mesh.ComputeFaceCenter(f));

    const plInt32 key[5] = {static_cast<plInt32>(plMath::Round(vNormal.x * 512.0f)), static_cast<plInt32>(plMath::Round(vNormal.y * 512.0f)),
      static_cast<plInt32>(plMath::Round(vNormal.z * 512.0f)), static_cast<plInt32>(plMath::Round(fOffset * 1000.0f)), face.m_uiMaterialSlot};
    planes[plHashingUtils::xxHash64(key, sizeof(key))].PushBack(f);
  }

  plHashTable<plUInt64, plUInt32> faceAtCorner;

  for (auto it : planes)
  {
    const plDynamicArray<plUInt32>& faces = it.Value();

    // shared corners connect most pieces
    faceAtCorner.Clear();

    for (plUInt32 f : faces)
    {
      for (plUInt32 uiCorner : mesh.m_Faces[f].m_Corners)
      {
        plUInt32 uiOther = 0;
        const plUInt64 uiKey = MakeCornerKey(mesh.m_Vertices[uiCorner]);

        if (faceAtCorner.TryGetValue(uiKey, uiOther))
          Unite(parents, f, uiOther);
        else
          faceAtCorner.Insert(uiKey, f);
      }
    }

    // T-junctions: pairwise, so only for planes with few faces (a dense imported floor shares corners anyway)
    if (faces.GetCount() > 256)
      continue;

    for (plUInt32 i = 0; i < faces.GetCount(); ++i)
    {
      for (plUInt32 j = i + 1; j < faces.GetCount(); ++j)
      {
        if (FindRoot(parents, faces[i]) == FindRoot(parents, faces[j]))
          continue;

        const plBrushFace& a = mesh.m_Faces[faces[i]];
        const plBrushFace& b = mesh.m_Faces[faces[j]];

        if (HasCornerOnEdge(mesh, a, b) || HasCornerOnEdge(mesh, b, a))
          Unite(parents, faces[i], faces[j]);
      }
    }
  }

  // a face id never spans two surfaces
  plHashTable<plUInt32, plUInt32> faceWithId;

  for (plUInt32 f = 0; f < uiFaceCount; ++f)
  {
    if ((mesh.m_Faces[f].m_uiFlags & plBrushFaceFlags::Internal) != 0)
      continue;

    plUInt32 uiOther = 0;

    if (faceWithId.TryGetValue(mesh.m_Faces[f].m_uiFaceId, uiOther))
      Unite(parents, f, uiOther);
    else
      faceWithId.Insert(mesh.m_Faces[f].m_uiFaceId, f);
  }

  // compact the roots into surface indices, in face order
  out_surfaceOfFace.SetCount(uiFaceCount);

  plHashTable<plUInt32, plUInt32> surfaceOfRoot;
  plUInt32 uiSurfaceCount = 0;

  for (plUInt32 f = 0; f < uiFaceCount; ++f)
  {
    if ((mesh.m_Faces[f].m_uiFlags & plBrushFaceFlags::Internal) != 0)
    {
      out_surfaceOfFace[f] = plInvalidIndex;
      continue;
    }

    const plUInt32 uiRoot = FindRoot(parents, f);
    plUInt32 uiSurface = 0;

    if (!surfaceOfRoot.TryGetValue(uiRoot, uiSurface))
    {
      uiSurface = uiSurfaceCount++;
      surfaceOfRoot.Insert(uiRoot, uiSurface);
    }

    out_surfaceOfFace[f] = uiSurface;
  }

  return uiSurfaceCount;
}

//////////////////////////////////////////////////////////////////////////
// face operations

plStatus plMeshOps::Extrude(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& faceIds, float fDistance, bool bRegion)
{
  plDynamicArray<plUInt32> selected;
  GatherFaceIndices(inout_mesh, faceIds, selected);

  if (selected.IsEmpty())
    return NothingSelected("Extrude");

  plMeshTopology topology;
  topology.Build(inout_mesh);

  plHashSet<plUInt32> selectedSet;
  for (plUInt32 f : selected)
    selectedSet.Insert(f);

  FaceEditor editor(inout_mesh);
  plEditableMesh& mesh = inout_mesh;

  // regions: all selected faces together, or each face alone
  plHybridArray<plHybridArray<plUInt32, 8>, 8> regions;

  if (bRegion)
  {
    regions.ExpandAndGetRef().PushBackRange(selected.GetArrayPtr());
  }
  else
  {
    for (plUInt32 f : selected)
      regions.ExpandAndGetRef().PushBack(f);
  }

  for (const auto& region : regions)
  {
    plHashSet<plUInt32> regionSet;
    for (plUInt32 f : region)
      regionSet.Insert(f);

    // duplicate every vertex the region uses, and gather its move direction
    plHashTable<plUInt32, plUInt32> duplicates;
    plHashTable<plUInt32, plVec3> directions;

    for (plUInt32 f : region)
    {
      const plVec3 vNormal = FaceNormal(mesh, f);

      for (plUInt32 uiCorner : mesh.m_Faces[f].m_Corners)
      {
        if (!duplicates.Contains(uiCorner))
        {
          duplicates.Insert(uiCorner, mesh.AddVertex(mesh.m_Vertices[uiCorner]));
          directions.Insert(uiCorner, plVec3::MakeZero());
        }

        *directions.GetValue(uiCorner) += vNormal;
      }
    }

    // walls along the region outline: edges whose other face is outside the region (or missing)
    for (plUInt32 f : region)
    {
      const plBrushFace sourceFace = mesh.m_Faces[f];
      const plUInt32 n = sourceFace.m_Corners.GetCount();

      for (plUInt32 i = 0; i < n; ++i)
      {
        const plUInt32 a = sourceFace.m_Corners[i];
        const plUInt32 b = sourceFace.m_Corners[(i + 1) % n];

        const plUInt32 uiTwin = topology.GetHalfEdge(topology.GetFaceHalfEdge(f, i)).m_uiTwin;
        const bool bInner = uiTwin != plMeshTopology::c_uiInvalid && regionSet.Contains(topology.GetHalfEdge(uiTwin).m_uiFace);

        if (bInner)
          continue;

        const plUInt32 wall[4] = {a, b, *duplicates.GetValue(b), *duplicates.GetValue(a)};
        editor.AddFace(plArrayPtr<const plUInt32>(wall), sourceFace, editor.m_FaceShells[f], editor.NewFaceId());
      }
    }

    // the region's faces move onto the duplicates
    for (plUInt32 f : region)
    {
      for (plUInt32& uiCorner : mesh.m_Faces[f].m_Corners)
      {
        uiCorner = *duplicates.GetValue(uiCorner);
      }
    }

    // offset: along the averaged normal, lengthened so every face plane moves by fDistance. Normals are
    // taken before anything moves.
    plHybridArray<plVec3, 8> regionNormals;
    for (plUInt32 f : region)
      regionNormals.PushBack(FaceNormal(mesh, f));

    plHybridArray<plVec3, 16> offsets;
    plHybridArray<plUInt32, 16> moved;

    for (auto it : duplicates)
    {
      const plVec3 vSum = *directions.GetValue(it.Key());
      const plVec3 vDir = SafeNormalize(vSum, plVec3(0, 0, 1));

      float fMinDot = 1.0f;
      for (plUInt32 r = 0; r < region.GetCount(); ++r)
      {
        if (mesh.m_Faces[region[r]].m_Corners.Contains(it.Value()))
          fMinDot = plMath::Min(fMinDot, vDir.Dot(regionNormals[r]));
      }

      moved.PushBack(it.Value());
      offsets.PushBack(vDir * (fDistance / plMath::Max(fMinDot, 0.25f)));
    }

    for (plUInt32 i = 0; i < moved.GetCount(); ++i)
    {
      mesh.m_Vertices[moved[i]] += offsets[i];
    }
  }

  editor.Finish();
  return plStatus(PL_SUCCESS);
}

plStatus plMeshOps::Inset(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& faceIds, float fAmount)
{
  plDynamicArray<plUInt32> selected;
  GatherFaceIndices(inout_mesh, faceIds, selected);

  if (selected.IsEmpty())
    return NothingSelected("Inset");

  if (fAmount <= 0.0f)
    return plStatus("Inset: the amount must be greater than zero.");

  FaceEditor editor(inout_mesh);
  plEditableMesh& mesh = inout_mesh;

  for (plUInt32 f : selected)
  {
    const plVec3 vNormal = FaceNormal(mesh, f);
    const plBrushFace sourceFace = mesh.m_Faces[f];
    const plUInt32 n = sourceFace.m_Corners.GetCount();

    plHybridArray<plUInt32, 8> inner;

    for (plUInt32 i = 0; i < n; ++i)
    {
      const plVec3& prev = mesh.m_Vertices[sourceFace.m_Corners[(i + n - 1) % n]];
      const plVec3& cur = mesh.m_Vertices[sourceFace.m_Corners[i]];
      const plVec3& next = mesh.m_Vertices[sourceFace.m_Corners[(i + 1) % n]];

      // inward perpendiculars of both edges, in the face plane
      const plVec3 vInPrev = SafeNormalize(vNormal.CrossRH(cur - prev), plVec3::MakeZero());
      const plVec3 vInNext = SafeNormalize(vNormal.CrossRH(next - cur), plVec3::MakeZero());
      const plVec3 vBisector = SafeNormalize(vInPrev + vInNext, vInNext);

      const float fScale = 1.0f / plMath::Max(vBisector.Dot(vInNext), 0.2f);
      inner.PushBack(mesh.AddVertex(cur + vBisector * fAmount * fScale));
    }

    for (plUInt32 i = 0; i < n; ++i)
    {
      const plUInt32 ring[4] = {sourceFace.m_Corners[i], sourceFace.m_Corners[(i + 1) % n], inner[(i + 1) % n], inner[i]};
      editor.AddFace(plArrayPtr<const plUInt32>(ring), sourceFace, editor.m_FaceShells[f], editor.NewFaceId());
    }

    plBrushFace& face = mesh.m_Faces[f];
    face.m_Corners.SetCount(static_cast<plUInt16>(n));
    for (plUInt32 i = 0; i < n; ++i)
      face.m_Corners[i] = inner[i];

    face.m_uiFlags &= ~plBrushFaceFlags::ExplicitUV;
    face.m_CornerUVs.Clear();
  }

  editor.Finish();
  return plStatus(PL_SUCCESS);
}

plStatus plMeshOps::Flip(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& faceIds)
{
  plDynamicArray<plUInt32> selected;
  GatherFaceIndices(inout_mesh, faceIds, selected);

  if (selected.IsEmpty())
    return NothingSelected("Flip");

  for (plUInt32 f : selected)
  {
    plBrushFace& face = inout_mesh.m_Faces[f];
    const plUInt32 n = face.m_Corners.GetCount();

    for (plUInt32 i = 0; i < n / 2; ++i)
    {
      plMath::Swap(face.m_Corners[i], face.m_Corners[n - 1 - i]);

      if (face.m_CornerUVs.GetCount() == n)
        plMath::Swap(face.m_CornerUVs[i], face.m_CornerUVs[n - 1 - i]);
    }
  }

  return plStatus(PL_SUCCESS);
}

plStatus plMeshOps::Subdivide(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& faceIds)
{
  plDynamicArray<plUInt32> selected;
  GatherFaceIndices(inout_mesh, faceIds, selected);

  if (selected.IsEmpty())
    return NothingSelected("Subdivide");

  FaceEditor editor(inout_mesh);
  plEditableMesh& mesh = inout_mesh;

  // one midpoint per edge, shared by both faces of the edge
  plHashTable<plUInt64, plUInt32> midpoints;

  auto getMidpoint = [&](plUInt32 a, plUInt32 b)
  {
    const plUInt64 uiKey = plMeshTopology::MakeKey(a, b);
    plUInt32 uiMid = 0;

    if (!midpoints.TryGetValue(uiKey, uiMid))
    {
      uiMid = mesh.AddVertex(plMath::Lerp(mesh.m_Vertices[a], mesh.m_Vertices[b], 0.5f));
      midpoints.Insert(uiKey, uiMid);
    }

    return uiMid;
  };

  // outlines of the selected faces, captured before any face changes
  struct Outline
  {
    plUInt32 m_uiFace;
    plHybridArray<plUInt32, 8> m_Corners;
  };
  plDynamicArray<Outline> outlines;

  for (plUInt32 f : selected)
  {
    Outline& o = outlines.ExpandAndGetRef();
    o.m_uiFace = f;
    o.m_Corners.PushBackRange(mesh.m_Faces[f].m_Corners.GetArrayPtr());
  }

  for (const Outline& o : outlines)
  {
    const plUInt32 n = o.m_Corners.GetCount();

    plVec3 vCenter = plVec3::MakeZero();
    for (plUInt32 c : o.m_Corners)
      vCenter += mesh.m_Vertices[c];
    const plUInt32 uiCenter = mesh.AddVertex(vCenter / static_cast<float>(n));

    const plBrushFace sourceFace = mesh.m_Faces[o.m_uiFace];

    for (plUInt32 i = 0; i < n; ++i)
    {
      const plUInt32 prev = o.m_Corners[(i + n - 1) % n];
      const plUInt32 cur = o.m_Corners[i];
      const plUInt32 next = o.m_Corners[(i + 1) % n];

      const plUInt32 quad[4] = {getMidpoint(prev, cur), cur, getMidpoint(cur, next), uiCenter};

      if (i == 0)
      {
        // the first quad takes over the original face (and its id)
        plBrushFace& face = mesh.m_Faces[o.m_uiFace];
        face.m_Corners.SetCount(4);
        for (plUInt32 k = 0; k < 4; ++k)
          face.m_Corners[k] = quad[k];
        face.m_uiFlags &= ~plBrushFaceFlags::ExplicitUV;
        face.m_CornerUVs.Clear();
      }
      else
      {
        editor.AddFace(plArrayPtr<const plUInt32>(quad), sourceFace, editor.m_FaceShells[o.m_uiFace], sourceFace.m_uiFaceId);
      }
    }
  }

  // unselected neighbours take the midpoints into their outlines; the subdivided faces (all carrying a
  // selected id) already contain them
  plHashSet<plUInt32> subdivided;

  for (plUInt32 f = 0; f < mesh.m_Faces.GetCount(); ++f)
  {
    if (faceIds.Contains(mesh.m_Faces[f].m_uiFaceId))
      subdivided.Insert(f);
  }

  for (auto it : midpoints)
  {
    const plUInt32 a = static_cast<plUInt32>(it.Key() & 0xFFFFFFFF);
    const plUInt32 b = static_cast<plUInt32>(it.Key() >> 32);
    const plUInt32 mid = it.Value();

    InsertOnEdge(mesh, a, b, plArrayPtr<const plUInt32>(&mid, 1), &subdivided);
  }

  editor.Finish();
  return plStatus(PL_SUCCESS);
}

plStatus plMeshOps::DeleteFaces(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& faceIds)
{
  plDynamicArray<plUInt32> selected;
  GatherFaceIndices(inout_mesh, faceIds, selected);

  if (selected.IsEmpty())
    return NothingSelected("Delete");

  plHashSet<plUInt32> remove;
  for (plUInt32 f : selected)
    remove.Insert(f);

  FaceEditor editor(inout_mesh);
  editor.RemoveFaces(remove);
  editor.Finish();
  return plStatus(PL_SUCCESS);
}

plStatus plMeshOps::Bridge(plEditableMesh& inout_mesh, plUInt32 uiFaceIdA, plUInt32 uiFaceIdB)
{
  plUInt32 fa = plInvalidIndex, fb = plInvalidIndex;

  for (plUInt32 f = 0; f < inout_mesh.m_Faces.GetCount(); ++f)
  {
    if (inout_mesh.m_Faces[f].m_uiFaceId == uiFaceIdA && fa == plInvalidIndex)
      fa = f;
    else if (inout_mesh.m_Faces[f].m_uiFaceId == uiFaceIdB && fb == plInvalidIndex)
      fb = f;
  }

  if (fa == plInvalidIndex || fb == plInvalidIndex || fa == fb)
    return plStatus("Bridge: select exactly two faces.");

  const plBrushFace faceA = inout_mesh.m_Faces[fa];
  const plBrushFace faceB = inout_mesh.m_Faces[fb];
  const plUInt32 n = faceA.m_Corners.GetCount();

  if (faceB.m_Corners.GetCount() != n)
    return plStatus(plFmt("Bridge: the faces need the same number of corners ({} and {}).", n, faceB.m_Corners.GetCount()));

  // B's loop runs the other way round a tube; pick the rotation that pairs up the closest corners
  plHybridArray<plUInt32, 8> loopB;
  for (plUInt32 i = n; i > 0; --i)
    loopB.PushBack(faceB.m_Corners[i - 1]);

  plUInt32 uiBestOffset = 0;
  float fBestDist = plMath::MaxValue<float>();

  for (plUInt32 offset = 0; offset < n; ++offset)
  {
    float fDist = 0.0f;
    for (plUInt32 i = 0; i < n; ++i)
      fDist += (inout_mesh.m_Vertices[faceA.m_Corners[i]] - inout_mesh.m_Vertices[loopB[(i + offset) % n]]).GetLengthSquared();

    if (fDist < fBestDist)
    {
      fBestDist = fDist;
      uiBestOffset = offset;
    }
  }

  FaceEditor editor(inout_mesh);

  // both faces' shells become one
  const plUInt32 uiShell = editor.m_FaceShells[fa];
  const plUInt32 uiOtherShell = editor.m_FaceShells[fb];

  for (plUInt32& s : editor.m_FaceShells)
  {
    if (s == uiOtherShell)
      s = uiShell;
  }

  for (plUInt32 i = 0; i < n; ++i)
  {
    const plUInt32 a0 = faceA.m_Corners[i];
    const plUInt32 a1 = faceA.m_Corners[(i + 1) % n];
    const plUInt32 b0 = loopB[(i + uiBestOffset) % n];
    const plUInt32 b1 = loopB[(i + 1 + uiBestOffset) % n];

    // the wall takes over the removed faces' edges: a0->a1 from A, b1->b0 from B
    const plUInt32 wall[4] = {a0, a1, b1, b0};
    editor.AddFace(plArrayPtr<const plUInt32>(wall), faceA, uiShell, editor.NewFaceId());
  }

  plHashSet<plUInt32> remove;
  remove.Insert(fa);
  remove.Insert(fb);
  editor.RemoveFaces(remove);
  editor.Finish();
  return plStatus(PL_SUCCESS);
}

plStatus plMeshOps::Detach(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& faceIds, plEditableMesh& out_detached)
{
  plDynamicArray<plUInt32> selected;
  GatherFaceIndices(inout_mesh, faceIds, selected);

  out_detached.Clear();

  if (selected.IsEmpty())
    return NothingSelected("Detach");

  if (selected.GetCount() == inout_mesh.m_Faces.GetCount())
    return plStatus("Detach: that is the whole brush. Duplicate it instead.");

  plHashSet<plUInt32> remove;
  plHashTable<plUInt32, plUInt32> remap;

  out_detached.BeginShell(false);

  for (plUInt32 f : selected)
  {
    remove.Insert(f);

    const plBrushFace& face = inout_mesh.m_Faces[f];
    plSmallArray<plUInt32, 8> corners;

    for (plUInt32 c : face.m_Corners)
    {
      plUInt32 uiNew = 0;
      if (!remap.TryGetValue(c, uiNew))
      {
        uiNew = out_detached.AddVertex(inout_mesh.m_Vertices[c]);
        remap.Insert(c, uiNew);
      }
      corners.PushBack(uiNew);
    }

    const plUInt32 uiNewFace = out_detached.AddFace(plArrayPtr<const plUInt32>(corners.GetData(), corners.GetCount()), face.m_uiFaceId, face.m_uiMaterialSlot);
    out_detached.m_Faces[uiNewFace].m_UV = face.m_UV;
    out_detached.m_Faces[uiNewFace].m_uiFlags = face.m_uiFlags;
    out_detached.m_Faces[uiNewFace].m_CornerUVs = face.m_CornerUVs;
  }

  out_detached.m_uiNextFaceId = plMath::Max(out_detached.m_uiNextFaceId, inout_mesh.m_uiNextFaceId);

  FaceEditor editor(inout_mesh);
  editor.RemoveFaces(remove);
  editor.Finish();
  return plStatus(PL_SUCCESS);
}

plStatus plMeshOps::Triangulate(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& faceIds)
{
  plDynamicArray<plUInt32> selected;
  GatherFaceIndices(inout_mesh, faceIds, selected);

  if (selected.IsEmpty())
    return NothingSelected("Triangulate");

  FaceEditor editor(inout_mesh);
  plEditableMesh& mesh = inout_mesh;

  plDynamicArray<plUInt32> tris;

  for (plUInt32 f : selected)
  {
    if (mesh.m_Faces[f].m_Corners.GetCount() <= 3)
      continue;

    mesh.TriangulateFace(f, tris);
    const plBrushFace source = mesh.m_Faces[f];

    for (plUInt32 t = 0; t + 2 < tris.GetCount(); t += 3)
    {
      const plUInt32 tri[3] = {source.m_Corners[tris[t]], source.m_Corners[tris[t + 1]], source.m_Corners[tris[t + 2]]};

      if (t == 0)
      {
        plBrushFace& face = mesh.m_Faces[f];
        face.m_Corners.SetCount(3);
        for (plUInt32 k = 0; k < 3; ++k)
          face.m_Corners[k] = tri[k];

        if (source.m_CornerUVs.GetCount() == source.m_Corners.GetCount())
        {
          face.m_CornerUVs.SetCount(3);
          for (plUInt32 k = 0; k < 3; ++k)
            face.m_CornerUVs[k] = source.m_CornerUVs[tris[t + k]];
        }
      }
      else
      {
        const plUInt32 uiNew = editor.AddFace(plArrayPtr<const plUInt32>(tri), source, editor.m_FaceShells[f], source.m_uiFaceId);

        if (source.m_CornerUVs.GetCount() == source.m_Corners.GetCount())
        {
          plBrushFace& face = mesh.m_Faces[uiNew];
          face.m_uiFlags |= source.m_uiFlags & plBrushFaceFlags::ExplicitUV;
          face.m_CornerUVs.SetCount(3);
          for (plUInt32 k = 0; k < 3; ++k)
            face.m_CornerUVs[k] = source.m_CornerUVs[tris[t + k]];
        }
      }
    }
  }

  editor.Finish();
  return plStatus(PL_SUCCESS);
}

plStatus plMeshOps::Flatten(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& faceIds)
{
  plDynamicArray<plUInt32> selected;
  GatherFaceIndices(inout_mesh, faceIds, selected);

  if (selected.IsEmpty())
    return NothingSelected("Flatten");

  for (plUInt32 f : selected)
  {
    const plVec3 vNormal = inout_mesh.ComputeFaceNormal(f);
    const plVec3 vCenter = inout_mesh.ComputeFaceCenter(f);

    for (plUInt32 c : inout_mesh.m_Faces[f].m_Corners)
    {
      plVec3& v = inout_mesh.m_Vertices[c];
      v -= vNormal * vNormal.Dot(v - vCenter);
    }
  }

  return plStatus(PL_SUCCESS);
}

//////////////////////////////////////////////////////////////////////////
// edge operations

plStatus plMeshOps::Bevel(plEditableMesh& inout_mesh, plArrayPtr<const plUInt64> edgeKeys, float fWidth, plUInt32 uiSegments)
{
  if (edgeKeys.IsEmpty())
    return NothingSelected("Bevel");

  if (fWidth <= 0.0f)
    return plStatus("Bevel: the width must be greater than zero.");

  uiSegments = plMath::Clamp<plUInt32>(uiSegments, 1, 16);

  plMeshTopology topology;
  topology.Build(inout_mesh);

  struct EdgeInfo
  {
    plUInt32 m_uiHalfEdge = 0; ///< a -> b in f1
    plUInt32 a = 0;
    plUInt32 b = 0;
  };

  plHybridArray<EdgeInfo, 8> edges;
  plHashSet<plUInt32> usedVertices;

  for (plUInt64 uiKey : edgeKeys)
  {
    const plUInt32 v0 = static_cast<plUInt32>(uiKey & 0xFFFFFFFF);
    const plUInt32 v1 = static_cast<plUInt32>(uiKey >> 32);

    const plUInt32 h = topology.FindEdge(v0, v1);

    if (h == plMeshTopology::c_uiInvalid || !topology.IsEdgeManifold(h))
      return plStatus("Bevel: every edge needs exactly two faces (the mesh is open or non-manifold there).");

    const auto& he = topology.GetHalfEdge(h);

    plDynamicArray<plUInt32> faces;
    for (plUInt32 v : {he.m_uiFrom, he.m_uiTo})
    {
      topology.GetVertexFaces(v, faces);
      if (faces.GetCount() != 3)
        return plStatus("Bevel: only edges between box-like corners (three faces per end) can be bevelled.");

      if (usedVertices.Contains(v))
        return plStatus("Bevel: selected edges must not share a corner.");
    }

    usedVertices.Insert(he.m_uiFrom);
    usedVertices.Insert(he.m_uiTo);

    EdgeInfo& e = edges.ExpandAndGetRef();
    e.m_uiHalfEdge = h;
    e.a = he.m_uiFrom;
    e.b = he.m_uiTo;
  }

  FaceEditor editor(inout_mesh);
  plEditableMesh& mesh = inout_mesh;

  for (const EdgeInfo& e : edges)
  {
    const auto& h1 = topology.GetHalfEdge(e.m_uiHalfEdge);        // a -> b in f1
    const auto& h2 = topology.GetHalfEdge(h1.m_uiTwin);           // b -> a in f2
    const plUInt32 f1 = h1.m_uiFace;
    const plUInt32 f2 = h2.m_uiFace;

    const plUInt32 p = topology.GetHalfEdge(h1.m_uiPrev).m_uiFrom; // p -> a in f1
    const plUInt32 q = topology.GetHalfEdge(h1.m_uiNext).m_uiTo;   // b -> q in f1
    const plUInt32 r = topology.GetHalfEdge(h2.m_uiNext).m_uiTo;   // a -> r in f2
    const plUInt32 s = topology.GetHalfEdge(h2.m_uiPrev).m_uiFrom; // s -> b in f2

    const plVec3 va = mesh.m_Vertices[e.a];
    const plVec3 vb = mesh.m_Vertices[e.b];

    auto along = [&](const plVec3& from, plUInt32 uiTowards)
    {
      const plVec3 dir = mesh.m_Vertices[uiTowards] - from;
      const float fLen = dir.GetLength();
      return from + dir * (plMath::Min(fWidth, fLen * 0.49f) / plMath::Max(fLen, 0.00001f));
    };

    const plVec3 a1 = along(va, p), b1 = along(vb, q);
    const plVec3 a2 = along(va, r), b2 = along(vb, s);

    // profile points from the f1 side (t=0) to the f2 side (t=1), rounded through the old corner
    plHybridArray<plUInt32, 17> profileA, profileB;

    for (plUInt32 k = 0; k <= uiSegments; ++k)
    {
      const float t = static_cast<float>(k) / uiSegments;
      const float u = 1.0f - t;

      profileA.PushBack(mesh.AddVertex(a1 * (u * u) + va * (2.0f * t * u) + a2 * (t * t)));
      profileB.PushBack(mesh.AddVertex(b1 * (u * u) + vb * (2.0f * t * u) + b2 * (t * t)));
    }

    // f1 and f2 shrink back from the edge
    ReplaceCorner(mesh.m_Faces[f1], e.a, plArrayPtr<const plUInt32>(&profileA[0], 1));
    ReplaceCorner(mesh.m_Faces[f1], e.b, plArrayPtr<const plUInt32>(&profileB[0], 1));
    ReplaceCorner(mesh.m_Faces[f2], e.a, plArrayPtr<const plUInt32>(&profileA[uiSegments], 1));
    ReplaceCorner(mesh.m_Faces[f2], e.b, plArrayPtr<const plUInt32>(&profileB[uiSegments], 1));

    // the third face at a runs r -> a -> p: a becomes the profile from the f2 side to the f1 side
    plDynamicArray<plUInt32> faces;
    topology.GetVertexFaces(e.a, faces);
    for (plUInt32 f3 : faces)
    {
      if (f3 == f1 || f3 == f2)
        continue;

      plHybridArray<plUInt32, 17> reversed;
      for (plUInt32 k = uiSegments + 1; k > 0; --k)
        reversed.PushBack(profileA[k - 1]);

      ReplaceCorner(mesh.m_Faces[f3], e.a, reversed.GetArrayPtr());
    }

    // the third face at b runs q -> b -> s: b becomes the profile from the f1 side to the f2 side
    topology.GetVertexFaces(e.b, faces);
    for (plUInt32 f4 : faces)
    {
      if (f4 == f1 || f4 == f2)
        continue;

      ReplaceCorner(mesh.m_Faces[f4], e.b, profileB.GetArrayPtr());
    }

    for (plUInt32 uiFace : {f1, f2})
    {
      mesh.m_Faces[uiFace].m_uiFlags &= ~plBrushFaceFlags::ExplicitUV;
      mesh.m_Faces[uiFace].m_CornerUVs.Clear();
    }

    // the chamfer strip
    const plBrushFace sourceFace = mesh.m_Faces[f1];
    const plUInt32 uiStripId = editor.NewFaceId();

    for (plUInt32 k = 0; k < uiSegments; ++k)
    {
      const plUInt32 strip[4] = {profileA[k], profileA[k + 1], profileB[k + 1], profileB[k]};
      editor.AddFace(plArrayPtr<const plUInt32>(strip), sourceFace, editor.m_FaceShells[f1], uiStripId);
    }
  }

  editor.Finish();
  return plStatus(PL_SUCCESS);
}

plStatus plMeshOps::LoopCut(plEditableMesh& inout_mesh, plUInt32 uiV0, plUInt32 uiV1, plUInt32 uiCuts, float fPosition)
{
  uiCuts = plMath::Clamp<plUInt32>(uiCuts, 1, 32);

  plMeshTopology topology;
  topology.Build(inout_mesh);

  const plUInt32 h = topology.FindHalfEdge(uiV0, uiV1) != plMeshTopology::c_uiInvalid ? topology.FindHalfEdge(uiV0, uiV1) : topology.FindHalfEdge(uiV1, uiV0);

  if (h == plMeshTopology::c_uiInvalid)
    return plStatus("Loop cut: pick an edge of the brush.");

  plDynamicArray<plMeshTopology::RingEdge> ring;
  plDynamicArray<plUInt32> quads;
  bool bClosed = false;
  topology.GetEdgeRing(h, ring, quads, bClosed);

  if (quads.IsEmpty())
    return plStatus("Loop cut: the edge has no quad next to it.");

  // cut positions from side A of the ring; a single cut honours fPosition measured from v0's side
  const bool bStartFromV0 = topology.GetHalfEdge(h).m_uiFrom == uiV0;
  plHybridArray<float, 32> params;

  for (plUInt32 k = 1; k <= uiCuts; ++k)
  {
    float t = static_cast<float>(k) / (uiCuts + 1);

    if (uiCuts == 1)
      t = plMath::Clamp(bStartFromV0 ? fPosition : 1.0f - fPosition, 0.01f, 0.99f);

    params.PushBack(t);
  }

  plEditableMesh& mesh = inout_mesh;

  // new vertices per ring edge, ordered from side A
  plHashTable<plUInt64, plHybridArray<plUInt32, 32>> points;

  for (const auto& e : ring)
  {
    auto& list = points[plMeshTopology::MakeKey(e.m_uiA, e.m_uiB)];

    if (!list.IsEmpty())
      continue;

    for (float t : params)
      list.PushBack(mesh.AddVertex(plMath::Lerp(mesh.m_Vertices[e.m_uiA], mesh.m_Vertices[e.m_uiB], t)));
  }

  plHashTable<plUInt64, plUInt32> sideA; // ring edge key -> its side-A vertex
  for (const auto& e : ring)
    sideA[plMeshTopology::MakeKey(e.m_uiA, e.m_uiB)] = e.m_uiA;

  auto pointsFromTo = [&](plUInt32 from, plUInt32 to, plHybridArray<plUInt32, 32>& out_points)
  {
    out_points.Clear();
    const plUInt64 uiKey = plMeshTopology::MakeKey(from, to);
    const auto& list = *points.GetValue(uiKey);

    if (*sideA.GetValue(uiKey) == from)
    {
      out_points = list;
    }
    else
    {
      for (plUInt32 i = list.GetCount(); i > 0; --i)
        out_points.PushBack(list[i - 1]);
    }
  };

  FaceEditor editor(inout_mesh);

  for (plUInt32 f : quads)
  {
    const plBrushFace source = mesh.m_Faces[f];
    const plUInt32* c = source.m_Corners.GetData();

    // rotate so edges (x0,x1) and (x2,x3) are the ring edges
    plUInt32 uiRot = plInvalidIndex;
    for (plUInt32 i = 0; i < 4; ++i)
    {
      if (points.Contains(plMeshTopology::MakeKey(c[i], c[(i + 1) % 4])) && points.Contains(plMeshTopology::MakeKey(c[(i + 2) % 4], c[(i + 3) % 4])))
      {
        uiRot = i;
        break;
      }
    }

    if (uiRot == plInvalidIndex)
      continue;

    const plUInt32 x0 = c[uiRot], x1 = c[(uiRot + 1) % 4], x2 = c[(uiRot + 2) % 4], x3 = c[(uiRot + 3) % 4];

    plHybridArray<plUInt32, 32> p, q;
    pointsFromTo(x0, x1, p);
    pointsFromTo(x3, x2, q);

    plHybridArray<plUInt32, 34> side0, side1;
    side0.PushBack(x0);
    side0.PushBackRange(p.GetArrayPtr());
    side0.PushBack(x1);
    side1.PushBack(x3);
    side1.PushBackRange(q.GetArrayPtr());
    side1.PushBack(x2);

    for (plUInt32 k = 0; k + 1 < side0.GetCount(); ++k)
    {
      const plUInt32 sub[4] = {side0[k], side0[k + 1], side1[k + 1], side1[k]};

      if (k == 0)
      {
        plBrushFace& face = mesh.m_Faces[f];
        face.m_Corners.SetCount(4);
        for (plUInt32 i = 0; i < 4; ++i)
          face.m_Corners[i] = sub[i];
        face.m_uiFlags &= ~plBrushFaceFlags::ExplicitUV;
        face.m_CornerUVs.Clear();
      }
      else
      {
        editor.AddFace(plArrayPtr<const plUInt32>(sub), source, editor.m_FaceShells[f], editor.NewFaceId());
      }
    }
  }

  // faces outside the ring that share an end edge get the new points, so no T-junctions appear; the
  // split quads no longer contain an unsplit ring edge, so only those neighbours match
  for (const auto& e : ring)
  {
    plHybridArray<plUInt32, 32> list;
    pointsFromTo(e.m_uiA, e.m_uiB, list);

    InsertOnEdge(mesh, e.m_uiA, e.m_uiB, list.GetArrayPtr(), nullptr);
  }

  editor.Finish();
  return plStatus(PL_SUCCESS);
}

plStatus plMeshOps::FillHole(plEditableMesh& inout_mesh, plUInt32 uiV0, plUInt32 uiV1)
{
  plMeshTopology topology;
  topology.Build(inout_mesh);

  plUInt32 h = topology.FindHalfEdge(uiV0, uiV1);
  if (h == plMeshTopology::c_uiInvalid || !topology.IsBoundary(h))
    h = topology.FindHalfEdge(uiV1, uiV0);

  if (h == plMeshTopology::c_uiInvalid || !topology.IsBoundary(h))
    return plStatus("Fill hole: pick an edge on the rim of an opening.");

  // the outgoing boundary half-edge of every boundary vertex
  plHashTable<plUInt32, plUInt32> outgoing;
  for (plUInt32 i = 0; i < topology.GetHalfEdgeCount(); ++i)
  {
    if (topology.IsBoundary(i))
      outgoing[topology.GetHalfEdge(i).m_uiFrom] = i;
  }

  plHybridArray<plUInt32, 16> loop;
  plUInt32 uiCur = h;

  for (plUInt32 uiGuard = 0; uiGuard < topology.GetHalfEdgeCount(); ++uiGuard)
  {
    loop.PushBack(topology.GetHalfEdge(uiCur).m_uiFrom);

    plUInt32 uiNext = plInvalidIndex;
    if (!outgoing.TryGetValue(topology.GetHalfEdge(uiCur).m_uiTo, uiNext))
      return plStatus("Fill hole: the opening's rim is not a closed loop.");

    if (uiNext == h)
      break;

    uiCur = uiNext;
  }

  if (loop.GetCount() < 3)
    return plStatus("Fill hole: the opening is too small.");

  // the new face runs against the rim's half-edges
  plHybridArray<plUInt32, 16> corners;
  for (plUInt32 i = loop.GetCount(); i > 0; --i)
    corners.PushBack(loop[i - 1]);

  FaceEditor editor(inout_mesh);
  const plUInt32 uiNeighbour = topology.GetHalfEdge(h).m_uiFace;
  editor.AddFace(corners.GetArrayPtr(), inout_mesh.m_Faces[uiNeighbour], editor.m_FaceShells[uiNeighbour], editor.NewFaceId());
  editor.Finish();
  return plStatus(PL_SUCCESS);
}

//////////////////////////////////////////////////////////////////////////
// vertex operations

plStatus plMeshOps::Weld(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& vertices)
{
  if (vertices.GetCount() < 2)
    return plStatus("Weld: select at least two vertices.");

  plVec3 vCenter = plVec3::MakeZero();
  plUInt32 uiTarget = plInvalidIndex;

  for (plUInt32 v : vertices)
  {
    if (v >= inout_mesh.m_Vertices.GetCount())
      return plStatus("Weld: the selection is out of date.");

    vCenter += inout_mesh.m_Vertices[v];
    uiTarget = plMath::Min(uiTarget, v);
  }

  inout_mesh.m_Vertices[uiTarget] = vCenter / static_cast<float>(vertices.GetCount());

  for (plBrushFace& face : inout_mesh.m_Faces)
  {
    for (plUInt32& c : face.m_Corners)
    {
      if (vertices.Contains(c))
        c = uiTarget;
    }
  }

  // collapsed corners and faces go; a tiny weld distance keeps everything else untouched
  WeldByDistance(inout_mesh, 0.000001f);
  return plStatus(PL_SUCCESS);
}

plStatus plMeshOps::Connect(plEditableMesh& inout_mesh, plUInt32 uiV0, plUInt32 uiV1)
{
  for (plUInt32 f = 0; f < inout_mesh.m_Faces.GetCount(); ++f)
  {
    const plBrushFace& face = inout_mesh.m_Faces[f];
    const plUInt32 n = face.m_Corners.GetCount();

    const plUInt32 i0 = face.m_Corners.IndexOf(uiV0);
    const plUInt32 i1 = face.m_Corners.IndexOf(uiV1);

    if (i0 == plInvalidIndex || i1 == plInvalidIndex)
      continue;

    if ((i0 + 1) % n == i1 || (i1 + 1) % n == i0)
      return plStatus("Connect: the vertices are already joined by an edge.");

    const plBrushFace source = face;

    plHybridArray<plUInt32, 16> first, second;
    for (plUInt32 i = i0;; i = (i + 1) % n)
    {
      first.PushBack(source.m_Corners[i]);
      if (i == i1)
        break;
    }
    for (plUInt32 i = i1;; i = (i + 1) % n)
    {
      second.PushBack(source.m_Corners[i]);
      if (i == i0)
        break;
    }

    FaceEditor editor(inout_mesh);

    plBrushFace& target = inout_mesh.m_Faces[f];
    target.m_Corners.SetCount(static_cast<plUInt16>(first.GetCount()));
    for (plUInt32 i = 0; i < first.GetCount(); ++i)
      target.m_Corners[i] = first[i];
    target.m_uiFlags &= ~plBrushFaceFlags::ExplicitUV;
    target.m_CornerUVs.Clear();

    editor.AddFace(second.GetArrayPtr(), source, editor.m_FaceShells[f], editor.NewFaceId());
    editor.Finish();
    return plStatus(PL_SUCCESS);
  }

  return plStatus("Connect: the two vertices do not share a face.");
}

plStatus plMeshOps::DeleteVertices(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& vertices)
{
  if (vertices.IsEmpty())
    return NothingSelected("Delete");

  plHashSet<plUInt32> remove;
  for (plUInt32 f = 0; f < inout_mesh.m_Faces.GetCount(); ++f)
  {
    for (plUInt32 c : inout_mesh.m_Faces[f].m_Corners)
    {
      if (vertices.Contains(c))
      {
        remove.Insert(f);
        break;
      }
    }
  }

  if (remove.IsEmpty())
    return NothingSelected("Delete");

  FaceEditor editor(inout_mesh);
  editor.RemoveFaces(remove);
  editor.Finish();
  return plStatus(PL_SUCCESS);
}


PL_STATICLINK_FILE(ModelingPlugin, ModelingPlugin_Modeling_Implementation_MeshOps);
