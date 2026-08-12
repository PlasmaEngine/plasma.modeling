#include <ModelingPlugin/ModelingPluginPCH.h>

#include <ModelingPlugin/Modeling/BrushGenerators.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_ENUM(plBrushShape, 1)
  PL_ENUM_CONSTANTS(plBrushShape::Box, plBrushShape::RampX, plBrushShape::RampY, plBrushShape::Column)
  PL_ENUM_CONSTANTS(plBrushShape::StairsX, plBrushShape::StairsY, plBrushShape::ArchX, plBrushShape::ArchY)
  PL_ENUM_CONSTANTS(plBrushShape::SpiralStairs, plBrushShape::Editable)
PL_END_STATIC_REFLECTED_ENUM;
// clang-format on

using Kind = plBrushFaceKind;

namespace
{
  /// Adds a closed axis-aligned box shell [vMin..vMax] whose face ids carry uiIndex.
  void AddBoxShell(plEditableMesh& mesh, const plVec3& vMin, const plVec3& vMax, plUInt32 uiIndex)
  {
    mesh.BeginShell(true);

    const plUInt32 c[8] = {
      mesh.AddVertex(plVec3(vMin.x, vMin.y, vMin.z)), // 0
      mesh.AddVertex(plVec3(vMax.x, vMin.y, vMin.z)), // 1
      mesh.AddVertex(plVec3(vMax.x, vMax.y, vMin.z)), // 2
      mesh.AddVertex(plVec3(vMin.x, vMax.y, vMin.z)), // 3
      mesh.AddVertex(plVec3(vMin.x, vMin.y, vMax.z)), // 4
      mesh.AddVertex(plVec3(vMax.x, vMin.y, vMax.z)), // 5
      mesh.AddVertex(plVec3(vMax.x, vMax.y, vMax.z)), // 6
      mesh.AddVertex(plVec3(vMin.x, vMax.y, vMax.z)), // 7
    };

    // CCW as seen from outside (+Z up)
    const plUInt32 top[4] = {c[4], c[5], c[6], c[7]};
    const plUInt32 bottom[4] = {c[3], c[2], c[1], c[0]};
    const plUInt32 posX[4] = {c[1], c[2], c[6], c[5]};
    const plUInt32 negX[4] = {c[3], c[0], c[4], c[7]};
    const plUInt32 posY[4] = {c[2], c[3], c[7], c[6]};
    const plUInt32 negY[4] = {c[0], c[1], c[5], c[4]};

    mesh.AddFace(plArrayPtr<const plUInt32>(top, 4), Kind::MakeId(Kind::Top, uiIndex));
    mesh.AddFace(plArrayPtr<const plUInt32>(bottom, 4), Kind::MakeId(Kind::Bottom, uiIndex));
    mesh.AddFace(plArrayPtr<const plUInt32>(posX, 4), Kind::MakeId(Kind::PosX, uiIndex));
    mesh.AddFace(plArrayPtr<const plUInt32>(negX, 4), Kind::MakeId(Kind::NegX, uiIndex));
    mesh.AddFace(plArrayPtr<const plUInt32>(posY, 4), Kind::MakeId(Kind::PosY, uiIndex));
    mesh.AddFace(plArrayPtr<const plUInt32>(negY, 4), Kind::MakeId(Kind::NegY, uiIndex));
  }

  void GenerateBox(const plVec3& vSize, plEditableMesh& mesh)
  {
    AddBoxShell(mesh, -vSize * 0.5f, vSize * 0.5f, 0);
  }

