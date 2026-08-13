#pragma once

#include <ModelingPlugin/ModelingPluginDLL.h>

#include <Foundation/Math/Plane.h>
#include <Foundation/Math/Transform.h>
#include <ModelingPlugin/Modeling/EditableMesh.h>

/// \brief The brush CSG kernel. Everything reduces to "clip a mesh by a plane with a cap", which is
/// why the contract is: THE CUTTER MUST BE CONVEX, THE TARGET CAN BE ANY CLOSED MESH.
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
  /// Fragments inherit face id, material slot and UV mapping. Cap faces take ids from ref_uiNextFaceId
  /// (advanced by the number of caps emitted) and the given material slot. Intersection vertices are
  /// welded exactly via edge keys, so shared edges never crack.
  PL_MODELINGPLUGIN_DLL void ClipByPlane(const plEditableMesh& in, const plPlane& plane, bool bKeepPositiveSide, plUInt8 uiCapMaterialSlot, plUInt32& ref_uiNextFaceId, plEditableMesh& out);

  /// \brief Target minus the convex volume described by the planes (target-local space).
  /// An empty result is valid — the target was consumed entirely.
  PL_MODELINGPLUGIN_DLL void SubtractConvexPlanes(const plEditableMesh& target, plArrayPtr<const plPlane> cutterPlanes, plUInt8 uiInteriorSlot, plEditableMesh& out);

  /// \brief Subtracts every shell of the cutter from the target. Fails (target copied unchanged) when
  /// any transformed cutter shell is not convex — the caller offers decomposition, never silent wrong
  /// geometry.
  PL_MODELINGPLUGIN_DLL plResult SubtractCutter(const plEditableMesh& target, const plEditableMesh& cutter, const plTransform& cutterToTarget, plUInt8 uiInteriorSlot, plEditableMesh& out);

  /// \brief Convex shells become rooms: each is replaced by walls of the given thickness. Shells too
  /// small for the thickness (or non-convex) stay solid; returns failure if NO shell could be hollowed.
  PL_MODELINGPLUGIN_DLL plResult MakeHollow(const plEditableMesh& mesh, float fThickness, plUInt8 uiInteriorSlot, plEditableMesh& out);
} // namespace plMeshCSG
