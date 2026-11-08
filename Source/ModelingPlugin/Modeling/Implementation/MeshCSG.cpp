#include <ModelingPlugin/ModelingPluginPCH.h>

#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/Containers/HashSet.h>
#include <Foundation/Containers/HashTable.h>
#include <ModelingPlugin/Modeling/MeshCSG.h>

namespace
{
  constexpr float c_fPlaneEpsilon = 0.0005f;

  plPlane MakeFacePlane(const plEditableMesh& mesh, const plBrushFace& face)
  {
    // Newell normal — robust for slightly non-planar loops
    plVec3 vNormal = plVec3::MakeZero();

    for (plUInt32 i = 0; i < face.m_Corners.GetCount(); ++i)
    {
      const plVec3& cur = mesh.m_Vertices[face.m_Corners[i]];
      const plVec3& next = mesh.m_Vertices[face.m_Corners[(i + 1) % face.m_Corners.GetCount()]];

      vNormal.x += (cur.y - next.y) * (cur.z + next.z);
      vNormal.y += (cur.z - next.z) * (cur.x + next.x);
      vNormal.z += (cur.x - next.x) * (cur.y + next.y);
    }

    vNormal.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();

    return plPlane::MakeFromNormalAndPoint(vNormal, mesh.m_Vertices[face.m_Corners[0]]);
  }

  plUInt64 MakeEdgeKey(plUInt32 uiV0, plUInt32 uiV1)
  {
    const plUInt32 uiLow = plMath::Min(uiV0, uiV1);
    const plUInt32 uiHigh = plMath::Max(uiV0, uiV1);
    return (static_cast<plUInt64>(uiHigh) << 32) | uiLow;
  }

  plUInt64 MakePositionKey(const plVec3& v)
  {
    const plInt64 key[3] = {
      static_cast<plInt64>(plMath::Round(v.x * 10000.0f)),
      static_cast<plInt64>(plMath::Round(v.y * 10000.0f)),
      static_cast<plInt64>(plMath::Round(v.z * 10000.0f))};
    return plHashingUtils::xxHash64(key, sizeof(key));
  }

  constexpr float c_fPlanarityEpsilon = 0.001f;

  /// Drops repeated corners and corners where the loop doubles back on itself (collinear, reversed).
  void RemoveSpikes(const plEditableMesh& mesh, plSmallArray<plUInt32, 8>& ref_corners, plSmallArray<plVec2, 8>* pUVs)
  {
    bool bChanged = true;

    while (bChanged && ref_corners.GetCount() >= 3)
    {
      bChanged = false;
      const plUInt32 n = ref_corners.GetCount();

      for (plUInt32 i = 0; i < n; ++i)
      {
        const plVec3& prev = mesh.m_Vertices[ref_corners[(i + n - 1) % n]];
        const plVec3& cur = mesh.m_Vertices[ref_corners[i]];
        const plVec3& next = mesh.m_Vertices[ref_corners[(i + 1) % n]];

        const plVec3 vIn = cur - prev;
        const plVec3 vOut = next - cur;

        const bool bRepeated = vIn.GetLengthSquared() < 1e-10f;
        const bool bSpike = !bRepeated && vIn.CrossRH(vOut).GetLengthSquared() <= 1e-10f * vIn.GetLengthSquared() * plMath::Max(vOut.GetLengthSquared(), 1e-6f) && vIn.Dot(vOut) < 0.0f;

        if (bRepeated || bSpike)
        {
          ref_corners.RemoveAtAndCopy(static_cast<plUInt16>(i));

          if (pUVs)
            pUVs->RemoveAtAndCopy(static_cast<plUInt16>(i));

          bChanged = true;
          break;
        }
      }
    }
  }

  /// The bounding planes of one face: its own plane when flat, one per triangle when a vertex edit bent it.
  void AppendFacePlanes(const plEditableMesh& mesh, plUInt32 uiFace, plDynamicArray<plPlane>& ref_planes)
  {
    const plBrushFace& face = mesh.m_Faces[uiFace];

    if (face.m_Corners.GetCount() == 3 || mesh.ComputeFacePlanarityError(uiFace) <= c_fPlanarityEpsilon)
    {
      ref_planes.PushBack(MakeFacePlane(mesh, face));
      return;
    }

    plDynamicArray<plUInt32> tris;
    mesh.TriangulateFace(uiFace, tris);

    for (plUInt32 t = 0; t + 2 < tris.GetCount(); t += 3)
    {
      plPlane plane;
      if (plane.SetFromPoints(mesh.m_Vertices[face.m_Corners[tris[t]]], mesh.m_Vertices[face.m_Corners[tris[t + 1]]], mesh.m_Vertices[face.m_Corners[tris[t + 2]]]).Succeeded())
      {
        ref_planes.PushBack(plane);
      }
    }
  }
} // namespace

bool plMeshCSG::IsShellConvex(const plEditableMesh& mesh, const plBrushShell& shell, float fEpsilon)
{
  plDynamicArray<plPlane> planes;
  GetShellPlanes(mesh, shell, planes);

  for (const plPlane& plane : planes)
  {
    for (plUInt32 uiOtherFace = shell.m_uiFirstFace; uiOtherFace < shell.m_uiFirstFace + shell.m_uiNumFaces; ++uiOtherFace)
    {
      for (plUInt32 uiCorner : mesh.m_Faces[uiOtherFace].m_Corners)
      {
        if (plane.GetDistanceTo(mesh.m_Vertices[uiCorner]) > fEpsilon)
          return false;
      }
    }
  }

  return true;
}

void plMeshCSG::GetShellPlanes(const plEditableMesh& mesh, const plBrushShell& shell, plDynamicArray<plPlane>& out_planes)
{
  out_planes.Clear();
  out_planes.Reserve(shell.m_uiNumFaces);

  for (plUInt32 uiFace = shell.m_uiFirstFace; uiFace < shell.m_uiFirstFace + shell.m_uiNumFaces; ++uiFace)
  {
    AppendFacePlanes(mesh, uiFace, out_planes);
  }

  // coplanar faces (a split face, a triangulated quad) would clip twice along the same plane
  for (plUInt32 i = 0; i < out_planes.GetCount(); ++i)
  {
    for (plUInt32 j = out_planes.GetCount() - 1; j > i; --j)
    {
      if (out_planes[i].m_vNormal.Dot(out_planes[j].m_vNormal) > 0.9999f && plMath::Abs(out_planes[i].m_fNegDistance - out_planes[j].m_fNegDistance) < 0.0005f)
      {
        out_planes.RemoveAtAndCopy(j);
      }
    }
  }
}