  /// Wedge rising towards +X: slope from the -X bottom edge up to the +X top edge.
  void GenerateRamp(const plVec3& vSize, plEditableMesh& mesh)
  {
    const plVec3 h = vSize * 0.5f;

    mesh.BeginShell(true);

    const plUInt32 c[6] = {
      mesh.AddVertex(plVec3(-h.x, -h.y, -h.z)), // 0 front bottom left
      mesh.AddVertex(plVec3(h.x, -h.y, -h.z)),  // 1 back bottom left
      mesh.AddVertex(plVec3(h.x, h.y, -h.z)),   // 2 back bottom right
      mesh.AddVertex(plVec3(-h.x, h.y, -h.z)),  // 3 front bottom right
      mesh.AddVertex(plVec3(h.x, -h.y, h.z)),   // 4 back top left
      mesh.AddVertex(plVec3(h.x, h.y, h.z)),    // 5 back top right
    };

    const plUInt32 slope[4] = {c[0], c[3], c[5], c[4]};
    const plUInt32 bottom[4] = {c[3], c[2], c[1], c[0]};
    const plUInt32 back[4] = {c[1], c[2], c[5], c[4]};
    const plUInt32 left[3] = {c[0], c[4], c[1]};
    const plUInt32 right[3] = {c[3], c[2], c[5]};

    mesh.AddFace(plArrayPtr<const plUInt32>(slope, 4), Kind::MakeId(Kind::Slope));
    mesh.AddFace(plArrayPtr<const plUInt32>(bottom, 4), Kind::MakeId(Kind::Bottom));
    mesh.AddFace(plArrayPtr<const plUInt32>(back, 4), Kind::MakeId(Kind::PosX));
    mesh.AddFace(plArrayPtr<const plUInt32>(left, 3), Kind::MakeId(Kind::NegY));
    mesh.AddFace(plArrayPtr<const plUInt32>(right, 3), Kind::MakeId(Kind::PosY));
  }

  void GenerateColumn(const plVec3& vSize, plUInt32 uiDetail, plEditableMesh& mesh)
  {
    const plUInt32 uiSegments = plMath::Clamp<plUInt32>(uiDetail, 3, 64);
    const float rx = vSize.x * 0.5f;
    const float ry = vSize.y * 0.5f;
    const float hz = vSize.z * 0.5f;

    mesh.BeginShell(true);

    plSmallArray<plUInt32, 64> bottomRing;
    plSmallArray<plUInt32, 64> topRing;

    for (plUInt32 s = 0; s < uiSegments; ++s)
    {
      const plAngle a = plAngle::MakeFromRadian(2.0f * plMath::Pi<float>() * s / uiSegments);
      const float x = plMath::Cos(a) * rx;
      const float y = plMath::Sin(a) * ry;

      bottomRing.PushBack(mesh.AddVertex(plVec3(x, y, -hz)));
      topRing.PushBack(mesh.AddVertex(plVec3(x, y, hz)));
    }

    for (plUInt32 s = 0; s < uiSegments; ++s)
    {
      const plUInt32 n = (s + 1) % uiSegments;
      const plUInt32 side[4] = {bottomRing[s], bottomRing[n], topRing[n], topRing[s]};
      mesh.AddFace(plArrayPtr<const plUInt32>(side, 4), Kind::MakeId(Kind::Side, s));
    }

    // caps: top CCW from above, bottom reversed
    plSmallArray<plUInt32, 64> cap;

    for (plUInt32 s = 0; s < uiSegments; ++s)
    {
      cap.PushBack(topRing[s]);
    }
    mesh.AddFace(plArrayPtr<const plUInt32>(cap.GetData(), cap.GetCount()), Kind::MakeId(Kind::Top));

    cap.Clear();
    for (plUInt32 s = uiSegments; s > 0; --s)
    {
      cap.PushBack(bottomRing[s - 1]);
    }
    mesh.AddFace(plArrayPtr<const plUInt32>(cap.GetData(), cap.GetCount()), Kind::MakeId(Kind::Bottom));
  }

