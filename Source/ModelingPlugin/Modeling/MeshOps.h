#pragma once

#include <ModelingPlugin/ModelingPluginDLL.h>

#include <Foundation/Containers/HashSet.h>
#include <Foundation/Types/Status.h>
#include <ModelingPlugin/Modeling/EditableMesh.h>

/// \brief The polygon operation library. Every operation works on a base (authored) mesh in place and
/// either succeeds or returns a failure status with a user-facing message, leaving the mesh untouched.
///
/// Faces are addressed by id (a set of ids selects every face carrying them), vertices by index. New
/// faces get fresh ids from the mesh's counter and inherit the material slot and UV mapping of the face
/// they grew from. Vertex indices do not survive an operation: callers re-derive vertex and edge
/// selections afterwards, face-id selections stay valid.
namespace plMeshOps
{
  // ---- bookkeeping ------------------------------------------------------------

  /// \brief The shell index of every face (0 for meshes without shell records).
  PL_MODELINGPLUGIN_DLL void GetFaceShells(const plEditableMesh& mesh, plDynamicArray<plUInt32>& out_faceShells);

  /// \brief Reorders faces so each shell's faces are contiguous again, given one shell index per face.
  /// Empty shells are dropped.
  PL_MODELINGPLUGIN_DLL void RebuildShells(plEditableMesh& inout_mesh, plArrayPtr<const plUInt32> faceShells);

  /// \brief Drops vertices no face references and renumbers the corners.
  PL_MODELINGPLUGIN_DLL void RemoveUnusedVertices(plEditableMesh& inout_mesh);

  /// \brief Merges vertices closer than fDistance and removes the degenerate corners and faces that leaves.
  PL_MODELINGPLUGIN_DLL void WeldByDistance(plEditableMesh& inout_mesh, float fDistance = 0.0001f);

  PL_MODELINGPLUGIN_DLL void GatherFaceIndices(const plEditableMesh& mesh, const plHashSet<plUInt32>& faceIds, plDynamicArray<plUInt32>& out_faceIndices);

  /// \brief Groups faces into surfaces: what reads as one face on screen. Faces join a surface when they lie
  /// in the same plane, use the same material slot and touch (share a corner, or a corner lies on the
  /// other's edge), and all faces with the same id share one surface, so a selection by id never splits
  /// one. CSG pieces of a cut face and the triangles of an imported quad come out as one surface.
  /// out_surfaceOfFace receives a surface index per face (plInvalidIndex for internal faces). Returns the
  /// number of surfaces.
  PL_MODELINGPLUGIN_DLL plUInt32 ComputeSurfaces(const plEditableMesh& mesh, plDynamicArray<plUInt32>& out_surfaceOfFace);

  // ---- face operations ----------------------------------------------------------

  /// \brief Pushes faces out along their normals, adding side walls. Region: connected faces move
  /// together and only the region's outline gets walls. Individual: every face gets its own walls.
  PL_MODELINGPLUGIN_DLL plStatus Extrude(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& faceIds, float fDistance, bool bRegion);

  /// \brief Shrinks each face inward by fAmount (a distance along the face), bridging to the old
  /// outline with a ring of quads. The inner face keeps the id.
  PL_MODELINGPLUGIN_DLL plStatus Inset(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& faceIds, float fAmount);

  PL_MODELINGPLUGIN_DLL plStatus Flip(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& faceIds);

  /// \brief Splits each face into quads around its centre. Neighbours get the new edge midpoints, so no
  /// T-junctions appear.
  PL_MODELINGPLUGIN_DLL plStatus Subdivide(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& faceIds);

  PL_MODELINGPLUGIN_DLL plStatus DeleteFaces(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& faceIds);

  /// \brief Replaces two faces with a tube of quads connecting their outlines. Both need the same corner count.
  PL_MODELINGPLUGIN_DLL plStatus Bridge(plEditableMesh& inout_mesh, plUInt32 uiFaceIdA, plUInt32 uiFaceIdB);

  /// \brief Moves the faces into out_detached (same space) and removes them here.
  PL_MODELINGPLUGIN_DLL plStatus Detach(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& faceIds, plEditableMesh& out_detached);

  /// \brief Splits faces into triangles. All pieces keep the face's id, so per-face materials stay.
  PL_MODELINGPLUGIN_DLL plStatus Triangulate(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& faceIds);

  /// \brief Projects the corners of each face onto its best-fit plane.
  PL_MODELINGPLUGIN_DLL plStatus Flatten(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& faceIds);

  // ---- edge operations ----------------------------------------------------------

  /// \brief Chamfers manifold edges whose end vertices touch exactly three faces (box-like corners).
  /// uiSegments > 1 rounds the chamfer. Selected edges must not share vertices.
  PL_MODELINGPLUGIN_DLL plStatus Bevel(plEditableMesh& inout_mesh, plArrayPtr<const plUInt64> edgeKeys, float fWidth, plUInt32 uiSegments);

  /// \brief Cuts the ring of quads through edge {v0, v1} with uiCuts evenly spaced loops, or one loop at
  /// fPosition (0..1 from v0's side) when uiCuts is 1.
  PL_MODELINGPLUGIN_DLL plStatus LoopCut(plEditableMesh& inout_mesh, plUInt32 uiV0, plUInt32 uiV1, plUInt32 uiCuts, float fPosition);

  /// \brief Closes the open boundary loop that contains the edge {v0, v1} with a new face.
  PL_MODELINGPLUGIN_DLL plStatus FillHole(plEditableMesh& inout_mesh, plUInt32 uiV0, plUInt32 uiV1);

  // ---- vertex operations --------------------------------------------------------

  /// \brief Merges the vertices into one at their centre, removing collapsed corners and faces.
  PL_MODELINGPLUGIN_DLL plStatus Weld(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& vertices);

  /// \brief Splits the face that contains both vertices along the line between them.
  PL_MODELINGPLUGIN_DLL plStatus Connect(plEditableMesh& inout_mesh, plUInt32 uiV0, plUInt32 uiV1);

  /// \brief Deletes every face using one of the vertices.
  PL_MODELINGPLUGIN_DLL plStatus DeleteVertices(plEditableMesh& inout_mesh, const plHashSet<plUInt32>& vertices);
} // namespace plMeshOps