void plMeshCSG::ClipByPlane(const plEditableMesh& in, const plPlane& plane, bool bKeepPositiveSide, plUInt8 uiCapMaterialSlot, plUInt32& ref_uiNextFaceId, plEditableMesh& out, plUInt32 uiFixedCapFaceId)
{
  out.Clear();
  out.m_uiNextFaceId = plMath::Max(in.m_uiNextFaceId, ref_uiNextFaceId);

  // signed distance per vertex, folded so "keep" is always the positive side
  plDynamicArray<float> dist;
  dist.SetCountUninitialized(in.m_Vertices.GetCount());

  const float fSide = bKeepPositiveSide ? 1.0f : -1.0f;

  for (plUInt32 i = 0; i < in.m_Vertices.GetCount(); ++i)
  {
    dist[i] = plane.GetDistanceTo(in.m_Vertices[i]) * fSide;
  }

  plDynamicArray<plUInt32> vertexRemap;
  vertexRemap.SetCount(in.m_Vertices.GetCount(), plInvalidIndex);

  plHashTable<plUInt64, plUInt32> edgeCuts; // original edge -> welded intersection vertex in `out`
  plDynamicArray<bool> outOnPlane;          // per `out` vertex: lies on the cutting plane

  auto mapVertex = [&](plUInt32 uiOriginal) -> plUInt32
  {
    if (vertexRemap[uiOriginal] == plInvalidIndex)
    {
      vertexRemap[uiOriginal] = out.AddVertex(in.m_Vertices[uiOriginal]);
      outOnPlane.PushBack(plMath::Abs(dist[uiOriginal]) <= c_fPlaneEpsilon);
    }

    return vertexRemap[uiOriginal];
  };

  auto cutEdge = [&](plUInt32 uiV0, plUInt32 uiV1) -> plUInt32
  {
    const plUInt64 uiKey = MakeEdgeKey(uiV0, uiV1);

    plUInt32 uiCut = 0;

    if (!edgeCuts.TryGetValue(uiKey, uiCut))
    {
      // parameter computed from the canonical (low, high) order so both faces of the edge weld exactly
      const plUInt32 uiLow = plMath::Min(uiV0, uiV1);
      const plUInt32 uiHigh = plMath::Max(uiV0, uiV1);
      const float fT = dist[uiLow] / (dist[uiLow] - dist[uiHigh]);

      uiCut = out.AddVertex(plMath::Lerp(in.m_Vertices[uiLow], in.m_Vertices[uiHigh], fT));
      outOnPlane.PushBack(true);
      edgeCuts[uiKey] = uiCut;
    }

    return uiCut;
  };

  for (plUInt32 uiShell = 0; uiShell < plMath::Max<plUInt32>(1, in.m_Shells.GetCount()); ++uiShell)
  {
    // a mesh without shell records is treated as one implicit shell over all faces
    const bool bHasShells = !in.m_Shells.IsEmpty();
    const plUInt32 uiFirstFace = bHasShells ? in.m_Shells[uiShell].m_uiFirstFace : 0;
    const plUInt32 uiEndFace = bHasShells ? uiFirstFace + in.m_Shells[uiShell].m_uiNumFaces : in.m_Faces.GetCount();

    out.BeginShell(false, bHasShells ? in.m_Shells[uiShell].m_bSubtractive : false);

    const plUInt32 uiShellFirstOutFace = out.m_Faces.GetCount();

    // cut segments of this shell, as vertex-index pairs in `out`
    plDynamicArray<plUInt32> capSegments;
    plHashSet<plUInt64> coplanarEdges; // directed edges of kept faces that lie in the plane

    // a shell that only touches the plane gets no cap
    bool bShellCut = false;

    for (plUInt32 uiFace = uiFirstFace; uiFace < uiEndFace && !bShellCut; ++uiFace)
    {
      for (plUInt32 uiCorner : in.m_Faces[uiFace].m_Corners)
      {
        if (dist[uiCorner] < -c_fPlaneEpsilon)
        {
          bShellCut = true;
          break;
        }
      }
    }

    for (plUInt32 uiFace = uiFirstFace; uiFace < uiEndFace; ++uiFace)
    {
      const plBrushFace& face = in.m_Faces[uiFace];
      const plUInt32 uiCorners = face.m_Corners.GetCount();

      plUInt32 uiKept = 0;

      for (plUInt32 i = 0; i < uiCorners; ++i)
      {
        if (dist[face.m_Corners[i]] >= -c_fPlaneEpsilon)
        {
          ++uiKept;
        }
      }

      if (uiKept == 0)
        continue;

      // a face lying in the plane belongs to the side its solid is on (opposite its normal)
      {
        bool bCoplanar = true;
        for (plUInt32 uiCorner : face.m_Corners)
        {
          bCoplanar &= plMath::Abs(dist[uiCorner]) <= c_fPlaneEpsilon;
        }

        if (bCoplanar && in.ComputeFaceNormal(uiFace).Dot(plane.m_vNormal * fSide) > 0.0f)
          continue;
      }

      const bool bExplicitUV = (face.m_uiFlags & plBrushFaceFlags::ExplicitUV) != 0 && face.m_CornerUVs.GetCount() == uiCorners;

      plSmallArray<plUInt32, 8> newCorners;
      plSmallArray<plVec2, 8> newUVs;

      if (uiKept == uiCorners)
      {
        for (plUInt32 i = 0; i < uiCorners; ++i)
        {
          newCorners.PushBack(mapVertex(face.m_Corners[i]));

          if (bExplicitUV)
            newUVs.PushBack(face.m_CornerUVs[i]);
        }
      }
      else
      {
        // Sutherland-Hodgman against the kept halfspace
        for (plUInt32 i = 0; i < uiCorners; ++i)
        {
          const plUInt32 uiNextIdx = (i + 1) % uiCorners;
          const plUInt32 uiCur = face.m_Corners[i];
          const plUInt32 uiNext = face.m_Corners[uiNextIdx];
          const bool bCurIn = dist[uiCur] >= -c_fPlaneEpsilon;
          const bool bNextIn = dist[uiNext] >= -c_fPlaneEpsilon;

          if (bCurIn)
          {
            newCorners.PushBack(mapVertex(uiCur));

            if (bExplicitUV)
              newUVs.PushBack(face.m_CornerUVs[i]);
          }

          if (bCurIn != bNextIn && plMath::Abs(dist[uiCur]) > c_fPlaneEpsilon && plMath::Abs(dist[uiNext]) > c_fPlaneEpsilon)
          {
            newCorners.PushBack(cutEdge(uiCur, uiNext));

            if (bExplicitUV)
            {
              const float fT = dist[uiCur] / (dist[uiCur] - dist[uiNext]);
              newUVs.PushBack(plMath::Lerp(face.m_CornerUVs[i], face.m_CornerUVs[uiNextIdx], fT));
            }
          }
        }
      }

      // clipping a concave face along an on-plane run leaves zero-area spikes and repeated corners
      RemoveSpikes(out, newCorners, bExplicitUV ? &newUVs : nullptr);

      if (newCorners.GetCount() < 3)
        continue;

      const plUInt32 uiNewFace = out.AddFace(plArrayPtr<const plUInt32>(newCorners.GetData(), newCorners.GetCount()), face.m_uiFaceId, face.m_uiMaterialSlot);
      out.m_Faces[uiNewFace].m_UV = face.m_UV;
      out.m_Faces[uiNewFace].m_uiFlags = face.m_uiFlags;

      if (bExplicitUV)
      {
        out.m_Faces[uiNewFace].m_CornerUVs = newUVs;
      }

      // the cap is bounded by every edge of the kept faces that lies in the plane, except where a kept
      // face lying in the plane already closes the solid (it holds the same edge, reversed)
      bool bAllOnPlane = true;
      for (plUInt32 uiCorner : newCorners)
      {
        bAllOnPlane &= outOnPlane[uiCorner];
      }

      for (plUInt32 i = 0; i < newCorners.GetCount(); ++i)
      {
        const plUInt32 a = newCorners[i];
        const plUInt32 b = newCorners[(i + 1) % newCorners.GetCount()];

        if (!outOnPlane[a] || !outOnPlane[b])
          continue;

        if (bAllOnPlane)
        {
          coplanarEdges.Insert((static_cast<plUInt64>(a) << 32) | b);
        }
        else
        {
          capSegments.PushBack(a);
          capSegments.PushBack(b);
        }
      }
    }

    if (!bShellCut)
    {
      capSegments.Clear();
    }
    else if (!coplanarEdges.IsEmpty())
    {
      for (plUInt32 i = 0; i < capSegments.GetCount();)
      {
        const plUInt64 uiReverse = (static_cast<plUInt64>(capSegments[i + 1]) << 32) | capSegments[i];

        if (coplanarEdges.Contains(uiReverse))
        {
          capSegments.RemoveAtAndCopy(i + 1);
          capSegments.RemoveAtAndCopy(i);
        }
        else
        {
          i += 2;
        }
      }
    }

    if (out.m_Faces.GetCount() == uiShellFirstOutFace)
    {
      // shell fully removed — drop the empty shell record
      out.m_Shells.PopBack();
      continue;
    }

    // chain the cut segments into cap loops (a non-convex shell can produce several)
    if (capSegments.GetCount() >= 6)
    {
      // weld crossings by position: split faces (T-junctions) cut the plane at the same point through
      // different edges, and their segments must still chain
      {
        plHashTable<plUInt64, plUInt32> canonical;

        for (plUInt32& uiVertex : capSegments)
        {
          const plUInt64 uiKey = MakePositionKey(out.m_Vertices[uiVertex]);
          plUInt32 uiCanonical = 0;

          if (canonical.TryGetValue(uiKey, uiCanonical))
          {
            uiVertex = uiCanonical;
          }
          else
          {
            canonical.Insert(uiKey, uiVertex);
          }
        }

        // segments collapsed to a point carry no edge
        for (plUInt32 i = 0; i < capSegments.GetCount();)
        {
          if (capSegments[i] == capSegments[i + 1])
          {
            capSegments.RemoveAtAndCopy(i + 1);
            capSegments.RemoveAtAndCopy(i);
          }
          else
          {
            i += 2;
          }
        }
      }

      plHashTable<plUInt32, plSmallArray<plUInt32, 2>> adjacency;

      for (plUInt32 i = 0; i < capSegments.GetCount(); i += 2)
      {
        adjacency[capSegments[i]].PushBack(capSegments[i + 1]);
        adjacency[capSegments[i + 1]].PushBack(capSegments[i]);
      }

      plHashSet<plUInt32> used;

      for (plUInt32 i = 0; i < capSegments.GetCount(); i += 2)
      {
        const plUInt32 uiStart = capSegments[i];

        if (used.Contains(uiStart))
          continue;

        plSmallArray<plUInt32, 16> loop;
        plUInt32 uiCurrent = uiStart;
        plUInt32 uiPrevious = plInvalidIndex;

        while (true)
        {
          loop.PushBack(uiCurrent);
          used.Insert(uiCurrent);

          const auto* pNeighbors = adjacency.GetValue(uiCurrent);

          if (pNeighbors == nullptr)
            break;

          plUInt32 uiNext = plInvalidIndex;

          for (plUInt32 uiCandidate : *pNeighbors)
          {
            if (uiCandidate != uiPrevious && !used.Contains(uiCandidate))
            {
              uiNext = uiCandidate;
              break;
            }
          }

          if (uiNext == plInvalidIndex)
            break;

          uiPrevious = uiCurrent;
          uiCurrent = uiNext;
        }

        if (loop.GetCount() < 3)
          continue;

        // the cap faces the REMOVED side
        plVec3 vLoopNormal = plVec3::MakeZero();

        for (plUInt32 j = 0; j < loop.GetCount(); ++j)
        {
          const plVec3& cur = out.m_Vertices[loop[j]];
          const plVec3& next = out.m_Vertices[loop[(j + 1) % loop.GetCount()]];

          vLoopNormal.x += (cur.y - next.y) * (cur.z + next.z);
          vLoopNormal.y += (cur.z - next.z) * (cur.x + next.x);
          vLoopNormal.z += (cur.x - next.x) * (cur.y + next.y);
        }

        const plVec3 vCapNormal = plane.m_vNormal * (bKeepPositiveSide ? -1.0f : 1.0f);

        if (vLoopNormal.Dot(vCapNormal) < 0.0f)
        {
          for (plUInt32 j = 0; j < loop.GetCount() / 2; ++j)
          {
            plMath::Swap(loop[j], loop[loop.GetCount() - 1 - j]);
          }
        }

        out.AddFace(plArrayPtr<const plUInt32>(loop.GetData(), loop.GetCount()), uiFixedCapFaceId != 0 ? uiFixedCapFaceId : ref_uiNextFaceId++, uiCapMaterialSlot);
      }
    }
  }

  out.m_uiNextFaceId = plMath::Max(out.m_uiNextFaceId, ref_uiNextFaceId);
}