  /// N closed box steps rising towards +X. With curvature, each step is rotated progressively around
  /// the box center — curved stairs out of rotated convex slabs, one shell per step (CSG-friendly).
  void GenerateStairs(const plVec3& vSize, plUInt32 uiSteps, plAngle curvature, plEditableMesh& mesh)
  {
    uiSteps = plMath::Clamp<plUInt32>(uiSteps, 1, 64);

    const plVec3 h = vSize * 0.5f;
    const float fStepDepth = vSize.x / uiSteps;
    const float fStepHeight = vSize.z / uiSteps;

    for (plUInt32 s = 0; s < uiSteps; ++s)
    {
      const plVec3 vMin(-h.x + s * fStepDepth, -h.y, -h.z);
      const plVec3 vMax(-h.x + (s + 1) * fStepDepth, h.y, -h.z + (s + 1) * fStepHeight);

      const plUInt32 uiFirstVertex = mesh.m_Vertices.GetCount();
      AddBoxShell(mesh, vMin, vMax, s);

      if (curvature != plAngle())
      {
        const plAngle stepAngle = plAngle::MakeFromRadian(curvature.GetRadian() * (s + 0.5f) / uiSteps);
        const plMat3 rot = plMat3::MakeRotationZ(stepAngle);

        for (plUInt32 v = uiFirstVertex; v < mesh.m_Vertices.GetCount(); ++v)
        {
          mesh.m_Vertices[v] = rot * mesh.m_Vertices[v];
        }
      }
    }
  }

  /// Ring of closed segment shells around Z, bounded by the box's ellipse. For the spiral each segment
  /// rises like a step. Sweep of 0 means a full circle.
  void GenerateArch(const plVec3& vSize, plUInt32 uiSegments, float fThickness, plAngle sweep, bool bSpiralSteps, plEditableMesh& mesh)
  {
    uiSegments = plMath::Clamp<plUInt32>(uiSegments, 3, 64);

    const float rxOuter = vSize.x * 0.5f;
    const float ryOuter = vSize.y * 0.5f;
    const float rxInner = plMath::Max(0.01f, rxOuter - fThickness);
    const float ryInner = plMath::Max(0.01f, ryOuter - fThickness);
    const float hz = vSize.z * 0.5f;

    const float fSweepRad = (sweep == plAngle()) ? 2.0f * plMath::Pi<float>() : sweep.GetRadian();

    for (plUInt32 s = 0; s < uiSegments; ++s)
    {
      const float a0 = fSweepRad * s / uiSegments;
      const float a1 = fSweepRad * (s + 1) / uiSegments;

      const plVec2 dir0(plMath::Cos(plAngle::MakeFromRadian(a0)), plMath::Sin(plAngle::MakeFromRadian(a0)));
      const plVec2 dir1(plMath::Cos(plAngle::MakeFromRadian(a1)), plMath::Sin(plAngle::MakeFromRadian(a1)));

      float fBottom = -hz;
      float fTop = hz;

      if (bSpiralSteps)
      {
        fTop = -hz + vSize.z * (s + 1) / uiSegments;
      }

      mesh.BeginShell(true);

      const plUInt32 outer0b = mesh.AddVertex(plVec3(dir0.x * rxOuter, dir0.y * ryOuter, fBottom));
      const plUInt32 outer1b = mesh.AddVertex(plVec3(dir1.x * rxOuter, dir1.y * ryOuter, fBottom));
      const plUInt32 inner0b = mesh.AddVertex(plVec3(dir0.x * rxInner, dir0.y * ryInner, fBottom));
      const plUInt32 inner1b = mesh.AddVertex(plVec3(dir1.x * rxInner, dir1.y * ryInner, fBottom));
      const plUInt32 outer0t = mesh.AddVertex(plVec3(dir0.x * rxOuter, dir0.y * ryOuter, fTop));
      const plUInt32 outer1t = mesh.AddVertex(plVec3(dir1.x * rxOuter, dir1.y * ryOuter, fTop));
      const plUInt32 inner0t = mesh.AddVertex(plVec3(dir0.x * rxInner, dir0.y * ryInner, fTop));
      const plUInt32 inner1t = mesh.AddVertex(plVec3(dir1.x * rxInner, dir1.y * ryInner, fTop));

      const plUInt32 outer[4] = {outer0b, outer1b, outer1t, outer0t};
      const plUInt32 inner[4] = {inner1b, inner0b, inner0t, inner1t};
      const plUInt32 top[4] = {outer0t, outer1t, inner1t, inner0t};
      const plUInt32 bottom[4] = {inner0b, inner1b, outer1b, outer0b};
      const plUInt32 start[4] = {inner0b, outer0b, outer0t, inner0t};
      const plUInt32 end[4] = {outer1b, inner1b, inner1t, outer1t};

      mesh.AddFace(plArrayPtr<const plUInt32>(outer, 4), Kind::MakeId(Kind::Outer, s));
      mesh.AddFace(plArrayPtr<const plUInt32>(inner, 4), Kind::MakeId(Kind::Inner, s));
      mesh.AddFace(plArrayPtr<const plUInt32>(top, 4), Kind::MakeId(Kind::Top, s));
      mesh.AddFace(plArrayPtr<const plUInt32>(bottom, 4), Kind::MakeId(Kind::Bottom, s));
      mesh.AddFace(plArrayPtr<const plUInt32>(start, 4), Kind::MakeId(Kind::SegStart, s));
      mesh.AddFace(plArrayPtr<const plUInt32>(end, 4), Kind::MakeId(Kind::SegEnd, s));
    }
  }

