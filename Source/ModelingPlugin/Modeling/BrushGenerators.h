#pragma once

#include <ModelingPlugin/ModelingPluginDLL.h>

#include <Foundation/Reflection/Reflection.h>
#include <ModelingPlugin/Modeling/EditableMesh.h>

/// \brief The parametric shapes a brush can generate. Mirrors plGreyBoxShape so artist muscle memory
/// (and the GreyBox migration) transfers 1:1. Editable means the mesh no longer follows parameters.
struct PL_MODELINGPLUGIN_DLL plBrushShape
{
  using StorageType = plUInt8;

  enum Enum
  {
    Box,
    RampX,
    RampY,
    Column,
    StairsX,
    StairsY,
    ArchX,
    ArchY,
    SpiralStairs,
    Editable,

    Default = Box
  };
};

PL_DECLARE_REFLECTABLE_TYPE(PL_MODELINGPLUGIN_DLL, plBrushShape);

/// \brief Semantic face kinds, stored in the low byte of a face id. The owning step / segment index sits
/// in bits 8+, so "the top of step 3" keeps its id when the step count changes.
struct plBrushFaceKind
{
  enum Enum : plUInt32
  {
    PosX = 1,
    NegX = 2,
    PosY = 3,
    NegY = 4,
    Top = 5,
    Bottom = 6,
    Slope = 7,
    Side = 8,     ///< cylinder side segment
    Outer = 9,    ///< arch outer wall
    Inner = 10,   ///< arch inner wall
    SegStart = 11,
    SegEnd = 12,
  };

  static constexpr plUInt32 MakeId(Enum kind, plUInt32 uiIndex = 0) { return (uiIndex << 8) | static_cast<plUInt32>(kind); }
};

struct PL_MODELINGPLUGIN_DLL plBrushGeneratorParams
{
  float m_fSizeNegX = 1, m_fSizePosX = 1;
  float m_fSizeNegY = 1, m_fSizePosY = 1;
  float m_fSizeNegZ = 1, m_fSizePosZ = 1;

  plUInt32 m_uiDetail = 16; ///< steps for stairs, segments for cylinder / arch
  plAngle m_Curvature;      ///< sweep angle for arch / spiral (0 = full circle), curve for stairs
  float m_fThickness = 0.5f;

  plVec3 GetSize() const { return plVec3(m_fSizeNegX + m_fSizePosX, m_fSizeNegY + m_fSizePosY, m_fSizeNegZ + m_fSizePosZ); }
  plVec3 GetOffset() const { return plVec3((m_fSizePosX - m_fSizeNegX) * 0.5f, (m_fSizePosY - m_fSizeNegY) * 0.5f, (m_fSizePosZ - m_fSizeNegZ) * 0.5f); }
};

namespace plBrushGenerators
{
  /// \brief Generates the given shape into out_mesh (cleared first), centered per the params' extents.
  /// Editable is a no-op — the caller keeps the stored mesh instead.
  PL_MODELINGPLUGIN_DLL void Generate(plBrushShape::Enum shape, const plBrushGeneratorParams& params, plEditableMesh& out_mesh);
} // namespace plBrushGenerators
