#pragma once

#include <ModelingPlugin/ModelingPluginDLL.h>

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/SmallArray.h>
#include <Foundation/Math/Angle.h>
#include <Foundation/Math/BoundingBox.h>
#include <Foundation/Math/Transform.h>
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

/// \brief Face id ranges. Ids below c_uiDerivedBit come from generators and the mesh's own counter;
/// ids with the bit set are derived (cut faces keyed by cutter id and plane) and never advance the counter.
struct PL_MODELINGPLUGIN_DLL plBrushFaceIds
{
  static constexpr plUInt32 c_uiDerivedBit = 0x80000000u;

  static constexpr bool IsDerived(plUInt32 uiFaceId) { return (uiFaceId & c_uiDerivedBit) != 0; }

  /// \brief The id of every face a cutter's plane leaves behind. Stable under cutter moves and reordering.
  static plUInt32 MakeCutFaceId(plUInt32 uiCutterId, plUInt32 uiShell, plUInt32 uiPlane, bool bOutsidePiece);
};

struct plBrushFaceFlags
{
  enum Enum : plUInt8
  {
    None = 0,
    HardEdges = PL_BIT(0),  ///< never shares smoothed normals with neighbours
    ExplicitUV = PL_BIT(1), ///< m_CornerUVs are authoritative instead of the projection
    Internal = PL_BIT(2),   ///< closes a shell for CSG but lies inside the solid: never rendered, collided or picked
  };
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
  plUInt8 m_uiFlags = plBrushFaceFlags::None;
  plSmallArray<plUInt32, 4> m_Corners;
  plBrushFaceUV m_UV;
  plSmallArray<plVec2, 4> m_CornerUVs; ///< one per corner, only with plBrushFaceFlags::ExplicitUV
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

  /// \brief Appends all shells, faces and vertices of another mesh (indices remapped).
  void AppendMesh(const plEditableMesh& other);

  /// \brief Copies one shell into a standalone mesh (only the vertices it references).
  void ExtractShell(plUInt32 uiShellIndex, plEditableMesh& out_mesh) const;

  /// \brief Axis-aligned bounds of all vertices. Invalid for an empty mesh.
  plBoundingBox ComputeBounds() const;

  /// \brief Render geometry, faces sorted by material slot.
  ///
  /// Faces are ear-clipped in their own plane, so concave faces triangulate correctly. Corners whose
  /// faces meet at less than smoothingAngle share an averaged normal (matched by position, so separate
  /// shells smooth too); HardEdges faces and a zero angle stay flat. Projected UVs use local positions,
  /// or pUVSpace-transformed positions when given (world-space tiling). out_ranges receives one entry
  /// per material slot that has faces; polygon order equals triangle order in the final index buffer.
  void BuildRenderGeometry(plGeometry& out_geom, plDynamicArray<plBrushSubMeshRange>& out_ranges, plAngle smoothingAngle = plAngle(), const plTransform* pUVSpace = nullptr) const;

  /// \brief Freezes every projected UV into explicit per-corner UVs (as BuildRenderGeometry would compute
  /// them), so the mapping survives moving the vertices into another space.
  void ConvertToExplicitUVs(const plTransform* pUVSpace = nullptr);

  /// \brief Transforms every vertex. A mirroring transform also reverses every face so normals stay outward.
  void TransformVertices(const plTransform& transform);

  /// \brief Triangulates one face into corner-index triples (indices into m_Corners, not vertices).
  /// Ear clipping in the face plane; falls back to a fan for degenerate loops.
  void TriangulateFace(plUInt32 uiFaceIndex, plDynamicArray<plUInt32>& out_cornerTriangles) const;

  /// \brief Largest distance of any corner from the face's best-fit plane.
  float ComputeFacePlanarityError(plUInt32 uiFaceIndex) const;

  float ComputeFaceArea(plUInt32 uiFaceIndex) const;

  /// \brief The shell index containing a face, or plInvalidIndex for meshes without shell records.
  plUInt32 FindShellOfFace(plUInt32 uiFaceIndex) const;

  /// \brief Structural validity: corner indices in range, >= 3 corners per face, shells covering the
  /// face array in order. Anything that fails this must never reach geometry generation.
  bool IsStructurallyValid() const;

  void Serialize(plStreamWriter& inout_stream) const;

  /// \brief Reads any known format version and validates the result. A damaged or truncated blob
  /// leaves the mesh empty and fails.
  plResult Deserialize(plStreamReader& inout_stream);

  void ToDataBuffer(plDataBuffer& out_buffer) const;
  plResult FromDataBuffer(const plDataBuffer& buffer);

  /// \brief Derives the Valve-220 world-aligned projection axes for a normal: the dominant axis picks the
  /// plane, so axis-aligned faces tile exactly and tilted faces get the nearest cardinal projection.
  static void GetWorldAlignedUVAxes(const plVec3& vNormal, plVec3& out_vUAxis, plVec3& out_vVAxis);

  /// \brief The projection axes of a face (world-aligned or authored), rotated by its UV rotation.
  void GetFaceUVAxes(plUInt32 uiFaceIndex, const plVec3& vNormal, plVec3& out_vUAxis, plVec3& out_vVAxis) const;

  static plVec2 ComputeProjectedUV(const plBrushFaceUV& uv, const plVec3& vUAxis, const plVec3& vVAxis, const plVec3& vPos);

  plDynamicArray<plVec3> m_Vertices;
  plDynamicArray<plBrushFace> m_Faces;
  plDynamicArray<plBrushShell> m_Shells;
  plUInt32 m_uiNextFaceId = 1;
};
