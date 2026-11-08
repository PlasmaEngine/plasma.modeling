#include <ModelingPlugin/ModelingPluginPCH.h>

#include <ModelingPlugin/Modeling/BrushEvaluator.h>
#include <ModelingPlugin/Modeling/MeshCSG.h>

bool plBrushEvaluator::CutterLess(plUInt32 uiIdA, const plVec3& vPosA, plUInt32 uiIdB, const plVec3& vPosB)
{
  if (uiIdA != uiIdB)
    return uiIdA < uiIdB;

  if (vPosA.x != vPosB.x)
    return vPosA.x < vPosB.x;
  if (vPosA.y != vPosB.y)
    return vPosA.y < vPosB.y;
  return vPosA.z < vPosB.z;
}

void plBrushEvaluator::ExtractRoomShells(const plEditableMesh& mesh, plEditableMesh& out_rooms)
{
  out_rooms.Clear();
  out_rooms.m_uiNextFaceId = mesh.m_uiNextFaceId;

  plEditableMesh shell;

  for (plUInt32 uiShell = 0; uiShell < mesh.m_Shells.GetCount(); ++uiShell)
  {
    if (plMeshCSG::IsShellInverted(mesh, mesh.m_Shells[uiShell]))
    {
      mesh.ExtractShell(uiShell, shell);
      out_rooms.AppendMesh(shell);
    }
  }
}

namespace
{
  /// A convex cutter moved into the target's space.
  struct PreparedCutter
  {
    plEditableMesh m_Mesh;
    plUInt32 m_uiBrushId = 0;
    bool m_bReachesTarget = false;
  };

  /// The classic carve of solid shells: every cutter that reaches the target in turn, each one cutting
  /// the previous result.
  void CarveSolids(plEditableMesh& inout_mesh, plArrayPtr<const PreparedCutter> cutters)
  {
    plEditableMesh result;

    for (const PreparedCutter& cutter : cutters)
    {
      if (inout_mesh.m_Faces.IsEmpty())
        break;

      if (!cutter.m_bReachesTarget)
        continue;

      if (plMeshCSG::SubtractCutter(inout_mesh, cutter.m_Mesh, plTransform::MakeIdentity(), 0, result, cutter.m_uiBrushId).Succeeded())
      {
        inout_mesh = result;
      }
    }
  }

  /// The room shells of every brush in the shared order (brush id, then position, then shell), as convex
  /// pieces in the target's space.
  void GatherRooms(const plEditableMesh& target, const plTransform& targetGlobalTransform, const plBrushRoomContext* pRooms, plDynamicArray<plCsgRoom>& out_rooms)
  {
    struct Source
    {
      plUInt32 m_uiBrushId = 0;
      plVec3 m_vPosition;
      const plBrushRoomInput* m_pOther = nullptr; ///< nullptr for the target itself
    };

    plHybridArray<Source, 8> sources;

    {
      Source& self = sources.ExpandAndGetRef();
      self.m_uiBrushId = pRooms ? pRooms->m_uiTargetBrushId : 0;
      self.m_vPosition = targetGlobalTransform.m_vPosition;
    }

    if (pRooms)
    {
      for (const plBrushRoomInput& other : pRooms->m_OtherRooms)
      {
        Source& source = sources.ExpandAndGetRef();
        source.m_uiBrushId = other.m_uiBrushId;
        source.m_vPosition = other.m_GlobalTransform.m_vPosition;
        source.m_pOther = &other;
      }
    }

    sources.Sort([](const Source& a, const Source& b)
      { return plBrushEvaluator::CutterLess(a.m_uiBrushId, a.m_vPosition, b.m_uiBrushId, b.m_vPosition); });

    plEditableMesh transformed;

    for (const Source& source : sources)
    {
      const plEditableMesh* pMesh = &target;

      if (source.m_pOther)
      {
        transformed = source.m_pOther->m_Mesh;
        transformed.TransformVertices(plTransform::MakeLocalTransform(targetGlobalTransform, source.m_pOther->m_GlobalTransform));
        pMesh = &transformed;
      }

      for (plUInt32 uiShell = 0; uiShell < pMesh->m_Shells.GetCount(); ++uiShell)
      {
        if (!plMeshCSG::IsShellInverted(*pMesh, pMesh->m_Shells[uiShell]))
          continue;

        plCsgRoom& room = out_rooms.ExpandAndGetRef();
        room.m_iTargetShell = source.m_pOther ? -1 : static_cast<plInt32>(uiShell);
        plMeshCSG::MakeRoomPieces(*pMesh, uiShell, room.m_Pieces);
      }
    }
  }
} // namespace

