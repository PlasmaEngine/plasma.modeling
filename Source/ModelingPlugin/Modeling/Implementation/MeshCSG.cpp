#include <ModelingPlugin/ModelingPluginPCH.h>

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
} // namespace

bool plMeshCSG::IsShellConvex(const plEditableMesh& mesh, const plBrushShell& shell, float fEpsilon)
{
  for (plUInt32 uiFace = shell.m_uiFirstFace; uiFace < shell.m_uiFirstFace + shell.m_uiNumFaces; ++uiFace)
  {
    const plPlane plane = MakeFacePlane(mesh, mesh.m_Faces[uiFace]);

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
    out_planes.PushBack(MakeFacePlane(mesh, mesh.m_Faces[uiFace]));
  }
}

void plMeshCSG::ClipByPlane(const plEditableMesh& in, const plPlane& plane, bool bKeepPositiveSide, plUInt8 uiCapMaterialSlot, plUInt32& ref_uiNextFaceId, plEditableMesh& out)
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

  auto mapVertex = [&](plUInt32 uiOriginal) -> plUInt32
  {
    if (vertexRemap[uiOriginal] == plInvalidIndex)
    {
      vertexRemap[uiOriginal] = out.AddVertex(in.m_Vertices[uiOriginal]);
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

      plSmallArray<plUInt32, 8> newCorners;
      plSmallArray<plUInt32, 4> onPlane; // output vertices lying on the cut, in loop order

      if (uiKept == uiCorners)
      {
        for (plUInt32 uiCorner : face.m_Corners)
        {
          newCorners.PushBack(mapVertex(uiCorner));
        }
      }
      else
      {
        // Sutherland-Hodgman against the kept halfspace
        for (plUInt32 i = 0; i < uiCorners; ++i)
        {
          const plUInt32 uiCur = face.m_Corners[i];
          const plUInt32 uiNext = face.m_Corners[(i + 1) % uiCorners];
          const bool bCurIn = dist[uiCur] >= -c_fPlaneEpsilon;
          const bool bNextIn = dist[uiNext] >= -c_fPlaneEpsilon;

          if (bCurIn)
          {
            const plUInt32 uiOut = mapVertex(uiCur);
            newCorners.PushBack(uiOut);

            if (plMath::Abs(dist[uiCur]) <= c_fPlaneEpsilon)
            {
              onPlane.PushBack(uiOut);
            }
          }

          if (bCurIn != bNextIn && plMath::Abs(dist[uiCur]) > c_fPlaneEpsilon && plMath::Abs(dist[uiNext]) > c_fPlaneEpsilon)
          {
            const plUInt32 uiCut = cutEdge(uiCur, uiNext);
            newCorners.PushBack(uiCut);
            onPlane.PushBack(uiCut);
          }
        }
      }

      if (newCorners.GetCount() < 3)
        continue;

      const plUInt32 uiNewFace = out.AddFace(plArrayPtr<const plUInt32>(newCorners.GetData(), newCorners.GetCount()), face.m_uiFaceId, face.m_uiMaterialSlot);
      out.m_Faces[uiNewFace].m_UV = face.m_UV;

      // a convex face crosses the plane in exactly one segment
      if (onPlane.GetCount() >= 2)
      {
        capSegments.PushBack(onPlane[0]);
        capSegments.PushBack(onPlane[1]);
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

        out.AddFace(plArrayPtr<const plUInt32>(loop.GetData(), loop.GetCount()), ref_uiNextFaceId++, uiCapMaterialSlot);
      }
    }
  }

  out.m_uiNextFaceId = plMath::Max(out.m_uiNextFaceId, ref_uiNextFaceId);
}

void plMeshCSG::SubtractConvexPlanes(const plEditableMesh& target, plArrayPtr<const plPlane> cutterPlanes, plUInt8 uiInteriorSlot, plEditableMesh& out)
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

  // shared cap-id counter across every clip keeps all cap face ids unique AND deterministic
  plUInt32 uiNextFaceId = target.m_uiNextFaceId;

  plEditableMesh current = target;
  plEditableMesh piece, inside;

  for (const plPlane& plane : cutterPlanes)
  {
    if (current.m_Faces.IsEmpty())
      break;

    // the piece fully outside this plane (and inside all previous ones) survives
    ClipByPlane(current, plane, true, uiInteriorSlot, uiNextFaceId, piece);

    if (!piece.m_Faces.IsEmpty())
    {
      out.AppendMesh(piece);
    }

    ClipByPlane(current, plane, false, uiInteriorSlot, uiNextFaceId, inside);
    current = inside;
  }

  // `current` is now the part inside the whole cutter — the removed volume

  out.m_uiNextFaceId = plMath::Max(out.m_uiNextFaceId, uiNextFaceId);
}

plResult plMeshCSG::SubtractCutter(const plEditableMesh& target, const plEditableMesh& cutter, const plTransform& cutterToTarget, plUInt8 uiInteriorSlot, plEditableMesh& out)
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

    SubtractConvexPlanes(out, planes.GetArrayPtr(), uiInteriorSlot, result);
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


PL_STATICLINK_FILE(ModelingPlugin, ModelingPlugin_Modeling_Implementation_MeshCSG);