void plMeshCSG::SubtractConvexPlanes(const plEditableMesh& target, plArrayPtr<const plPlane> cutterPlanes, plUInt8 uiInteriorSlot, plEditableMesh& out, plUInt32 uiCutterId, plUInt32 uiCutterShell)
{
  out.Clear();

  // no intersection at all -> unchanged
  for (const plPlane& plane : cutterPlanes)
  {
    bool bAnyInside = false;

    for (const plVec3& v : target.m_Vertices)
    {
      if (plane.GetDistanceTo(v) < -c_fPlaneEpsilon)
      {
        bAnyInside = true;
        break;
      }
    }

    if (!bAnyInside)
    {
      out = target;
      return;
    }
  }

  // with a cutter id, cut faces are keyed by (cutter, shell, plane) and stay put when cutters move or
  // reorder; without one a shared counter keeps them unique and deterministic
  plUInt32 uiNextFaceId = target.m_uiNextFaceId;

  plEditableMesh current = target;
  plEditableMesh piece, inside;

  for (plUInt32 uiPlane = 0; uiPlane < cutterPlanes.GetCount(); ++uiPlane)
  {
    const plPlane& plane = cutterPlanes[uiPlane];

    if (current.m_Faces.IsEmpty())
      break;

    const plUInt32 uiOutsideCapId = uiCutterId != 0 ? plBrushFaceIds::MakeCutFaceId(uiCutterId, uiCutterShell, uiPlane, true) : 0;
    const plUInt32 uiInsideCapId = uiCutterId != 0 ? plBrushFaceIds::MakeCutFaceId(uiCutterId, uiCutterShell, uiPlane, false) : 0;

    // the piece fully outside this plane (and inside all previous ones) survives
    ClipByPlane(current, plane, true, uiInteriorSlot, uiNextFaceId, piece, uiOutsideCapId);

    if (!piece.m_Faces.IsEmpty())
    {
      out.AppendMesh(piece);
    }

    ClipByPlane(current, plane, false, uiInteriorSlot, uiNextFaceId, inside, uiInsideCapId);
    current = inside;
  }

  // `current` is now the part inside the whole cutter — the removed volume

  out.m_uiNextFaceId = plMath::Max(out.m_uiNextFaceId, uiNextFaceId);
}