void plBrushEvaluator::Carve(plEditableMesh& inout_mesh, const plTransform& targetGlobalTransform, plArrayPtr<const plBrushCutterInput> cutters,
  plDynamicArray<plUInt32>* out_pRefusedCutters, const plBrushRoomContext* pRooms)
{
  if (inout_mesh.m_Faces.IsEmpty() || cutters.IsEmpty())
    return;

  plBoundingBox targetWorldBounds = inout_mesh.ComputeBounds();
  targetWorldBounds.TransformFromOrigin(targetGlobalTransform.GetAsMat4());

  const bool bHasRooms = plMeshCSG::HasInvertedShell(inout_mesh);

  // the cutters that reach the target and can cut: a non-convex one is refused, never half applied. Rooms
  // also take the convex cutters that only reach those, since passages end where another cutter begins.
  // The list keeps the given order, which every brush shares.
  plHybridArray<PreparedCutter, 8> prepared;
  bool bAnyReachesTarget = false;

  for (const plBrushCutterInput& cutter : cutters)
  {
    if (cutter.m_Mesh.m_Faces.IsEmpty())
      continue;

    const bool bReachesTarget = !cutter.m_WorldBounds.IsValid() || cutter.m_WorldBounds.Overlaps(targetWorldBounds);

    if (!bReachesTarget && !bHasRooms)
      continue;

    plEditableMesh transformed = cutter.m_Mesh;
    transformed.TransformVertices(plTransform::MakeLocalTransform(targetGlobalTransform, cutter.m_GlobalTransform));

    PreparedCutter& p = prepared.ExpandAndGetRef();
    p.m_uiBrushId = cutter.m_uiBrushId;
    p.m_bReachesTarget = bReachesTarget;

    // an edited cutter (a moved vertex, an extruded face) is split into convex pieces that cut together
    if (plMeshCSG::MakeConvexCutter(transformed, p.m_Mesh).Failed())
    {
      if (bReachesTarget && out_pRefusedCutters)
        out_pRefusedCutters->PushBack(cutter.m_uiBrushId);

      prepared.PopBack();
      continue;
    }

    bAnyReachesTarget |= bReachesTarget;
  }

  if (!bAnyReachesTarget)
    return;

  if (!bHasRooms)
  {
    CarveSolids(inout_mesh, prepared.GetArrayPtr());
    return;
  }

  // solid shells take the classic carve, room shells the room carve; solids come first in the result
  plEditableMesh solids, shell;
  solids.m_uiNextFaceId = inout_mesh.m_uiNextFaceId;

  for (plUInt32 uiShell = 0; uiShell < inout_mesh.m_Shells.GetCount(); ++uiShell)
  {
    if (!plMeshCSG::IsShellInverted(inout_mesh, inout_mesh.m_Shells[uiShell]))
    {
      inout_mesh.ExtractShell(uiShell, shell);
      solids.AppendMesh(shell);
    }
  }

  CarveSolids(solids, prepared.GetArrayPtr());

  plDynamicArray<plCsgRoomCutter> roomCutters;

  for (const PreparedCutter& cutter : prepared)
  {
    for (plUInt32 uiShell = 0; uiShell < cutter.m_Mesh.m_Shells.GetCount(); ++uiShell)
    {
      cutter.m_Mesh.ExtractShell(uiShell, shell);

      plCsgRoomCutter& roomCutter = roomCutters.ExpandAndGetRef();
      roomCutter.m_Volume.SetShell(shell);
      roomCutter.m_uiCutterId = cutter.m_uiBrushId;
      roomCutter.m_uiShell = uiShell;
    }
  }

  plDynamicArray<plCsgRoom> rooms;
  GatherRooms(inout_mesh, targetGlobalTransform, pRooms, rooms);

  plEditableMesh carvedRooms;
  plMeshCSG::CarveRooms(inout_mesh, roomCutters, rooms, 0, carvedRooms);

  inout_mesh = solids;
  inout_mesh.AppendMesh(carvedRooms);
}

void plBrushEvaluator::Evaluate(plEditableMesh& inout_mesh, const plTransform& targetGlobalTransform, plArrayPtr<const plBrushCutterInput> cutters,
  const plBrushFaceAttributeMap& attributes, plDynamicArray<plUInt32>* out_pRefusedCutters, const plBrushRoomContext* pRooms)
{
  Carve(inout_mesh, targetGlobalTransform, cutters, out_pRefusedCutters, pRooms);

  plBrushFaceAttributeCodec::Apply(attributes, inout_mesh);
}


PL_STATICLINK_FILE(ModelingPlugin, ModelingPlugin_Modeling_Implementation_BrushEvaluator);
