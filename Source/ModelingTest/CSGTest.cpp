#include <ModelingTest/ModelingTestPCH.h>

#include <Foundation/Math/Random.h>
#include <ModelingPlugin/Modeling/BrushEvaluator.h>
#include <ModelingPlugin/Modeling/BrushGenerators.h>
#include <ModelingPlugin/Modeling/MeshCSG.h>
#include <ModelingTest/TestHelpers.h>

PL_CREATE_SIMPLE_TEST_GROUP(CSG);

using namespace plModelingTestUtils;

namespace
{
  plBrushCutterInput MakeCutter(const plVec3& vMin, const plVec3& vMax, plUInt32 uiId, const plVec3& vPosition = plVec3::MakeZero())
  {
    plBrushCutterInput cutter;
    cutter.m_Mesh = MakeBox(vMin, vMax);
    cutter.m_GlobalTransform = plTransform(vPosition);
    cutter.m_uiBrushId = uiId;
    cutter.m_WorldBounds = cutter.m_Mesh.ComputeBounds();
    cutter.m_WorldBounds.Translate(vPosition);
    return cutter;
  }

  plHashSet<plUInt32> CollectIds(const plEditableMesh& mesh)
  {
    plHashSet<plUInt32> ids;
    for (const plBrushFace& face : mesh.m_Faces)
      ids.Insert(face.m_uiFaceId);
    return ids;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(CSG, Subtract)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "A doorway removes exactly its volume")
  {
    // a 4 x 0.5 x 3 wall with a 1 x (through) x 2 doorway
    const plEditableMesh wall = MakeBox(plVec3(-2, -0.25f, 0), plVec3(2, 0.25f, 3));
    const plEditableMesh door = MakeBox(plVec3(-0.5f, -1, 0), plVec3(0.5f, 1, 2));

    plEditableMesh result;
    PL_TEST_BOOL(plMeshCSG::SubtractCutter(wall, door, plTransform::MakeIdentity(), 0, result, 42).Succeeded());

    PL_TEST_DOUBLE(ComputeVolume(result), 4.0 * 0.5 * 3.0 - 1.0 * 0.5 * 2.0, 0.0001);
    PL_TEST_BOOL(result.IsStructurallyValid());

    // the jambs and the lintel underside are cut faces keyed by the cutter
    plUInt32 uiCutFaces = 0;
    for (const plBrushFace& face : result.m_Faces)
    {
      if (plBrushFaceIds::IsDerived(face.m_uiFaceId))
        ++uiCutFaces;
    }
    PL_TEST_BOOL(uiCutFaces >= 3);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "A non-convex cutter is refused and changes nothing")
  {
    const plEditableMesh wall = MakeBox(plVec3(-2, -0.25f, 0), plVec3(2, 0.25f, 3));

    plEditableMesh lShape = MakeBox(plVec3(-0.5f, -1, 0), plVec3(0.5f, 1, 2));
    // pull one top corner in: the shell is no longer convex
    lShape.m_Vertices[6] = plVec3(0.0f, 0.0f, 1.0f);

    plEditableMesh result;
    PL_TEST_BOOL(plMeshCSG::SubtractCutter(wall, lShape, plTransform::MakeIdentity(), 0, result, 1).Failed());
    PL_TEST_DOUBLE(ComputeVolume(result), ComputeVolume(wall), 0.0001);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Hollow leaves walls of the given thickness")
  {
    const plEditableMesh room = MakeBox(plVec3(-2, -2, 0), plVec3(2, 2, 3));

    plEditableMesh result;
    PL_TEST_BOOL(plMeshCSG::MakeHollow(room, 0.25f, 0, result).Succeeded());

    const double fInner = 3.5 * 3.5 * 2.5;
    PL_TEST_DOUBLE(ComputeVolume(result), 4.0 * 4.0 * 3.0 - fInner, 0.001);
  }
}

PL_CREATE_SIMPLE_TEST(CSG, RandomCarves)
{
  // repeated carves by disjoint random boxes: the result volume must match the analytic overlap
  plRandom rnd;
  rnd.Initialize(1234);

  plBrushFaceAttributeMap noAttributes;

  for (plUInt32 uiRound = 0; uiRound < 50; ++uiRound)
  {
    const plVec3 vTargetMin(-4, -1, 0);
    const plVec3 vTargetMax(4, 1, 3);
    plEditableMesh mesh = MakeBox(vTargetMin, vTargetMax);

    double fExpected = 8.0 * 2.0 * 3.0;

    plHybridArray<plBrushCutterInput, 4> cutters;

    for (plUInt32 uiCutter = 0; uiCutter < 4; ++uiCutter)
    {
      // each cutter owns an X slot of 2 m, so cutters never overlap each other
      const float fSlot = -4.0f + uiCutter * 2.0f;
      const plVec3 vMin(fSlot + static_cast<float>(rnd.DoubleMinMax(0.0, 0.8)), static_cast<float>(rnd.DoubleMinMax(-2.0, 0.5)), static_cast<float>(rnd.DoubleMinMax(-1.0, 2.0)));
      const plVec3 vMax(fSlot + static_cast<float>(rnd.DoubleMinMax(1.0, 2.0)), vMin.y + static_cast<float>(rnd.DoubleMinMax(0.1, 3.0)), vMin.z + static_cast<float>(rnd.DoubleMinMax(0.1, 3.0)));

      plBrushCutterInput& c = cutters.ExpandAndGetRef();
      c.m_Mesh = MakeBox(vMin, vMax);
      c.m_uiBrushId = 1000 + uiCutter;
      c.m_WorldBounds = c.m_Mesh.ComputeBounds();

      const double dx = plMath::Max(0.0, static_cast<double>(plMath::Min(vMax.x, vTargetMax.x) - plMath::Max(vMin.x, vTargetMin.x)));
      const double dy = plMath::Max(0.0, static_cast<double>(plMath::Min(vMax.y, vTargetMax.y) - plMath::Max(vMin.y, vTargetMin.y)));
      const double dz = plMath::Max(0.0, static_cast<double>(plMath::Min(vMax.z, vTargetMax.z) - plMath::Max(vMin.z, vTargetMin.z)));
      fExpected -= dx * dy * dz;
    }

    plBrushEvaluator::Evaluate(mesh, plTransform::MakeIdentity(), cutters, noAttributes);

    PL_TEST_BOOL(mesh.IsStructurallyValid());
    PL_TEST_DOUBLE_MSG(ComputeVolume(mesh), fExpected, 0.001, "round %u", uiRound);
  }
}

PL_CREATE_SIMPLE_TEST(CSG, CutFaceIdsAreStable)
{
  const plEditableMesh wall = MakeBox(plVec3(-4, -0.25f, 0), plVec3(4, 0.25f, 3));

  plBrushFaceAttributeMap noAttributes;

  PL_TEST_BLOCK(plTestBlock::Enabled, "Moving a cutter keeps its cut-face ids")
  {
    plBrushCutterInput a = MakeCutter(plVec3(-0.5f, -1, 0), plVec3(0.5f, 1, 2), 100, plVec3(-2, 0, 0));
    plBrushCutterInput a2 = MakeCutter(plVec3(-0.5f, -1, 0), plVec3(0.5f, 1, 2), 100, plVec3(-1.5f, 0, 0));

    plEditableMesh r1 = wall, r2 = wall;
    plBrushEvaluator::Evaluate(r1, plTransform::MakeIdentity(), plArrayPtr<const plBrushCutterInput>(&a, 1), noAttributes);
    plBrushEvaluator::Evaluate(r2, plTransform::MakeIdentity(), plArrayPtr<const plBrushCutterInput>(&a2, 1), noAttributes);

    const plHashSet<plUInt32> ids1 = CollectIds(r1);
    const plHashSet<plUInt32> ids2 = CollectIds(r2);

    PL_TEST_INT(ids1.GetCount(), ids2.GetCount());
    for (plUInt32 id : ids1)
    {
      PL_TEST_BOOL(ids2.Contains(id));
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Cutters passing each other keep their faces")
  {
    // two doorways; swapping their positions must not swap which cutter owns which cut faces
    plBrushCutterInput left = MakeCutter(plVec3(-0.5f, -1, 0), plVec3(0.5f, 1, 2), 7, plVec3(-2, 0, 0));
    plBrushCutterInput right = MakeCutter(plVec3(-0.5f, -1, 0), plVec3(0.5f, 1, 2), 3, plVec3(2, 0, 0));

    plHybridArray<plBrushCutterInput, 2> order1, order2;
    order1.PushBack(left);
    order1.PushBack(right);
    order2.PushBack(right);
    order2.PushBack(left);

    auto sortCutters = [](plHybridArray<plBrushCutterInput, 2>& ref_cutters)
    {
      ref_cutters.Sort([](const plBrushCutterInput& a, const plBrushCutterInput& b)
        { return plBrushEvaluator::CutterLess(a.m_uiBrushId, a.m_GlobalTransform.m_vPosition, b.m_uiBrushId, b.m_GlobalTransform.m_vPosition); });
    };

    sortCutters(order1);
    sortCutters(order2);
    PL_TEST_INT(order1[0].m_uiBrushId, order2[0].m_uiBrushId);

    // the cut faces of cutter 7 are the same set wherever the cutters stand
    plEditableMesh before = wall;
    plBrushEvaluator::Evaluate(before, plTransform::MakeIdentity(), order1, noAttributes);

    left.m_GlobalTransform.m_vPosition.x = 2.5f;
    right.m_GlobalTransform.m_vPosition.x = -2.5f;
    left.m_WorldBounds = left.m_Mesh.ComputeBounds();
    left.m_WorldBounds.Translate(left.m_GlobalTransform.m_vPosition);
    right.m_WorldBounds = right.m_Mesh.ComputeBounds();
    right.m_WorldBounds.Translate(right.m_GlobalTransform.m_vPosition);

    plHybridArray<plBrushCutterInput, 2> moved;
    moved.PushBack(left);
    moved.PushBack(right);
    sortCutters(moved);

    plEditableMesh after = wall;
    plBrushEvaluator::Evaluate(after, plTransform::MakeIdentity(), moved, noAttributes);

    for (plUInt32 uiPlane = 0; uiPlane < 6; ++uiPlane)
    {
      const plUInt32 uiId = plBrushFaceIds::MakeCutFaceId(7, 0, uiPlane, true);
      PL_TEST_BOOL(HasFaceId(before, uiId) == HasFaceId(after, uiId));
    }

    PL_TEST_DOUBLE(ComputeVolume(before), ComputeVolume(after), 0.0001);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Attributes reach cut faces")
  {
    plBrushCutterInput door = MakeCutter(plVec3(-0.5f, -1, 0), plVec3(0.5f, 1, 2), 55);

    plBrushFaceAttributeMap attributes;
    for (plUInt32 uiPlane = 0; uiPlane < 6; ++uiPlane)
    {
      plBrushFaceAttributes& attr = attributes[plBrushFaceIds::MakeCutFaceId(55, 0, uiPlane, true)];
      attr.m_uiMask = plBrushFaceAttributes::Slot;
      attr.m_uiMaterialSlot = 2;
    }

    plEditableMesh result = wall;
    plBrushEvaluator::Evaluate(result, plTransform::MakeIdentity(), plArrayPtr<const plBrushCutterInput>(&door, 1), attributes);

    bool bAnySlot2 = false;
    for (const plBrushFace& face : result.m_Faces)
    {
      if (plBrushFaceIds::IsDerived(face.m_uiFaceId) && face.m_uiMaterialSlot == 2)
        bAnySlot2 = true;
    }
    PL_TEST_BOOL(bAnySlot2);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Cut faces never advance the id counter")
  {
    plEditableMesh mesh = wall;
    const plUInt32 uiBefore = mesh.m_uiNextFaceId;

    plBrushCutterInput door = MakeCutter(plVec3(-0.5f, -1, 0), plVec3(0.5f, 1, 2), 9);
    plBrushEvaluator::Evaluate(mesh, plTransform::MakeIdentity(), plArrayPtr<const plBrushCutterInput>(&door, 1), noAttributes);

    PL_TEST_INT(mesh.m_uiNextFaceId, uiBefore);
  }
}

PL_CREATE_SIMPLE_TEST(CSG, DecomposeAndUnion)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Decomposing an L prism gives convex pieces of the same volume")
  {
    plEditableMesh lPrism;
    lPrism.BeginShell(false);

    // extruded L, CCW from above, 1 m high
    const plVec2 outline[] = {plVec2(0, 0), plVec2(2, 0), plVec2(2, 1), plVec2(1, 1), plVec2(1, 2), plVec2(0, 2)};
    const plUInt32 n = PL_ARRAY_SIZE(outline);

    plHybridArray<plUInt32, 6> bottom, top;
    for (plUInt32 i = 0; i < n; ++i)
    {
      bottom.PushBack(lPrism.AddVertex(plVec3(outline[i].x, outline[i].y, 0)));
      top.PushBack(lPrism.AddVertex(plVec3(outline[i].x, outline[i].y, 1)));
    }

    lPrism.AddFace(plArrayPtr<const plUInt32>(top.GetData(), top.GetCount()), 1);

    plHybridArray<plUInt32, 6> reversed;
    for (plUInt32 i = n; i > 0; --i)
      reversed.PushBack(bottom[i - 1]);
    lPrism.AddFace(plArrayPtr<const plUInt32>(reversed.GetData(), reversed.GetCount()), 2);

    for (plUInt32 i = 0; i < n; ++i)
    {
      const plUInt32 j = (i + 1) % n;
      const plUInt32 side[4] = {bottom[i], bottom[j], top[j], top[i]};
      lPrism.AddFace(plArrayPtr<const plUInt32>(side), 3 + i);
    }

    PL_TEST_DOUBLE(ComputeVolume(lPrism), 3.0, 0.0001);
    PL_TEST_BOOL(!plMeshCSG::AreAllShellsConvex(lPrism));

    plEditableMesh pieces;
    const plUInt32 uiPieces = plMeshCSG::DecomposeConvex(lPrism, pieces);

    PL_TEST_BOOL(uiPieces >= 2);
    PL_TEST_BOOL(plMeshCSG::AreAllShellsConvex(pieces));
    PL_TEST_DOUBLE(ComputeVolume(pieces), 3.0, 0.0001);

    // the decomposed cutter now cuts like the original L would
    const plEditableMesh block = MakeBox(plVec3(-1, -1, -1), plVec3(3, 3, 2));
    plEditableMesh carved;
    PL_TEST_BOOL(plMeshCSG::SubtractCutter(block, pieces, plTransform::MakeIdentity(), 0, carved, 12).Succeeded());
    PL_TEST_DOUBLE(ComputeVolume(carved), 4.0 * 4.0 * 3.0 - 3.0, 0.001);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Union of overlapping boxes")
  {
    const plEditableMesh a = MakeBox(plVec3(0, 0, 0), plVec3(2, 2, 2));
    const plEditableMesh b = MakeBox(plVec3(0, 0, 0), plVec3(2, 2, 2));

    plEditableMesh result;
    PL_TEST_BOOL(plMeshCSG::Union(a, b, plTransform(plVec3(1, 0, 0)), result).Succeeded());

    // 2x2x2 + 2x2x2 overlapping by 1x2x2
    PL_TEST_DOUBLE(ComputeVolume(result), 12.0, 0.0001);

    // A kept its ids, nothing collides with them
    for (plUInt32 uiFace = 0; uiFace < 6; ++uiFace)
    {
      PL_TEST_BOOL(HasFaceId(result, a.m_Faces[uiFace].m_uiFaceId));
    }
  }
}

namespace
{
  /// A box turned inside out: its faces point into the empty room.
  plEditableMesh MakeRoom(const plVec3& vMin, const plVec3& vMax)
  {
    plEditableMesh room = MakeBox(vMin, vMax);

    for (plBrushFace& face : room.m_Faces)
    {
      const plUInt32 n = face.m_Corners.GetCount();
      for (plUInt32 i = 0; i < n / 2; ++i)
        plMath::Swap(face.m_Corners[i], face.m_Corners[n - 1 - i]);
    }

    return room;
  }

  plUInt32 CountDerivedFaces(const plEditableMesh& mesh)
  {
    plUInt32 uiCount = 0;
    for (const plBrushFace& face : mesh.m_Faces)
    {
      if (plBrushFaceIds::IsDerived(face.m_uiFaceId))
        ++uiCount;
    }
    return uiCount;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(CSG, Rooms)
{
  const plBrushFaceAttributeMap noAttributes;

  PL_TEST_BLOCK(plTestBlock::Enabled, "An inside-out box is a room")
  {
    const plEditableMesh room = MakeRoom(plVec3(0, 0, 0), plVec3(4, 4, 4));
    PL_TEST_BOOL(plMeshCSG::IsShellInverted(room, room.m_Shells[0]));
    PL_TEST_BOOL(plMeshCSG::HasInvertedShell(room));

    const plEditableMesh box = MakeBox(plVec3(0, 0, 0), plVec3(4, 4, 4));
    PL_TEST_BOOL(!plMeshCSG::HasInvertedShell(box));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "A cutter between two rooms opens one passage")
  {
    // two 4 m rooms 1 m apart, a 2 x 2 x 2.5 doorway cutter reaching half a metre into each
    const plEditableMesh roomA = MakeRoom(plVec3(0, 0, 0), plVec3(4, 4, 4));
    const plEditableMesh roomB = MakeRoom(plVec3(5, 0, 0), plVec3(9, 4, 4));
    const plBrushCutterInput door = MakeCutter(plVec3(3.5f, 1, 0), plVec3(5.5f, 3, 2.5f), 7);

    plBrushRoomInput inputA;
    inputA.m_Mesh = roomA;
    inputA.m_uiBrushId = 1;

    plBrushRoomInput inputB;
    inputB.m_Mesh = roomB;
    inputB.m_uiBrushId = 2;

    plEditableMesh resultA = roomA;
    plBrushRoomContext contextA;
    contextA.m_uiTargetBrushId = 1;
    contextA.m_OtherRooms = plArrayPtr<const plBrushRoomInput>(&inputB, 1);
    plBrushEvaluator::Evaluate(resultA, plTransform::MakeIdentity(), plArrayPtr<const plBrushCutterInput>(&door, 1), noAttributes, nullptr, &contextA);

    plEditableMesh resultB = roomB;
    plBrushRoomContext contextB;
    contextB.m_uiTargetBrushId = 2;
    contextB.m_OtherRooms = plArrayPtr<const plBrushRoomInput>(&inputA, 1);
    plBrushEvaluator::Evaluate(resultB, plTransform::MakeIdentity(), plArrayPtr<const plBrushCutterInput>(&door, 1), noAttributes, nullptr, &contextB);

    PL_TEST_BOOL(resultA.IsStructurallyValid());
    PL_TEST_BOOL(resultB.IsStructurallyValid());

    // the lower brush id draws the passage, the other room only gets its doorway
    PL_TEST_BOOL(CountDerivedFaces(resultA) > 0);
    PL_TEST_INT(CountDerivedFaces(resultB), 0);

    // together they enclose both rooms and the 1 m passage between them, seen from inside
    plEditableMesh both = resultA;
    both.AppendMesh(resultB);
    PL_TEST_DOUBLE(ComputeVolume(both), -(64.0 + 64.0 + 1.0 * 2.0 * 2.5), 0.001);

    // nothing of the passage reaches into either room
    for (const plBrushFace& face : resultA.m_Faces)
    {
      if (!plBrushFaceIds::IsDerived(face.m_uiFaceId))
        continue;

      for (plUInt32 uiCorner : face.m_Corners)
      {
        PL_TEST_BOOL(resultA.m_Vertices[uiCorner].x >= 4.0f - 0.001f && resultA.m_Vertices[uiCorner].x <= 5.0f + 0.001f);
      }
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "A cutter through one wall makes an alcove")
  {
    const plEditableMesh room = MakeRoom(plVec3(0, 0, 0), plVec3(4, 4, 4));
    const plBrushCutterInput niche = MakeCutter(plVec3(3.5f, 1, 0), plVec3(5, 3, 2), 3);

    plEditableMesh result = room;
    plBrushEvaluator::Evaluate(result, plTransform::MakeIdentity(), plArrayPtr<const plBrushCutterInput>(&niche, 1), noAttributes);

    PL_TEST_BOOL(result.IsStructurallyValid());
    PL_TEST_DOUBLE(ComputeVolume(result), -(64.0 + 1.0 * 2.0 * 2.0), 0.001);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Solid shells next to a room still carve as solids")
  {
    plEditableMesh mesh = MakeRoom(plVec3(0, 0, 0), plVec3(4, 4, 4));
    mesh.AppendMesh(MakeBox(plVec3(10, 0, 0), plVec3(12, 2, 2)));

    const plBrushCutterInput cutter = MakeCutter(plVec3(11, -1, -1), plVec3(13, 3, 3), 5);

    plEditableMesh result = mesh;
    plBrushEvaluator::Evaluate(result, plTransform::MakeIdentity(), plArrayPtr<const plBrushCutterInput>(&cutter, 1), noAttributes);

    // the box loses half, the room is untouched
    PL_TEST_DOUBLE(ComputeVolume(result), -64.0 + 4.0, 0.001);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Overlapping cutters share one passage")
  {
    const plEditableMesh roomA = MakeRoom(plVec3(0, 0, 0), plVec3(4, 4, 4));
    const plEditableMesh roomB = MakeRoom(plVec3(6, 0, 0), plVec3(10, 4, 4));

    // an L-free passage made of two overlapping cutters
    const plBrushCutterInput cutters[] = {
      MakeCutter(plVec3(3.5f, 1, 0), plVec3(5.5f, 3, 2), 7),
      MakeCutter(plVec3(4.5f, 1, 0), plVec3(6.5f, 3, 2), 8),
    };

    plBrushRoomInput inputB;
    inputB.m_Mesh = roomB;
    inputB.m_uiBrushId = 2;

    plBrushRoomInput inputA;
    inputA.m_Mesh = roomA;
    inputA.m_uiBrushId = 1;

    plEditableMesh resultA = roomA;
    plBrushRoomContext contextA;
    contextA.m_uiTargetBrushId = 1;
    contextA.m_OtherRooms = plArrayPtr<const plBrushRoomInput>(&inputB, 1);
    plBrushEvaluator::Evaluate(resultA, plTransform::MakeIdentity(), cutters, noAttributes, nullptr, &contextA);

    plEditableMesh resultB = roomB;
    plBrushRoomContext contextB;
    contextB.m_uiTargetBrushId = 2;
    contextB.m_OtherRooms = plArrayPtr<const plBrushRoomInput>(&inputA, 1);
    plBrushEvaluator::Evaluate(resultB, plTransform::MakeIdentity(), cutters, noAttributes, nullptr, &contextB);

    plEditableMesh both = resultA;
    both.AppendMesh(resultB);
    PL_TEST_DOUBLE(ComputeVolume(both), -(64.0 + 64.0 + 2.0 * 2.0 * 2.0), 0.001);
  }
}

PL_CREATE_SIMPLE_TEST(CSG, EditedCutters)
{
  const plBrushFaceAttributeMap noAttributes;

  auto makeCutterInput = [](const plEditableMesh& mesh, plUInt32 uiId)
  {
    plBrushCutterInput cutter;
    cutter.m_Mesh = mesh;
    cutter.m_uiBrushId = uiId;
    cutter.m_WorldBounds = mesh.ComputeBounds();
    return cutter;
  };

  // a box cutter with one top corner pulled in, as a vertex edit leaves it: not convex, faces bent
  plEditableMesh dented = MakeBox(plVec3(-0.5f, -1, 0), plVec3(0.5f, 1, 2));
  dented.m_Vertices[6] = plVec3(0.0f, 0.0f, 1.0f);

  PL_TEST_BLOCK(plTestBlock::Enabled, "A non-convex cutter is split into convex pieces")
  {
    PL_TEST_BOOL(!plMeshCSG::AreAllShellsConvex(dented));

    plEditableMesh pieces;
    PL_TEST_BOOL(plMeshCSG::MakeConvexCutter(dented, pieces).Succeeded());
    PL_TEST_BOOL(plMeshCSG::AreAllShellsConvex(pieces));
    PL_TEST_DOUBLE(ComputeVolume(pieces), ComputeVolume(dented), 0.001);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "An edited cutter carves its own shape")
  {
    plEditableMesh block = MakeBox(plVec3(-2, -2, -1), plVec3(2, 2, 3));
    const double fBlock = ComputeVolume(block);

    const plBrushCutterInput cutter = makeCutterInput(dented, 9);

    plDynamicArray<plUInt32> refused;
    plBrushEvaluator::Evaluate(block, plTransform::MakeIdentity(), plArrayPtr<const plBrushCutterInput>(&cutter, 1), noAttributes, &refused);

    PL_TEST_BOOL(refused.IsEmpty());
    PL_TEST_BOOL(block.IsStructurallyValid());
    PL_TEST_DOUBLE(ComputeVolume(block), fBlock - ComputeVolume(dented), 0.001);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "An edited cutter between two rooms still opens a passage")
  {
    const plEditableMesh roomA = MakeRoom(plVec3(0, 0, 0), plVec3(4, 4, 4));
    const plEditableMesh roomB = MakeRoom(plVec3(5, 0, 0), plVec3(9, 4, 4));

    // the doorway with its top corner over the gap pulled down: a sloped passage ceiling
    plEditableMesh door = MakeBox(plVec3(3.5f, 1, 0), plVec3(5.5f, 3, 2.5f));
    door.m_Vertices[6] = plVec3(5.5f, 3, 2.0f);
    const plBrushCutterInput cutter = makeCutterInput(door, 7);

    plBrushRoomInput inputB;
    inputB.m_Mesh = roomB;
    inputB.m_uiBrushId = 2;

    plBrushRoomInput inputA;
    inputA.m_Mesh = roomA;
    inputA.m_uiBrushId = 1;

    plEditableMesh resultA = roomA;
    plBrushRoomContext contextA;
    contextA.m_uiTargetBrushId = 1;
    contextA.m_OtherRooms = plArrayPtr<const plBrushRoomInput>(&inputB, 1);

    plDynamicArray<plUInt32> refused;
    plBrushEvaluator::Evaluate(resultA, plTransform::MakeIdentity(), plArrayPtr<const plBrushCutterInput>(&cutter, 1), noAttributes, &refused, &contextA);

    plEditableMesh resultB = roomB;
    plBrushRoomContext contextB;
    contextB.m_uiTargetBrushId = 2;
    contextB.m_OtherRooms = plArrayPtr<const plBrushRoomInput>(&inputA, 1);
    plBrushEvaluator::Evaluate(resultB, plTransform::MakeIdentity(), plArrayPtr<const plBrushCutterInput>(&cutter, 1), noAttributes, &refused, &contextB);

    PL_TEST_BOOL(refused.IsEmpty());
    PL_TEST_BOOL(CountDerivedFaces(resultA) > 0);

    // a closed space: both rooms plus a passage a little smaller than the box doorway
    plEditableMesh both = resultA;
    both.AppendMesh(resultB);
    const double fVolume = -ComputeVolume(both);
    PL_TEST_BOOL(fVolume > 128.0 + 4.0 && fVolume < 128.0 + 5.0);
  }
}