plResult plMeshCSG::SubtractCutter(const plEditableMesh& target, const plEditableMesh& cutter, const plTransform& cutterToTarget, plUInt8 uiInteriorSlot, plEditableMesh& out, plUInt32 uiCutterId)
{
  // the cutter's shells, transformed into target space
  plEditableMesh transformedCutter = cutter;

  for (plVec3& v : transformedCutter.m_Vertices)
  {
    v = cutterToTarget.TransformPosition(v);
  }

  for (plUInt32 uiShell = 0; uiShell < transformedCutter.m_Shells.GetCount(); ++uiShell)
  {
    // the contract: every cutter shell must be convex — refuse instead of producing garbage
    if (!IsShellConvex(transformedCutter, transformedCutter.m_Shells[uiShell]))
    {
      out = target;
      return PL_FAILURE;
    }
  }

  out = target;

  plDynamicArray<plPlane> planes;
  plEditableMesh result;

  for (plUInt32 uiShell = 0; uiShell < transformedCutter.m_Shells.GetCount(); ++uiShell)
  {
    if (out.m_Faces.IsEmpty())
      break;

    GetShellPlanes(transformedCutter, transformedCutter.m_Shells[uiShell], planes);

    if (planes.IsEmpty())
      continue;

    SubtractConvexPlanes(out, planes.GetArrayPtr(), uiInteriorSlot, result, uiCutterId, uiShell);
    out = result;
  }

  return PL_SUCCESS;
}

plResult plMeshCSG::MakeHollow(const plEditableMesh& mesh, float fThickness, plUInt8 uiInteriorSlot, plEditableMesh& out)
{
  out.Clear();

  if (fThickness <= 0.0f)
    return PL_FAILURE;

  bool bAnyHollowed = false;

  plEditableMesh shellMesh, hollowed;
  plDynamicArray<plPlane> planes;

  for (plUInt32 uiShell = 0; uiShell < mesh.m_Shells.GetCount(); ++uiShell)
  {
    mesh.ExtractShell(uiShell, shellMesh);

    bool bCanHollow = IsShellConvex(shellMesh, shellMesh.m_Shells[0]);

    if (bCanHollow)
    {
      GetShellPlanes(shellMesh, shellMesh.m_Shells[0], planes);

      // planes moved inward by the wall thickness form the removed inner volume
      const plBoundingBox bounds = shellMesh.ComputeBounds();
      const plVec3 vCenter = bounds.GetCenter();

      for (plPlane& plane : planes)
      {
        plane = plPlane::MakeFromNormalAndPoint(plane.m_vNormal, plane.ProjectOntoPlane(vCenter) - plane.m_vNormal * fThickness);
      }

      // a wall thicker than the shell leaves no inner volume — keep the shell solid then
      for (const plPlane& plane : planes)
      {
        if (plane.GetDistanceTo(vCenter) >= -c_fPlaneEpsilon)
        {
          bCanHollow = false;
          break;
        }
      }
    }

    if (!bCanHollow)
    {
      out.AppendMesh(shellMesh);
      continue;
    }

    SubtractConvexPlanes(shellMesh, planes.GetArrayPtr(), uiInteriorSlot, hollowed);

    if (hollowed.m_Faces.IsEmpty())
    {
      out.AppendMesh(shellMesh);
      continue;
    }

    out.AppendMesh(hollowed);
    bAnyHollowed = true;
  }

  return bAnyHollowed ? PL_SUCCESS : PL_FAILURE;
}

