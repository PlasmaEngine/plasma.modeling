#include <ModelingTest/ModelingTestPCH.h>

#include <ModelingPlugin/Modeling/BrushGenerators.h>
#include <ModelingPlugin/Modeling/MeshCSG.h>
#include <ModelingTest/TestHelpers.h>

PL_CREATE_SIMPLE_TEST_GROUP(Generators);

using namespace plModelingTestUtils;

namespace
{
  const plBrushShape::Enum c_AllShapes[] = {plBrushShape::Box, plBrushShape::RampX, plBrushShape::RampY, plBrushShape::Column, plBrushShape::StairsX,
    plBrushShape::StairsY, plBrushShape::ArchX, plBrushShape::ArchY, plBrushShape::SpiralStairs, plBrushShape::Sphere, plBrushShape::Cone,
    plBrushShape::Pyramid};

  plBrushGeneratorParams MakeParams()
  {
    plBrushGeneratorParams params;
    params.m_fSizeNegX = 1.5f;
    params.m_fSizePosX = 2.0f;
    params.m_fSizeNegY = 0.75f;
    params.m_fSizePosY = 1.25f;
    params.m_fSizeNegZ = 0.0f;
    params.m_fSizePosZ = 3.0f;
    params.m_uiDetail = 8;
    params.m_Curvature = plAngle::MakeFromDegree(180);
    params.m_fThickness = 0.4f;
    return params;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Generators, EveryShapeIsClosedAndConvexPerShell)
{
  for (plBrushShape::Enum shape : c_AllShapes)
  {
    plBrushGeneratorParams params = MakeParams();

    // curved stairs rotate their steps past the box on purpose
    if (shape == plBrushShape::StairsX || shape == plBrushShape::StairsY)
      params.m_Curvature = plAngle();

    plEditableMesh mesh;
    plBrushGenerators::Generate(shape, params, mesh);

    PL_TEST_BOOL_MSG(mesh.IsStructurallyValid(), "shape %d", static_cast<int>(shape));
    PL_TEST_BOOL_MSG(!mesh.m_Shells.IsEmpty(), "shape %d", static_cast<int>(shape));

    for (plUInt32 uiShell = 0; uiShell < mesh.m_Shells.GetCount(); ++uiShell)
    {
      plEditableMesh shellMesh;
      mesh.ExtractShell(uiShell, shellMesh);

      PL_TEST_BOOL_MSG(IsClosedByIndex(shellMesh), "shape %d shell %u not closed", static_cast<int>(shape), uiShell);
      PL_TEST_BOOL_MSG(ComputeVolume(shellMesh) > 0.0, "shape %d shell %u faces inward", static_cast<int>(shape), uiShell);
      PL_TEST_BOOL_MSG(plMeshCSG::IsShellConvex(shellMesh, shellMesh.m_Shells[0]), "shape %d shell %u not convex", static_cast<int>(shape), uiShell);
    }

    // everything stays inside the authored extents
    const plBoundingBox bounds = mesh.ComputeBounds();
    PL_TEST_BOOL_MSG(bounds.m_vMin.x >= -1.5f - 0.001f && bounds.m_vMax.x <= 2.0f + 0.001f, "shape %d exceeds X", static_cast<int>(shape));
    PL_TEST_BOOL_MSG(bounds.m_vMin.y >= -0.75f - 0.001f && bounds.m_vMax.y <= 1.25f + 0.001f, "shape %d exceeds Y", static_cast<int>(shape));
    PL_TEST_BOOL_MSG(bounds.m_vMin.z >= -0.001f && bounds.m_vMax.z <= 3.0f + 0.001f, "shape %d exceeds Z", static_cast<int>(shape));
  }
}

PL_CREATE_SIMPLE_TEST(Generators, FaceIdsAreSemantic)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Stair step ids survive a step count change")
  {
    plBrushGeneratorParams params = MakeParams();
    params.m_Curvature = plAngle();

    plEditableMesh four, six;
    params.m_uiDetail = 4;
    plBrushGenerators::Generate(plBrushShape::StairsX, params, four);
    params.m_uiDetail = 6;
    plBrushGenerators::Generate(plBrushShape::StairsX, params, six);

    // the top of step 3 exists in both
    const plUInt32 uiTopOfStep3 = plBrushFaceKind::MakeId(plBrushFaceKind::Top, 2);
    PL_TEST_BOOL(HasFaceId(four, uiTopOfStep3));
    PL_TEST_BOOL(HasFaceId(six, uiTopOfStep3));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Face ids are unique within a shape")
  {
    const plBrushGeneratorParams params = MakeParams();

    for (plBrushShape::Enum shape : c_AllShapes)
    {
      plEditableMesh mesh;
      plBrushGenerators::Generate(shape, params, mesh);

      plHashSet<plUInt32> ids;
      for (const plBrushFace& face : mesh.m_Faces)
      {
        PL_TEST_BOOL_MSG(!ids.Insert(face.m_uiFaceId), "shape %d repeats id %u", static_cast<int>(shape), face.m_uiFaceId);
      }
    }
  }
}

PL_CREATE_SIMPLE_TEST(Generators, ArchesStandUpright)
{
  plBrushGeneratorParams params;
  params.m_fSizeNegX = params.m_fSizePosX = 2.0f;
  params.m_fSizeNegY = params.m_fSizePosY = 0.25f;
  params.m_fSizeNegZ = params.m_fSizePosZ = 2.0f;
  params.m_uiDetail = 8;
  params.m_Curvature = plAngle::MakeFromDegree(180);
  params.m_fThickness = 0.5f;

  plEditableMesh archX;
  plBrushGenerators::Generate(plBrushShape::ArchX, params, archX);

  // spans X, depth along Y, and a half ring only occupies the upper half
  plBoundingBox b = archX.ComputeBounds();
  PL_TEST_FLOAT(b.m_vMin.x, -2.0f, 0.01f);
  PL_TEST_FLOAT(b.m_vMax.x, 2.0f, 0.01f);
  PL_TEST_FLOAT(b.m_vMax.y - b.m_vMin.y, 0.5f, 0.01f);
  PL_TEST_FLOAT(b.m_vMin.z, 0.0f, 0.01f);

  // Arch Y is the same arch turned to span Y
  params.m_fSizeNegX = params.m_fSizePosX = 0.25f;
  params.m_fSizeNegY = params.m_fSizePosY = 2.0f;

  plEditableMesh archY;
  plBrushGenerators::Generate(plBrushShape::ArchY, params, archY);

  b = archY.ComputeBounds();
  PL_TEST_FLOAT(b.m_vMin.y, -2.0f, 0.01f);
  PL_TEST_FLOAT(b.m_vMax.y, 2.0f, 0.01f);
  PL_TEST_FLOAT(b.m_vMax.x - b.m_vMin.x, 0.5f, 0.01f);
  PL_TEST_FLOAT(b.m_vMin.z, 0.0f, 0.01f);

  PL_TEST_DOUBLE(ComputeVolume(archX), ComputeVolume(archY), 0.0001);
}

PL_CREATE_SIMPLE_TEST(Generators, RoundShapesApproachTheirVolume)
{
  plBrushGeneratorParams params;
  params.m_fSizeNegX = params.m_fSizePosX = 1.0f;
  params.m_fSizeNegY = params.m_fSizePosY = 1.0f;
  params.m_fSizeNegZ = params.m_fSizePosZ = 1.0f;
  params.m_uiDetail = 64;

  const double fPi = plMath::Pi<double>();

  // faceted shapes sit inside the true surface, so they come out a little smaller
  plEditableMesh sphere;
  plBrushGenerators::Generate(plBrushShape::Sphere, params, sphere);
  PL_TEST_DOUBLE(ComputeVolume(sphere), 4.0 / 3.0 * fPi, 0.1);

  plEditableMesh cone;
  plBrushGenerators::Generate(plBrushShape::Cone, params, cone);
  PL_TEST_DOUBLE(ComputeVolume(cone), fPi * 2.0 / 3.0, 0.02);

  // the pyramid is exact: base 2 x 2, height 2
  plEditableMesh pyramid;
  plBrushGenerators::Generate(plBrushShape::Pyramid, params, pyramid);
  PL_TEST_DOUBLE(ComputeVolume(pyramid), 8.0 / 3.0, 0.0001);

  // apexes and poles touch the box
  PL_TEST_FLOAT(sphere.ComputeBounds().m_vMax.z, 1.0f, 0.0001f);
  PL_TEST_FLOAT(sphere.ComputeBounds().m_vMin.z, -1.0f, 0.0001f);
  PL_TEST_FLOAT(cone.ComputeBounds().m_vMax.z, 1.0f, 0.0001f);
  PL_TEST_FLOAT(pyramid.ComputeBounds().m_vMax.z, 1.0f, 0.0001f);
}
