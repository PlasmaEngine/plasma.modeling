#pragma once

#include <ModelingPlugin/ModelingPluginDLL.h>

#include <Foundation/Math/Plane.h>
#include <Foundation/Math/Transform.h>
#include <ModelingPlugin/Modeling/EditableMesh.h>

/// \brief A convex volume of a room carve, in the target's space. Its faces point out of the volume.
struct PL_MODELINGPLUGIN_DLL plCsgConvexVolume
{
  /// \brief Takes one closed convex shell and derives its planes.
  void SetShell(const plEditableMesh& shellMesh);

  plEditableMesh m_Mesh;            ///< exactly one shell
  plDynamicArray<plPlane> m_Planes; ///< outward, coplanar faces merged
};

/// \brief One convex cutter shell taking part in a room carve.
struct PL_MODELINGPLUGIN_DLL plCsgRoomCutter
{
  plCsgConvexVolume m_Volume;
  plUInt32 m_uiCutterId = 0;
  plUInt32 m_uiShell = 0;
};

/// \brief The empty space of one room (an inside-out shell), split into convex volumes.
struct PL_MODELINGPLUGIN_DLL plCsgRoom
{
  plDynamicArray<plCsgConvexVolume> m_Pieces;
  plInt32 m_iTargetShell = -1; ///< the target's shell this room is, -1 for a room of another brush
};

/// \brief The brush CSG kernel. Everything reduces to "clip a mesh by a plane with a cap", which is
/// why the contract is: THE CUTTER MUST BE CONVEX, THE TARGET CAN BE ANY CLOSED MESH.
///
/// Rooms are the exception: a shell whose faces point inward encloses empty space and its solid is
/// everything around it, so a plane through it has no bounded cap. Rooms are carved as surfaces instead
/// (CarveRooms).
///
/// Subtraction is the Quake carve: target minus convex cutter = the union over the cutter's planes of
/// (target outside plane i, inside planes 1..i-1). Faces split by a plane hand their id and attributes
/// to every fragment, so per-face materials survive; cap faces get fresh ids from a shared counter, so
/// identical inputs produce identical ids on every evaluation — which is what keeps FaceData overrides
/// on doorway jambs stable.
namespace plMeshCSG
{
  /// \brief True when every vertex lies on or behind every face plane of the shell.
  PL_MODELINGPLUGIN_DLL bool IsShellConvex(const plEditableMesh& mesh, const plBrushShell& shell, float fEpsilon = 0.01f);

  /// \brief The outward-facing plane of every face of a shell.
  PL_MODELINGPLUGIN_DLL void GetShellPlanes(const plEditableMesh& mesh, const plBrushShell& shell, plDynamicArray<plPlane>& out_planes);

  /// \brief Clips the whole mesh by a plane, keeping one side and capping every cut shell.
  ///
  /// Fragments inherit face id, flags, material slot and UV mapping (explicit UVs are interpolated). Cap
  /// faces take uiFixedCapFaceId, or ids from ref_uiNextFaceId when it is 0, and the given material slot.
  /// Intersection vertices are welded exactly via edge keys, so shared edges never crack; cap loops chain
  /// by position, so meshes with T-junctions still cap cleanly.
  PL_MODELINGPLUGIN_DLL void ClipByPlane(const plEditableMesh& in, const plPlane& plane, bool bKeepPositiveSide, plUInt8 uiCapMaterialSlot, plUInt32& ref_uiNextFaceId, plEditableMesh& out, plUInt32 uiFixedCapFaceId = 0);

  /// \brief Target minus the convex volume described by the planes (target-local space).
  /// An empty result is valid — the target was consumed entirely. With a cutter id, the faces each plane
  /// leaves behind get plBrushFaceIds::MakeCutFaceId ids, independent of evaluation order.
  PL_MODELINGPLUGIN_DLL void SubtractConvexPlanes(const plEditableMesh& target, plArrayPtr<const plPlane> cutterPlanes, plUInt8 uiInteriorSlot, plEditableMesh& out, plUInt32 uiCutterId = 0, plUInt32 uiCutterShell = 0);

  /// \brief Subtracts every shell of the cutter from the target. Fails (target copied unchanged) when
  /// any transformed cutter shell is not convex — the caller offers decomposition, never silent wrong
  /// geometry.
  PL_MODELINGPLUGIN_DLL plResult SubtractCutter(const plEditableMesh& target, const plEditableMesh& cutter, const plTransform& cutterToTarget, plUInt8 uiInteriorSlot, plEditableMesh& out, plUInt32 uiCutterId = 0);

  /// \brief Convex shells become rooms: each is replaced by walls of the given thickness. Shells too
  /// small for the thickness (or non-convex) stay solid; returns failure if NO shell could be hollowed.
  PL_MODELINGPLUGIN_DLL plResult MakeHollow(const plEditableMesh& mesh, float fThickness, plUInt8 uiInteriorSlot, plEditableMesh& out);

  /// \brief Splits every non-convex shell along its own face planes until each piece is convex. The faces
  /// between pieces are marked Internal. Returns the number of pieces written.
  PL_MODELINGPLUGIN_DLL plUInt32 DecomposeConvex(const plEditableMesh& mesh, plEditableMesh& out, plUInt32 uiMaxPieces = 64);

  PL_MODELINGPLUGIN_DLL bool AreAllShellsConvex(const plEditableMesh& mesh);

  /// \brief A cutter any shape can take: convex shells pass through, the others are split into convex
  /// pieces (the faces between pieces marked Internal). Fails only when a shell cannot be split into
  /// convex pieces, e.g. because it is not closed.
  PL_MODELINGPLUGIN_DLL plResult MakeConvexCutter(const plEditableMesh& cutter, plEditableMesh& out);

  /// \brief A + B as one closed mesh in A's space: the convex operand cuts the other (A + (B - A)), and
  /// the cut faces are marked Internal. A keeps its face ids. Fails when neither operand is convex.
  PL_MODELINGPLUGIN_DLL plResult Union(const plEditableMesh& a, const plEditableMesh& b, const plTransform& bToA, plEditableMesh& out);

  /// \brief True when the shell encloses negative volume: its faces point inward, so it is a room.
  PL_MODELINGPLUGIN_DLL bool IsShellInverted(const plEditableMesh& mesh, const plBrushShell& shell);

  PL_MODELINGPLUGIN_DLL bool HasInvertedShell(const plEditableMesh& mesh);

  /// \brief The empty space of a room shell as convex volumes (the shell turned outward, decomposed when
  /// it is not convex).
  PL_MODELINGPLUGIN_DLL void MakeRoomPieces(const plEditableMesh& mesh, plUInt32 uiShell, plDynamicArray<plCsgConvexVolume>& out_pieces);

  /// \brief Carves every room shell of the target: the result is the surface of the solid around the
  /// rooms and cutters, seen from inside.
  ///
  /// A room keeps its faces outside every cutter. The faces of a cutter that lie outside every room and
  /// every other cutter become the walls of the passage it opens, facing into it. Each cutter's passage
  /// belongs to exactly one room, the first one in `rooms` it overlaps, so rooms sharing a cutter never
  /// draw it twice. `rooms` lists the target's own room shells (m_iTargetShell) and the rooms of other
  /// brushes, in the same order for every brush. Solid shells of the target are not written to `out`.
  PL_MODELINGPLUGIN_DLL void CarveRooms(const plEditableMesh& target, plArrayPtr<const plCsgRoomCutter> cutters, plArrayPtr<const plCsgRoom> rooms, plUInt8 uiInteriorSlot, plEditableMesh& out);
} // namespace plMeshCSG