plUInt32 plMeshCSG::DecomposeConvex(const plEditableMesh& mesh, plEditableMesh& out, plUInt32 uiMaxPieces)
{
  out.Clear();
  out.m_uiNextFaceId = mesh.m_uiNextFaceId;

  plUInt32 uiNextFaceId = mesh.m_uiNextFaceId;
  plUInt32 uiPieces = 0;

  plDynamicArray<plEditableMesh> queue;

  for (plUInt32 uiShell = 0; uiShell < mesh.m_Shells.GetCount(); ++uiShell)
  {
    mesh.ExtractShell(uiShell, queue.ExpandAndGetRef());
  }

  plDynamicArray<plPlane> planes;
  plEditableMesh front, back, single;

  while (!queue.IsEmpty())
  {
    plEditableMesh piece = std::move(queue.PeekBack());
    queue.PopBack();

    if (piece.m_Shells.IsEmpty() || piece.m_Faces.IsEmpty())
      continue;

    if (IsShellConvex(piece, piece.m_Shells[0]) || uiPieces + queue.GetCount() + 1 >= uiMaxPieces)
    {
      piece.m_Shells[0].m_bConvex = IsShellConvex(piece, piece.m_Shells[0]);
      out.AppendMesh(piece);
      ++uiPieces;
      continue;
    }

    // split along the face plane that divides the piece most evenly
    GetShellPlanes(piece, piece.m_Shells[0], planes);

    const plPlane* pBest = nullptr;
    plInt32 iBestScore = plMath::MaxValue<plInt32>();

    for (const plPlane& plane : planes)
    {
      plInt32 iFront = 0, iBack = 0;

      for (const plVec3& v : piece.m_Vertices)
      {
        const float fDist = plane.GetDistanceTo(v);

        if (fDist > 0.01f)
          ++iFront;
        else if (fDist < -0.01f)
          ++iBack;
      }

      if (iFront == 0 || iBack == 0)
        continue;

      const plInt32 iScore = plMath::Abs(iFront - iBack);

      if (iScore < iBestScore)
      {
        iBestScore = iScore;
        pBest = &plane;
      }
    }

    if (pBest == nullptr)
    {
      out.AppendMesh(piece);
      ++uiPieces;
      continue;
    }

    const plPlane splitPlane = *pBest;

    ClipByPlane(piece, splitPlane, true, 0, uiNextFaceId, front);
    ClipByPlane(piece, splitPlane, false, 0, uiNextFaceId, back);

    // the new caps sit between pieces: they close each piece but lie inside the original solid
    for (plEditableMesh* pHalf : {&front, &back})
    {
      for (plBrushFace& face : pHalf->m_Faces)
      {
        if (face.m_uiFaceId >= piece.m_uiNextFaceId && !plBrushFaceIds::IsDerived(face.m_uiFaceId))
        {
          face.m_uiFlags |= plBrushFaceFlags::Internal;
        }
      }

      for (plUInt32 uiShell = 0; uiShell < pHalf->m_Shells.GetCount(); ++uiShell)
      {
        pHalf->ExtractShell(uiShell, single);
        single.m_uiNextFaceId = uiNextFaceId;
        queue.PushBack(single);
      }
    }
  }

  out.m_uiNextFaceId = plMath::Max(out.m_uiNextFaceId, uiNextFaceId);
  return uiPieces;
}

plResult plMeshCSG::MakeConvexCutter(const plEditableMesh& cutter, plEditableMesh& out)
{
  if (AreAllShellsConvex(cutter))
  {
    out = cutter;
    return PL_SUCCESS;
  }

  if (cutter.m_Shells.IsEmpty())
  {
    out = cutter;
    return PL_FAILURE;
  }

  // a vertex edit bends faces, and a piece holding a bent face is never convex: split those into their
  // triangles first, so every piece is bounded by flat faces
  plEditableMesh flat;
  flat.m_uiNextFaceId = cutter.m_uiNextFaceId;

  plDynamicArray<plUInt32> tris;

  // same vertex indices as the cutter
  for (const plVec3& v : cutter.m_Vertices)
  {
    flat.AddVertex(v);
  }

  for (plUInt32 uiShell = 0; uiShell < cutter.m_Shells.GetCount(); ++uiShell)
  {
    const plBrushShell& shell = cutter.m_Shells[uiShell];
    flat.BeginShell(false, shell.m_bSubtractive);

    for (plUInt32 uiFace = shell.m_uiFirstFace; uiFace < shell.m_uiFirstFace + shell.m_uiNumFaces; ++uiFace)
    {
      const plBrushFace& face = cutter.m_Faces[uiFace];

      if (face.m_Corners.GetCount() == 3 || cutter.ComputeFacePlanarityError(uiFace) <= c_fPlanarityEpsilon)
      {
        const plUInt32 uiNew = flat.AddFace(plArrayPtr<const plUInt32>(face.m_Corners.GetData(), face.m_Corners.GetCount()), face.m_uiFaceId, face.m_uiMaterialSlot);
        flat.m_Faces[uiNew].m_uiFlags = face.m_uiFlags;
        continue;
      }

      cutter.TriangulateFace(uiFace, tris);

      for (plUInt32 t = 0; t + 2 < tris.GetCount(); t += 3)
      {
        const plUInt32 corners[3] = {face.m_Corners[tris[t]], face.m_Corners[tris[t + 1]], face.m_Corners[tris[t + 2]]};
        const plUInt32 uiNew = flat.AddFace(plArrayPtr<const plUInt32>(corners), face.m_uiFaceId, face.m_uiMaterialSlot);
        flat.m_Faces[uiNew].m_uiFlags = face.m_uiFlags;
      }
    }
  }

  if (AreAllShellsConvex(flat))
  {
    out = flat;
    return PL_SUCCESS;
  }

  DecomposeConvex(flat, out);
  return AreAllShellsConvex(out) ? PL_SUCCESS : PL_FAILURE;
}

bool plMeshCSG::AreAllShellsConvex(const plEditableMesh& mesh)
{
  for (const plBrushShell& shell : mesh.m_Shells)
  {
    if (!IsShellConvex(mesh, shell))
      return false;
  }

  return !mesh.m_Shells.IsEmpty();
}

plResult plMeshCSG::Union(const plEditableMesh& a, const plEditableMesh& b, const plTransform& bToA, plEditableMesh& out)
{
  plEditableMesh bInA = b;

  for (plVec3& v : bInA.m_Vertices)
  {
    v = bToA.TransformPosition(v);
  }

  // A + (B - A): the convex operand cuts the other, so nothing is covered twice
  const bool bAConvex = AreAllShellsConvex(a);
  const bool bBConvex = AreAllShellsConvex(bInA);

  if (!bAConvex && !bBConvex)
  {
    out = a;
    return PL_FAILURE;
  }

  // A keeps its face ids (its per-face materials stay put); B's get renumbered past everything in use
  auto renumber = [](plEditableMesh& ref_mesh, plUInt32 uiOffset)
  {
    for (plBrushFace& face : ref_mesh.m_Faces)
    {
      if (!plBrushFaceIds::IsDerived(face.m_uiFaceId))
        face.m_uiFaceId += uiOffset;
    }

    ref_mesh.m_uiNextFaceId += uiOffset;
  };

  auto markCutFacesInternal = [](const plEditableMesh& before, plEditableMesh& ref_after)
  {
    plHashSet<plUInt32> ids;
    for (const plBrushFace& face : before.m_Faces)
    {
      ids.Insert(face.m_uiFaceId);
    }

    // the cut faces lie against the cutter's surface, inside the combined solid
    for (plBrushFace& face : ref_after.m_Faces)
    {
      if (!ids.Contains(face.m_uiFaceId))
        face.m_uiFlags |= plBrushFaceFlags::Internal;
    }
  };

  plEditableMesh remainder;

  if (bAConvex)
  {
    renumber(bInA, a.m_uiNextFaceId);

    if (SubtractCutter(bInA, a, plTransform::MakeIdentity(), 0, remainder).Failed())
    {
      out = a;
      return PL_FAILURE;
    }

    markCutFacesInternal(bInA, remainder);

    out = a;
    out.AppendMesh(remainder);
  }
  else
  {
    if (SubtractCutter(a, bInA, plTransform::MakeIdentity(), 0, remainder).Failed())
    {
      out = a;
      return PL_FAILURE;
    }

    markCutFacesInternal(a, remainder);
    renumber(bInA, remainder.m_uiNextFaceId);

    out = remainder;
    out.AppendMesh(bInA);
  }

  return PL_SUCCESS;
}

