#pragma once

#include <ModelingPlugin/ModelingPluginDLL.h>

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/SmallArray.h>
#include <Foundation/Math/Angle.h>
#include <Foundation/Math/Vec2.h>
#include <Foundation/Math/Vec3.h>
#include <Foundation/Types/VariantType.h>

class plStreamWriter;
class plStreamReader;
class plGeometry;
class plColor;

/// \brief Per-face projected UV mapping (Valve-220 style).
///
/// World-aligned faces derive their projection axes from the face normal at mesh generation time, so
/// tiling runs seamlessly across adjacent brushes without authored UVs. Face-aligned (explicit) axes
/// are stored for later editing phases. UV = (dot(pos, uAxis), dot(pos, vAxis)) * scale + offset.
struct PL_MODELINGPLUGIN_DLL plBrushFaceUV
{
  plVec3 m_vUAxis = plVec3(1, 0, 0);
  plVec3 m_vVAxis = plVec3(0, 1, 0);
  plVec2 m_vOffset = plVec2::MakeZero();
  plVec2 m_vScale = plVec2(1, 1); ///< texture repeats per world unit
  plAngle m_Rotation;
  bool m_bWorldAligned = true;
};

/// \brief One polygonal face of a brush. Corners index into the mesh's vertex array, counter-clockwise.
///
/// The face id is the linchpin of the system: generators emit SEMANTIC ids (the top of step 3 keeps its
/// id no matter how many steps exist), so per-face materials and UVs survive parameter changes. Selection
/// and attribute maps are keyed by face id, never by index.
struct PL_MODELINGPLUGIN_DLL plBrushFace
{
  plUInt32 m_uiFaceId = 0;
  plUInt8 m_uiMaterialSlot = 0;
  plSmallArray<plUInt32, 4> m_Corners;
  plBrushFaceUV m_UV;
};

/// \brief A connected component of the mesh — one closed solid (or open sheet).
struct PL_MODELINGPLUGIN_DLL plBrushShell
{
  plUInt32 m_uiFirstFace = 0;
  plUInt32 m_uiNumFaces = 0;
  bool m_bConvex = false;
  bool m_bSubtractive = false;
};

/// \brief Triangle range of one material slot inside the generated render geometry.
struct PL_MODELINGPLUGIN_DLL plBrushSubMeshRange
{
  plUInt32 m_uiFirstTriangle = 0;
  plUInt32 m_uiNumTriangles = 0;
  plUInt8 m_uiMaterialSlot = 0;
};

/// \brief The editable mesh every modeling feature operates on: parametric generators emit it, poly
/// operations reshape it, CSG carves it, and the component serializes it.
///
/// Deliberately stores only positions, face loops and per-face attributes. Adjacency (half-edge) is
/// derived on demand in later phases — CSG output and imported geometry are routinely non-manifold, and
/// a derived view can simply leave twin links invalid where a stored structure would have to be repaired.
class PL_MODELINGPLUGIN_DLL plEditableMesh
{
public:
  void Clear();

  plUInt32 AddVertex(const plVec3& vPos);

  /// \brief Adds a face over existing vertices. Corners must wind counter-clockwise as seen from outside.
  plUInt32 AddFace(plArrayPtr<const plUInt32> corners, plUInt32 uiFaceId, plUInt8 uiMaterialSlot = 0);

  /// \brief Convenience: adds count vertices and a face over them in one call.
  plUInt32 AddFace(plArrayPtr<const plVec3> positions, plUInt32 uiFaceId, plUInt8 uiMaterialSlot = 0);

  /// \brief Begins a new shell; faces added afterwards belong to it (until the next BeginShell).
  void BeginShell(bool bConvex, bool bSubtractive = false);

  plVec3 ComputeFaceNormal(plUInt32 uiFaceIndex) const; ///< Newell's method — robust for slightly non-planar loops
  plVec3 ComputeFaceCenter(plUInt32 uiFaceIndex) const;

  /// \brief Highest material slot referenced by any face, +1. At least 1.
  plUInt32 GetUsedMaterialSlotCount() const;

  /// \brief Flat-shaded triangle-soup render geometry, faces sorted by material slot.
  ///
  /// Emits fan triangles per face (faces are convex), duplicating vertices per face so normals stay hard.
  /// World-aligned UVs are derived here from each face normal. out_ranges receives one entry per material
  /// slot that has faces; polygon order in the geometry equals triangle order in the final index buffer.
  void BuildRenderGeometry(plGeometry& out_geom, plDynamicArray<plBrushSubMeshRange>& out_ranges, const plColor& color) const;

  void Serialize(plStreamWriter& inout_stream) const;
  plResult Deserialize(plStreamReader& inout_stream);

  void ToDataBuffer(plDataBuffer& out_buffer) const;
  plResult FromDataBuffer(const plDataBuffer& buffer);

  /// \brief Derives the Valve-220 world-aligned projection axes for a normal: the dominant axis picks the
  /// plane, so axis-aligned faces tile exactly and tilted faces get the nearest cardinal projection.
  static void GetWorldAlignedUVAxes(const plVec3& vNormal, plVec3& out_vUAxis, plVec3& out_vVAxis);

  plDynamicArray<plVec3> m_Vertices;
  plDynamicArray<plBrushFace> m_Faces;
  plDynamicArray<plBrushShell> m_Shells;
  plUInt32 m_uiNextFaceId = 1;
};