  void TranslateAll(plEditableMesh& mesh, const plVec3& vOffset)
  {
    for (plVec3& v : mesh.m_Vertices)
    {
      v += vOffset;
    }
  }

  void SwapXY(plEditableMesh& mesh)
  {
    // rotate -90 degrees around Z, same as the greybox RampY / StairsY path
    for (plVec3& v : mesh.m_Vertices)
    {
      v = plVec3(v.y, -v.x, v.z);
    }
  }
} // namespace

void plBrushGenerators::Generate(plBrushShape::Enum shape, const plBrushGeneratorParams& params, plEditableMesh& out_mesh)
{
  out_mesh.Clear();

  plVec3 vSize = params.GetSize();

  if (vSize.x <= 0 || vSize.y <= 0 || vSize.z <= 0)
  {
    // tiny dummy box so there is always valid geometry
    AddBoxShell(out_mesh, plVec3(-0.005f), plVec3(0.005f), 0);
    TranslateAll(out_mesh, params.GetOffset());
    return;
  }

  switch (shape)
  {
    case plBrushShape::Box:
      GenerateBox(vSize, out_mesh);
      break;

    case plBrushShape::RampX:
      GenerateRamp(vSize, out_mesh);
      break;

    case plBrushShape::RampY:
      plMath::Swap(vSize.x, vSize.y);
      GenerateRamp(vSize, out_mesh);
      SwapXY(out_mesh);
      break;

    case plBrushShape::Column:
      GenerateColumn(vSize, params.m_uiDetail, out_mesh);
      break;

    case plBrushShape::StairsX:
      GenerateStairs(vSize, params.m_uiDetail, params.m_Curvature, out_mesh);
      break;

    case plBrushShape::StairsY:
      plMath::Swap(vSize.x, vSize.y);
      GenerateStairs(vSize, params.m_uiDetail, params.m_Curvature, out_mesh);
      SwapXY(out_mesh);
      break;

    case plBrushShape::ArchX:
    case plBrushShape::ArchY:
      GenerateArch(vSize, params.m_uiDetail, params.m_fThickness, params.m_Curvature, false, out_mesh);
      break;

    case plBrushShape::SpiralStairs:
      GenerateArch(vSize, params.m_uiDetail, params.m_fThickness, params.m_Curvature, true, out_mesh);
      break;

    case plBrushShape::Editable:
      // the stored mesh is authoritative — nothing to generate
      break;

    default:
      PL_ASSERT_NOT_IMPLEMENTED;
  }

  TranslateAll(out_mesh, params.GetOffset());
}


PL_STATICLINK_FILE(ModelingPlugin, ModelingPlugin_Modeling_Implementation_BrushGenerators);