//////////////////////////////////////////////////////////////////////////
// rooms

namespace
{
  /// A face fragment during a room carve: positions in order, UVs only for explicit-UV faces.
  struct Polygon
  {
    plHybridArray<plVec3, 8> m_Points;
    plHybridArray<plVec2, 8> m_UVs;
  };

  float ComputePolygonArea(const Polygon& poly)
  {
    plVec3 vNormal = plVec3::MakeZero();

    for (plUInt32 i = 0; i < poly.m_Points.GetCount(); ++i)
    {
      vNormal += poly.m_Points[i].CrossRH(poly.m_Points[(i + 1) % poly.m_Points.GetCount()]);
    }

    return vNormal.GetLength() * 0.5f;
  }

  bool IsLexicographicallyLess(const plVec3& a, const plVec3& b)
  {
    if (a.x != b.x)
      return a.x < b.x;
    if (a.y != b.y)
      return a.y < b.y;
    return a.z < b.z;
  }

  /// Splits a polygon by a plane. Both sides of an edge get the bit-identical crossing point no matter
  /// which face the edge belongs to: the parameter is computed from the lexicographically lower end.
  void SplitPolygon(const Polygon& in, const plHybridArray<float, 8>& dist, Polygon& out_front, Polygon& out_back)
  {
    out_front.m_Points.Clear();
    out_front.m_UVs.Clear();
    out_back.m_Points.Clear();
    out_back.m_UVs.Clear();

    const bool bUVs = in.m_UVs.GetCount() == in.m_Points.GetCount();
    const plUInt32 n = in.m_Points.GetCount();

    for (plUInt32 i = 0; i < n; ++i)
    {
      const plUInt32 j = (i + 1) % n;
      const float d0 = dist[i];
      const float d1 = dist[j];

      if (d0 >= 0.0f)
      {
        out_front.m_Points.PushBack(in.m_Points[i]);
        if (bUVs)
          out_front.m_UVs.PushBack(in.m_UVs[i]);
      }

      if (d0 <= 0.0f)
      {
        out_back.m_Points.PushBack(in.m_Points[i]);
        if (bUVs)
          out_back.m_UVs.PushBack(in.m_UVs[i]);
      }

      if ((d0 > 0.0f && d1 < 0.0f) || (d0 < 0.0f && d1 > 0.0f))
      {
        const bool bSwap = IsLexicographicallyLess(in.m_Points[j], in.m_Points[i]);
        const plUInt32 a = bSwap ? j : i;
        const plUInt32 b = bSwap ? i : j;
        const float fT = dist[a] / (dist[a] - dist[b]);

        const plVec3 vCross = plMath::Lerp(in.m_Points[a], in.m_Points[b], fT);
        out_front.m_Points.PushBack(vCross);
        out_back.m_Points.PushBack(vCross);

        if (bUVs)
        {
          const plVec2 vUV = plMath::Lerp(in.m_UVs[a], in.m_UVs[b], fT);
          out_front.m_UVs.PushBack(vUV);
          out_back.m_UVs.PushBack(vUV);
        }
      }
    }

  }

  /// How a fragment lying in one of the volume's planes counts: adjacent (its volume touches this one
  /// from outside) or overlapping (both volumes lie behind the plane).
  struct CoplanarRule
  {
    bool m_bAdjacentInside = true;
    bool m_bOverlappingInside = false;
  };

  /// Splits a fragment into the parts outside a convex volume (appended to ref_outside) and the part
  /// inside it (returned in out_pInside, if given). vNormal is the outward normal of the fragment's own
  /// volume, used only for fragments lying in one of the planes.
  void CarveByVolume(const Polygon& poly, const plVec3& vNormal, const plCsgConvexVolume& volume, const CoplanarRule& rule, plDynamicArray<Polygon>& ref_outside, Polygon* out_pInside = nullptr)
  {
    Polygon remaining = poly;
    Polygon front, back;
    plHybridArray<float, 8> dist;

    for (const plPlane& plane : volume.m_Planes)
    {
      dist.SetCount(remaining.m_Points.GetCount());

      bool bAnyFront = false;
      bool bAnyBack = false;

      for (plUInt32 i = 0; i < remaining.m_Points.GetCount(); ++i)
      {
        float d = plane.GetDistanceTo(remaining.m_Points[i]);

        if (plMath::Abs(d) <= c_fPlaneEpsilon)
          d = 0.0f;

        dist[i] = d;
        bAnyFront |= d > 0.0f;
        bAnyBack |= d < 0.0f;
      }

      if (!bAnyFront && !bAnyBack)
      {
        const bool bAdjacent = vNormal.Dot(plane.m_vNormal) < 0.0f;
        const bool bInside = bAdjacent ? rule.m_bAdjacentInside : rule.m_bOverlappingInside;

        if (!bInside)
        {
          ref_outside.PushBack(remaining);
          return;
        }

        continue;
      }

      if (!bAnyBack)
      {
        ref_outside.PushBack(remaining);
        return;
      }

      if (!bAnyFront)
        continue;

      SplitPolygon(remaining, dist, front, back);

      if (front.m_Points.GetCount() >= 3 && ComputePolygonArea(front) > 1e-8f)
        ref_outside.PushBack(front);

      remaining = back;

      if (remaining.m_Points.GetCount() < 3)
        return;
    }

    if (out_pInside)
      *out_pInside = remaining;
  }

