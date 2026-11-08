#include <ModelingTest/ModelingTestPCH.h>

#include <ModelingPlugin/Modeling/MeshOps.h>
#include <ModelingPlugin/Modeling/MeshTopology.h>
#include <ModelingPlugin/Modeling/MeshValidator.h>
#include <ModelingTest/TestHelpers.h>

PL_CREATE_SIMPLE_TEST_GROUP(MeshOps);

using namespace plModelingTestUtils;

namespace
{
  // face ids of MakeBox: 1 top, 2 bottom, 3 +X, 4 -X, 5 +Y, 6 -Y
  constexpr plUInt32 c_uiTop = 1;
  constexpr plUInt32 c_uiPosX = 3;

  plHashSet<plUInt32> Ids(std::initializer_list<plUInt32> ids)
  {
    plHashSet<plUInt32> set;
    for (plUInt32 id : ids)
      set.Insert(id);
    return set;
  }

  bool IsClosedManifold(const plEditableMesh& mesh)
  {
    plMeshTopology topology;
    topology.Build(mesh);
    return topology.IsClosedManifold();
  }

  plUInt32 FindVertex(const plEditableMesh& mesh, const plVec3& v)
  {
    for (plUInt32 i = 0; i < mesh.m_Vertices.GetCount(); ++i)
    {
      if (mesh.m_Vertices[i].IsEqual(v, 0.0001f))
        return i;
    }
    return plInvalidIndex;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(MeshOps, Topology)
{
  const plEditableMesh box = MakeBox(plVec3(-1), plVec3(1));

  plMeshTopology topology;
  topology.Build(box);

  PL_TEST_BOOL(topology.IsClosedManifold());
  PL_TEST_INT(topology.GetHalfEdgeCount(), 24);
  PL_TEST_INT(topology.GetBoundaryEdgeCount(), 0);

  // a ring through a top edge runs top -> side -> bottom -> side and closes
  const plUInt32 h = topology.GetFaceHalfEdge(0, 0);
  plDynamicArray<plMeshTopology::RingEdge> ring;
  plDynamicArray<plUInt32> quads;
  bool bClosed = false;
  topology.GetEdgeRing(h, ring, quads, bClosed);

  PL_TEST_BOOL(bClosed);
  PL_TEST_INT(ring.GetCount(), 4);
  PL_TEST_INT(quads.GetCount(), 4);

  // side A of every ring edge is on the same side: all A vertices share one coordinate
  const plVec3 vA0 = box.m_Vertices[ring[0].m_uiA];
  const plVec3 vB0 = box.m_Vertices[ring[0].m_uiB];
  const plVec3 vAxis = (vB0 - vA0).GetNormalized();
  for (const auto& e : ring)
  {
    PL_TEST_FLOAT((box.m_Vertices[e.m_uiA] - vA0).Dot(vAxis), 0.0f, 0.0001f);
  }
}

PL_CREATE_SIMPLE_TEST(MeshOps, Extrude)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Region extrude of one face")
  {
    plEditableMesh box = MakeBox(plVec3(-1), plVec3(1));
    PL_TEST_BOOL(plMeshOps::Extrude(box, Ids({c_uiTop}), 1.0f, true).Succeeded());

    PL_TEST_INT(box.m_Faces.GetCount(), 10);
    PL_TEST_BOOL(IsClosedManifold(box));
    PL_TEST_DOUBLE(ComputeVolume(box), 12.0, 0.0001);
    PL_TEST_BOOL(box.ComputeBounds().m_vMax.z > 1.99f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Region extrude of two adjacent faces keeps them joined")
  {
    plEditableMesh box = MakeBox(plVec3(-1), plVec3(1));
    PL_TEST_BOOL(plMeshOps::Extrude(box, Ids({c_uiTop, c_uiPosX}), 0.5f, true).Succeeded());

    PL_TEST_BOOL(IsClosedManifold(box));
    // both planes move out by 0.5: 2x2x2 grows to 2.5 (x) by 2 (y) by 2.5 (z)
    PL_TEST_DOUBLE(ComputeVolume(box), 2.5 * 2.0 * 2.5, 0.0001);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Pushing in after an inset makes a recess")
  {
    plEditableMesh box = MakeBox(plVec3(-1), plVec3(1));
    PL_TEST_BOOL(plMeshOps::Inset(box, Ids({c_uiTop}), 0.25f).Succeeded());
    PL_TEST_BOOL(IsClosedManifold(box));
    PL_TEST_DOUBLE(ComputeVolume(box), 8.0, 0.0001);
    PL_TEST_INT(box.m_Faces.GetCount(), 10);

    PL_TEST_BOOL(plMeshOps::Extrude(box, Ids({c_uiTop}), -0.5f, true).Succeeded());
    PL_TEST_BOOL(IsClosedManifold(box));
    PL_TEST_DOUBLE(ComputeVolume(box), 8.0 - 1.5 * 1.5 * 0.5, 0.0001);
  }
}

PL_CREATE_SIMPLE_TEST(MeshOps, Bevel)
{
  plEditableMesh box = MakeBox(plVec3(-1), plVec3(1));

  // the top edge on the +X side
  const plUInt32 v0 = FindVertex(box, plVec3(1, -1, 1));
  const plUInt32 v1 = FindVertex(box, plVec3(1, 1, 1));
  const plUInt64 uiEdge = plMeshTopology::MakeKey(v0, v1);

  PL_TEST_BLOCK(plTestBlock::Enabled, "Chamfer")
  {
    plEditableMesh mesh = box;
    PL_TEST_BOOL(plMeshOps::Bevel(mesh, plArrayPtr<const plUInt64>(&uiEdge, 1), 0.2f, 1).Succeeded());

    PL_TEST_BOOL(IsClosedManifold(mesh));
    PL_TEST_INT(mesh.m_Faces.GetCount(), 7);
    PL_TEST_DOUBLE(ComputeVolume(mesh), 8.0 - 0.5 * 0.2 * 0.2 * 2.0, 0.0001);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Rounded")
  {
    plEditableMesh mesh = box;
    PL_TEST_BOOL(plMeshOps::Bevel(mesh, plArrayPtr<const plUInt64>(&uiEdge, 1), 0.2f, 4).Succeeded());

    PL_TEST_BOOL(IsClosedManifold(mesh));
    const double fVolume = ComputeVolume(mesh);
    PL_TEST_BOOL(fVolume < 8.0 && fVolume > 8.0 - 0.04);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Refuses edges of an open mesh")
  {
    plEditableMesh mesh = box;
    PL_TEST_BOOL(plMeshOps::DeleteFaces(mesh, Ids({c_uiTop})).Succeeded());

    const plUInt32 a = FindVertex(mesh, plVec3(1, -1, 1));
    const plUInt32 b = FindVertex(mesh, plVec3(1, 1, 1));
    const plUInt64 uiOpenEdge = plMeshTopology::MakeKey(a, b);

    plEditableMesh before = mesh;
    PL_TEST_BOOL(plMeshOps::Bevel(mesh, plArrayPtr<const plUInt64>(&uiOpenEdge, 1), 0.2f, 1).Failed());
    PL_TEST_INT(mesh.m_Faces.GetCount(), before.m_Faces.GetCount());
  }
}

PL_CREATE_SIMPLE_TEST(MeshOps, LoopCutAndSubdivide)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Loop cut around a box")
  {
    plEditableMesh box = MakeBox(plVec3(-1), plVec3(1));
    const plUInt32 v0 = FindVertex(box, plVec3(-1, -1, 1));
    const plUInt32 v1 = FindVertex(box, plVec3(1, -1, 1));

    PL_TEST_BOOL(plMeshOps::LoopCut(box, v0, v1, 1, 0.25f).Succeeded());

    PL_TEST_INT(box.m_Faces.GetCount(), 10);
    PL_TEST_BOOL(IsClosedManifold(box));
    PL_TEST_DOUBLE(ComputeVolume(box), 8.0, 0.0001);

    // the cut sits a quarter of the way along X from v0's side
    PL_TEST_BOOL(FindVertex(box, plVec3(-0.5f, -1, 1)) != plInvalidIndex);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Three even cuts")
  {
    plEditableMesh box = MakeBox(plVec3(-1), plVec3(1));
    const plUInt32 v0 = FindVertex(box, plVec3(-1, -1, 1));
    const plUInt32 v1 = FindVertex(box, plVec3(1, -1, 1));

    PL_TEST_BOOL(plMeshOps::LoopCut(box, v0, v1, 3, 0.5f).Succeeded());
    PL_TEST_INT(box.m_Faces.GetCount(), 6 + 4 * 3);
    PL_TEST_BOOL(IsClosedManifold(box));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Subdividing one face keeps the mesh closed")
  {
    plEditableMesh box = MakeBox(plVec3(-1), plVec3(1));
    PL_TEST_BOOL(plMeshOps::Subdivide(box, Ids({c_uiTop})).Succeeded());

    PL_TEST_INT(box.m_Faces.GetCount(), 9);
    PL_TEST_BOOL(IsClosedManifold(box));
    PL_TEST_DOUBLE(ComputeVolume(box), 8.0, 0.0001);
  }
}

PL_CREATE_SIMPLE_TEST(MeshOps, Topological)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Delete and fill")
  {
    plEditableMesh box = MakeBox(plVec3(-1), plVec3(1));
    PL_TEST_BOOL(plMeshOps::DeleteFaces(box, Ids({c_uiTop})).Succeeded());

    plMeshValidationReport report;
    plMeshValidator::Validate(box, report, true);
    PL_TEST_INT(report.m_uiOpenEdges, 4);

    const plUInt32 a = FindVertex(box, plVec3(-1, -1, 1));
    const plUInt32 b = FindVertex(box, plVec3(1, -1, 1));
    PL_TEST_BOOL(plMeshOps::FillHole(box, a, b).Succeeded());

    PL_TEST_BOOL(IsClosedManifold(box));
    PL_TEST_DOUBLE(ComputeVolume(box), 8.0, 0.0001);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Flip turns the volume negative")
  {
    plEditableMesh box = MakeBox(plVec3(-1), plVec3(1));
    PL_TEST_BOOL(plMeshOps::Flip(box, Ids({1, 2, 3, 4, 5, 6})).Succeeded());
    PL_TEST_DOUBLE(ComputeVolume(box), -8.0, 0.0001);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Weld collapses an edge")
  {
    plEditableMesh box = MakeBox(plVec3(-1), plVec3(1));
    plHashSet<plUInt32> verts;
    verts.Insert(FindVertex(box, plVec3(1, -1, 1)));
    verts.Insert(FindVertex(box, plVec3(1, 1, 1)));

    PL_TEST_BOOL(plMeshOps::Weld(box, verts).Succeeded());
    PL_TEST_INT(box.m_Vertices.GetCount(), 7);
    PL_TEST_BOOL(IsClosedManifold(box));

    // the side faces bend, so the exact volume depends on their triangulation; it only shrinks
    const double fVolume = ComputeVolume(box);
    PL_TEST_BOOL(fVolume < 8.0 && fVolume > 5.5);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Connect splits a face")
  {
    plEditableMesh box = MakeBox(plVec3(-1), plVec3(1));
    PL_TEST_BOOL(plMeshOps::Connect(box, FindVertex(box, plVec3(-1, -1, 1)), FindVertex(box, plVec3(1, 1, 1))).Succeeded());
    PL_TEST_INT(box.m_Faces.GetCount(), 7);
    PL_TEST_BOOL(IsClosedManifold(box));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Bridge two boxes")
  {
    plEditableMesh mesh = MakeBox(plVec3(-3, -1, -1), plVec3(-1, 1, 1));
    plEditableMesh second = MakeBox(plVec3(1, -1, -1), plVec3(3, 1, 1));
    for (plBrushFace& face : second.m_Faces)
      face.m_uiFaceId += 10;
    mesh.AppendMesh(second);

    // the +X face of the left box and the -X face of the right box face each other
    PL_TEST_BOOL(plMeshOps::Bridge(mesh, 3, 14).Succeeded());

    PL_TEST_BOOL(IsClosedManifold(mesh));
    PL_TEST_INT(mesh.m_Shells.GetCount(), 1);
    PL_TEST_DOUBLE(ComputeVolume(mesh), 8.0 + 8.0 + 8.0, 0.0001);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Detach and triangulate")
  {
    plEditableMesh box = MakeBox(plVec3(-1), plVec3(1));
    plEditableMesh detached;
    PL_TEST_BOOL(plMeshOps::Detach(box, Ids({c_uiTop}), detached).Succeeded());
    PL_TEST_INT(box.m_Faces.GetCount(), 5);
    PL_TEST_INT(detached.m_Faces.GetCount(), 1);
    PL_TEST_INT(detached.m_Vertices.GetCount(), 4);

    plEditableMesh tri = MakeBox(plVec3(-1), plVec3(1));
    PL_TEST_BOOL(plMeshOps::Triangulate(tri, Ids({1, 2, 3, 4, 5, 6})).Succeeded());
    PL_TEST_INT(tri.m_Faces.GetCount(), 12);
    PL_TEST_BOOL(IsClosedManifold(tri));
    PL_TEST_DOUBLE(ComputeVolume(tri), 8.0, 0.0001);
  }
}

PL_CREATE_SIMPLE_TEST(MeshOps, Validator)
{
  plEditableMesh box = MakeBox(plVec3(-1), plVec3(1));

  plMeshValidationReport report;
  plMeshValidator::Validate(box, report, true);
  PL_TEST_BOOL(!report.HasProblems());
  PL_TEST_INT(report.m_uiNonConvexShells, 0);

  // split vertices (every face its own corners, as imported meshes with UV seams have): closed in space
  {
    plEditableMesh split;
    split.m_Shells = box.m_Shells;
    split.m_uiNextFaceId = box.m_uiNextFaceId;

    for (const plBrushFace& face : box.m_Faces)
    {
      plBrushFace& copy = split.m_Faces.ExpandAndGetRef();
      copy = face;
      copy.m_Corners.Clear();

      for (plUInt32 uiCorner : face.m_Corners)
      {
        copy.m_Corners.PushBack(split.m_Vertices.GetCount());
        split.m_Vertices.PushBack(box.m_Vertices[uiCorner]);
      }
    }

    plMeshValidator::Validate(split, report, false);
    PL_TEST_INT(report.m_uiOpenEdges, 0);
  }

  // bend a face
  box.m_Vertices[box.m_Faces[0].m_Corners[0]].z += 0.3f;
  plMeshValidator::Validate(box, report, true);
  PL_TEST_BOOL(report.m_uiNonPlanarFaces >= 1);
  PL_TEST_BOOL(report.HasProblems());

  PL_TEST_BOOL(plMeshOps::Triangulate(box, Ids({1, 2, 3, 4, 5, 6})).Succeeded());
  plMeshValidator::Validate(box, report, true);
  PL_TEST_INT(report.m_uiNonPlanarFaces, 0);
}

PL_CREATE_SIMPLE_TEST(MeshOps, Surfaces)
{
  // a wall front (z = 0 plane, facing +Z) built from CSG-like pieces in separate shells:
  // A spans x 0..4 below, B and C sit on top of A, touching it only at T-junctions (no shared corner
  // with A's top edge ends), D is coplanar but far away, E touches A but uses another material,
  // F shares A's corner but lies in another plane
  plEditableMesh mesh;
  auto quad = [&](plVec3 a, plVec3 b, plVec3 c, plVec3 d, plUInt32 uiId, plUInt8 uiSlot = 0)
  {
    mesh.BeginShell(false);
    const plVec3 corners[4] = {a, b, c, d};
    return mesh.AddFace(plArrayPtr<const plVec3>(corners, 4), uiId, uiSlot);
  };

  const plUInt32 a = quad(plVec3(0, 0, 0), plVec3(4, 0, 0), plVec3(4, 1, 0), plVec3(0, 1, 0), 10);
  const plUInt32 b = quad(plVec3(1, 1, 0), plVec3(2, 1, 0), plVec3(2, 2, 0), plVec3(1, 2, 0), 11);
  const plUInt32 c = quad(plVec3(2, 1, 0), plVec3(3, 1, 0), plVec3(3, 2, 0), plVec3(2, 2, 0), 12);
  const plUInt32 d = quad(plVec3(10, 0, 0), plVec3(11, 0, 0), plVec3(11, 1, 0), plVec3(10, 1, 0), 13);
  const plUInt32 e = quad(plVec3(4, 0, 0), plVec3(5, 0, 0), plVec3(5, 1, 0), plVec3(4, 1, 0), 14, 1);
  const plUInt32 f = quad(plVec3(0, 0, 0), plVec3(0, 1, 0), plVec3(0, 1, -1), plVec3(0, 0, -1), 15);

  // a piece far away that carries D's id belongs with D
  const plUInt32 g = quad(plVec3(20, 0, 0), plVec3(21, 0, 0), plVec3(21, 1, 0), plVec3(20, 1, 0), 13);

  plDynamicArray<plUInt32> surfaceOf;
  const plUInt32 uiCount = plMeshOps::ComputeSurfaces(mesh, surfaceOf);

  PL_TEST_INT(uiCount, 4);
  PL_TEST_INT(surfaceOf[a], surfaceOf[b]);
  PL_TEST_INT(surfaceOf[a], surfaceOf[c]);
  PL_TEST_BOOL(surfaceOf[d] != surfaceOf[a]);
  PL_TEST_INT(surfaceOf[d], surfaceOf[g]);
  PL_TEST_BOOL(surfaceOf[e] != surfaceOf[a]);
  PL_TEST_BOOL(surfaceOf[f] != surfaceOf[a]);
}