  /// Carves every fragment by a volume, keeping the outside parts.
  void CarveAllByVolume(plDynamicArray<Polygon>& ref_fragments, const plVec3& vNormal, const plCsgConvexVolume& volume, const CoplanarRule& rule)
  {
    plDynamicArray<Polygon> outside;

    for (const Polygon& poly : ref_fragments)
    {
      CarveByVolume(poly, vNormal, volume, rule, outside);
    }

    ref_fragments.Swap(outside);
  }

  Polygon MakePolygon(const plEditableMesh& mesh, const plBrushFace& face)
  {
    Polygon poly;

    for (plUInt32 uiCorner : face.m_Corners)
    {
      poly.m_Points.PushBack(mesh.m_Vertices[uiCorner]);
    }

    if ((face.m_uiFlags & plBrushFaceFlags::ExplicitUV) != 0 && face.m_CornerUVs.GetCount() == face.m_Corners.GetCount())
    {
      for (const plVec2& uv : face.m_CornerUVs)
      {
        poly.m_UVs.PushBack(uv);
      }
    }

    return poly;
  }

  /// Whether any face of `a` has a part of positive area strictly inside `b`.
  bool HasFaceInside(const plCsgConvexVolume& a, const plCsgConvexVolume& b)
  {
    const CoplanarRule touching = {false, false};
    plDynamicArray<Polygon> outside;

    for (plUInt32 uiFace = 0; uiFace < a.m_Mesh.m_Faces.GetCount(); ++uiFace)
    {
      Polygon inside;
      outside.Clear();
      CarveByVolume(MakePolygon(a.m_Mesh, a.m_Mesh.m_Faces[uiFace]), a.m_Mesh.ComputeFaceNormal(uiFace), b, touching, outside, &inside);

      if (inside.m_Points.GetCount() >= 3 && ComputePolygonArea(inside) > 1e-6f)
        return true;
    }

    return false;
  }

  /// Two convex volumes share space when a face of one reaches into the other (which also covers one
  /// containing the other entirely).
  bool VolumesOverlap(const plCsgConvexVolume& a, const plCsgConvexVolume& b)
  {
    return HasFaceInside(a, b) || HasFaceInside(b, a);
  }

  /// Writes fragments into a mesh, welding corners by position.
  class FragmentWriter
  {
  public:
    explicit FragmentWriter(plEditableMesh& ref_mesh)
      : m_Mesh(ref_mesh)
    {
    }

    void Write(const Polygon& poly, bool bReverse, plUInt32 uiFaceId, plUInt8 uiSlot, plUInt8 uiFlags, const plBrushFaceUV& uv)
    {
      plSmallArray<plUInt32, 8> corners;
      plSmallArray<plVec2, 8> uvs;

      const plUInt32 n = poly.m_Points.GetCount();

      for (plUInt32 i = 0; i < n; ++i)
      {
        const plUInt32 uiSource = bReverse ? (n - 1 - i) : i;
        const plUInt32 uiVertex = GetVertex(poly.m_Points[uiSource]);

        // welding can merge corners closer than the weld grid
        if (!corners.IsEmpty() && (corners.PeekBack() == uiVertex || (i + 1 == n && corners[0] == uiVertex)))
          continue;

        corners.PushBack(uiVertex);

        if (poly.m_UVs.GetCount() == n)
          uvs.PushBack(poly.m_UVs[uiSource]);
      }

      if (corners.GetCount() < 3)
        return;

      const plUInt32 uiFace = m_Mesh.AddFace(plArrayPtr<const plUInt32>(corners.GetData(), corners.GetCount()), uiFaceId, uiSlot);
      m_Mesh.m_Faces[uiFace].m_UV = uv;
      m_Mesh.m_Faces[uiFace].m_uiFlags = uiFlags;

      if (uvs.GetCount() == corners.GetCount())
        m_Mesh.m_Faces[uiFace].m_CornerUVs = uvs;
      else
        m_Mesh.m_Faces[uiFace].m_uiFlags &= ~plBrushFaceFlags::ExplicitUV;
    }

  private:
    plUInt32 GetVertex(const plVec3& vPos)
    {
      const plUInt64 uiKey = MakePositionKey(vPos);
      plUInt32 uiVertex = 0;

      if (!m_Welded.TryGetValue(uiKey, uiVertex))
      {
        uiVertex = m_Mesh.AddVertex(vPos);
        m_Welded.Insert(uiKey, uiVertex);
      }

      return uiVertex;
    }

    plEditableMesh& m_Mesh;
    plHashTable<plUInt64, plUInt32> m_Welded;
  };

  /// The index of the plane a face lies in, matching how the solid carve numbers its cut faces.
  plUInt32 FindPlaneIndex(const plCsgConvexVolume& volume, const plVec3& vNormal, const plVec3& vPoint)
  {
    for (plUInt32 i = 0; i < volume.m_Planes.GetCount(); ++i)
    {
      if (volume.m_Planes[i].m_vNormal.Dot(vNormal) > 0.999f && plMath::Abs(volume.m_Planes[i].GetDistanceTo(vPoint)) < 0.001f)
        return i;
    }

    return 0;
  }
} // namespace

void plCsgConvexVolume::SetShell(const plEditableMesh& shellMesh)
{
  m_Mesh = shellMesh;

  if (m_Mesh.m_Shells.IsEmpty())
  {
    m_Planes.Clear();
    return;
  }

  plMeshCSG::GetShellPlanes(m_Mesh, m_Mesh.m_Shells[0], m_Planes);
}

bool plMeshCSG::IsShellInverted(const plEditableMesh& mesh, const plBrushShell& shell)
{
  // signed volume by the divergence theorem, relative to the shell's first vertex for precision
  if (shell.m_uiNumFaces == 0)
    return false;

  const plVec3 vOrigin = mesh.m_Vertices[mesh.m_Faces[shell.m_uiFirstFace].m_Corners[0]];
  double fVolume = 0.0;

  for (plUInt32 uiFace = shell.m_uiFirstFace; uiFace < shell.m_uiFirstFace + shell.m_uiNumFaces; ++uiFace)
  {
    const plBrushFace& face = mesh.m_Faces[uiFace];
    const plVec3 a = mesh.m_Vertices[face.m_Corners[0]] - vOrigin;

    for (plUInt32 i = 1; i + 1 < face.m_Corners.GetCount(); ++i)
    {
      const plVec3 b = mesh.m_Vertices[face.m_Corners[i]] - vOrigin;
      const plVec3 c = mesh.m_Vertices[face.m_Corners[i + 1]] - vOrigin;
      fVolume += static_cast<double>(a.Dot(b.CrossRH(c)));
    }
  }

  return fVolume < -1e-9;
}

bool plMeshCSG::HasInvertedShell(const plEditableMesh& mesh)
{
  for (const plBrushShell& shell : mesh.m_Shells)
  {
    if (IsShellInverted(mesh, shell))
      return true;
  }

  return false;
}

void plMeshCSG::MakeRoomPieces(const plEditableMesh& mesh, plUInt32 uiShell, plDynamicArray<plCsgConvexVolume>& out_pieces)
{
  out_pieces.Clear();

  // the room's empty space is the shell turned outward
  plEditableMesh space;
  mesh.ExtractShell(uiShell, space);

  for (plBrushFace& face : space.m_Faces)
  {
    const plUInt32 n = face.m_Corners.GetCount();

    for (plUInt32 i = 0; i < n / 2; ++i)
    {
      plMath::Swap(face.m_Corners[i], face.m_Corners[n - 1 - i]);
    }
  }

  if (space.m_Shells.IsEmpty())
    return;

  if (IsShellConvex(space, space.m_Shells[0]))
  {
    out_pieces.ExpandAndGetRef().SetShell(space);
    return;
  }

  plEditableMesh pieces, piece;
  DecomposeConvex(space, pieces);

  for (plUInt32 uiPiece = 0; uiPiece < pieces.m_Shells.GetCount(); ++uiPiece)
  {
    pieces.ExtractShell(uiPiece, piece);
    out_pieces.ExpandAndGetRef().SetShell(piece);
  }
}

void plMeshCSG::CarveRooms(const plEditableMesh& target, plArrayPtr<const plCsgRoomCutter> cutters, plArrayPtr<const plCsgRoom> rooms, plUInt8 uiInteriorSlot, plEditableMesh& out)
{
  out.Clear();
  out.m_uiNextFaceId = target.m_uiNextFaceId;

  // each cutter's passage belongs to the first room it overlaps
  plDynamicArray<plUInt32> owners;
  owners.SetCount(cutters.GetCount(), plInvalidIndex);

  for (plUInt32 c = 0; c < cutters.GetCount(); ++c)
  {
    for (plUInt32 r = 0; r < rooms.GetCount() && owners[c] == plInvalidIndex; ++r)
    {
      for (const plCsgConvexVolume& piece : rooms[r].m_Pieces)
      {
        if (VolumesOverlap(cutters[c].m_Volume, piece))
        {
          owners[c] = r;
          break;
        }
      }
    }
  }

  plDynamicArray<Polygon> fragments;

  // a room's wall stays where no cutter reaches; a cutter face flush against it from outside opens it
  const CoplanarRule wallRule = {true, false};

  // a passage wall goes where a room or an earlier cutter already has a face in the same plane
  const CoplanarRule passageVsRoom = {true, true};
  const CoplanarRule passageVsLaterCutter = {true, false};
  const CoplanarRule passageVsEarlierCutter = {true, true};

  for (plUInt32 uiShell = 0; uiShell < target.m_Shells.GetCount(); ++uiShell)
  {
    plUInt32 uiRoom = plInvalidIndex;

    for (plUInt32 r = 0; r < rooms.GetCount(); ++r)
    {
      if (rooms[r].m_iTargetShell == static_cast<plInt32>(uiShell))
        uiRoom = r;
    }

    if (uiRoom == plInvalidIndex)
      continue;

    const plBrushShell& shell = target.m_Shells[uiShell];
    out.BeginShell(false, shell.m_bSubtractive);

    const plUInt32 uiFirstOutFace = out.m_Faces.GetCount();
    FragmentWriter writer(out); // welds within the shell only, shells never share vertices

    for (plUInt32 uiFace = shell.m_uiFirstFace; uiFace < shell.m_uiFirstFace + shell.m_uiNumFaces; ++uiFace)
    {
      const plBrushFace& face = target.m_Faces[uiFace];

      // the room's faces point into the empty space, the volume's outward normal is the opposite
      const plVec3 vVolumeNormal = -target.ComputeFaceNormal(uiFace);

      fragments.Clear();
      fragments.PushBack(MakePolygon(target, face));

      for (const plCsgRoomCutter& cutter : cutters)
      {
        CarveAllByVolume(fragments, vVolumeNormal, cutter.m_Volume, wallRule);
      }

      for (const Polygon& poly : fragments)
      {
        writer.Write(poly, false, face.m_uiFaceId, face.m_uiMaterialSlot, face.m_uiFlags, face.m_UV);
      }
    }

    for (plUInt32 c = 0; c < cutters.GetCount(); ++c)
    {
      if (owners[c] != uiRoom)
        continue;

      const plCsgRoomCutter& cutter = cutters[c];
      const plEditableMesh& cutterMesh = cutter.m_Volume.m_Mesh;

      for (plUInt32 uiFace = 0; uiFace < cutterMesh.m_Faces.GetCount(); ++uiFace)
      {
        const plVec3 vNormal = cutterMesh.ComputeFaceNormal(uiFace);

        fragments.Clear();
        fragments.PushBack(MakePolygon(cutterMesh, cutterMesh.m_Faces[uiFace]));
        fragments[0].m_UVs.Clear();

        for (const plCsgRoom& room : rooms)
        {
          for (const plCsgConvexVolume& piece : room.m_Pieces)
          {
            CarveAllByVolume(fragments, vNormal, piece, passageVsRoom);
          }
        }

        for (plUInt32 other = 0; other < cutters.GetCount() && !fragments.IsEmpty(); ++other)
        {
          if (other != c)
            CarveAllByVolume(fragments, vNormal, cutters[other].m_Volume, other < c ? passageVsEarlierCutter : passageVsLaterCutter);
        }

        if (fragments.IsEmpty())
          continue;

        // keyed like the solid carve's faces on the same cutter plane, so jamb materials carry over
        const plUInt32 uiPlane = FindPlaneIndex(cutter.m_Volume, vNormal, cutterMesh.m_Vertices[cutterMesh.m_Faces[uiFace].m_Corners[0]]);
        const plUInt32 uiFaceId = plBrushFaceIds::MakeCutFaceId(cutter.m_uiCutterId, cutter.m_uiShell, uiPlane, true);

        for (const Polygon& poly : fragments)
        {
          // the passage is seen from inside the cutter
          writer.Write(poly, true, uiFaceId, uiInteriorSlot, plBrushFaceFlags::None, plBrushFaceUV());
        }
      }
    }

    if (out.m_Faces.GetCount() == uiFirstOutFace)
    {
      out.m_Shells.PopBack();
    }
  }
}


PL_STATICLINK_FILE(ModelingPlugin, ModelingPlugin_Modeling_Implementation_MeshCSG);
