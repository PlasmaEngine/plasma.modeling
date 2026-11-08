#pragma once

#include <ModelingPlugin/ModelingPluginDLL.h>

#include <Foundation/Containers/HashSet.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Containers/HybridArray.h>
#include <ModelingPlugin/Modeling/EditableMesh.h>

/// \brief A half-edge view derived from an plEditableMesh's face loops. Never stored, rebuilt when needed.
///
/// Half-edge h of face f corner i runs from corner i to corner i+1. Vertices are matched by index, so
/// T-junctions and coincident-but-separate vertices show up as boundary edges. Edges shared by more than
/// two faces keep an invalid twin and count as non-manifold; operations that need manifold input check
/// IsEdgeManifold and refuse instead of guessing.
class PL_MODELINGPLUGIN_DLL plMeshTopology
{
public:
  static constexpr plUInt32 c_uiInvalid = plInvalidIndex;

  struct HalfEdge
  {
    plUInt32 m_uiFrom = c_uiInvalid;
    plUInt32 m_uiTo = c_uiInvalid;
    plUInt32 m_uiFace = c_uiInvalid;
    plUInt32 m_uiCorner = c_uiInvalid; ///< index of m_uiFrom within the face's corner list
    plUInt32 m_uiNext = c_uiInvalid;
    plUInt32 m_uiPrev = c_uiInvalid;
    plUInt32 m_uiTwin = c_uiInvalid;
  };

  void Build(const plEditableMesh& mesh);

  plUInt32 GetHalfEdgeCount() const { return m_HalfEdges.GetCount(); }
  const HalfEdge& GetHalfEdge(plUInt32 uiHalfEdge) const { return m_HalfEdges[uiHalfEdge]; }

  /// \brief The half-edge of a face's corner (from corner to the next corner).
  plUInt32 GetFaceHalfEdge(plUInt32 uiFace, plUInt32 uiCorner) const { return m_FaceFirstHalfEdge[uiFace] + uiCorner; }

  /// \brief The directed half-edge v0 -> v1, or c_uiInvalid.
  plUInt32 FindHalfEdge(plUInt32 uiFrom, plUInt32 uiTo) const;

  /// \brief Any half-edge of the undirected edge {v0, v1}, or c_uiInvalid.
  plUInt32 FindEdge(plUInt32 uiV0, plUInt32 uiV1) const;

  /// \brief Exactly two faces share the edge, with opposite directions.
  bool IsEdgeManifold(plUInt32 uiHalfEdge) const;

  bool IsBoundary(plUInt32 uiHalfEdge) const { return m_HalfEdges[uiHalfEdge].m_uiTwin == c_uiInvalid && !m_NonManifold.Contains(MakeKey(m_HalfEdges[uiHalfEdge].m_uiFrom, m_HalfEdges[uiHalfEdge].m_uiTo)); }

  /// \brief Every edge manifold: a closed, consistently wound surface without T-junctions.
  bool IsClosedManifold() const;

  plUInt32 GetNonManifoldEdgeCount() const { return m_NonManifold.GetCount(); }
  plUInt32 GetBoundaryEdgeCount() const;

  /// \brief The faces touching a vertex.
  void GetVertexFaces(plUInt32 uiVertex, plDynamicArray<plUInt32>& out_faces) const;

  /// \brief One edge of an edge ring: m_uiA lies on the same side of the ring for every edge.
  struct RingEdge
  {
    plUInt32 m_uiA = c_uiInvalid;
    plUInt32 m_uiB = c_uiInvalid;
  };

  /// \brief The ring of parallel edges across quads through the given half-edge: each step jumps to the
  /// opposite edge of the current quad. Stops at non-quads and boundaries; out_bClosed when it comes
  /// back around. out_quads receives the quads between consecutive ring edges, in order.
  void GetEdgeRing(plUInt32 uiStartHalfEdge, plDynamicArray<RingEdge>& out_edges, plDynamicArray<plUInt32>& out_quads, bool& out_bClosed) const;

  /// \brief The loop of edges running end to end through valence-4 vertices. Output: half-edges in order.
  void GetEdgeLoop(plUInt32 uiStartHalfEdge, plDynamicArray<plUInt32>& out_halfEdges) const;

  static plUInt64 MakeKey(plUInt32 uiV0, plUInt32 uiV1)
  {
    const plUInt32 uiLow = plMath::Min(uiV0, uiV1);
    const plUInt32 uiHigh = plMath::Max(uiV0, uiV1);
    return (static_cast<plUInt64>(uiHigh) << 32) | uiLow;
  }

private:
  plDynamicArray<HalfEdge> m_HalfEdges;
  plDynamicArray<plUInt32> m_FaceFirstHalfEdge;
  plDynamicArray<plUInt32> m_FaceCornerCount;
  plHashTable<plUInt64, plUInt32> m_Directed; ///< (from << 32 | to) -> half-edge
  plHashSet<plUInt64> m_NonManifold;          ///< undirected keys of edges with != 2 uses or equal directions
  plDynamicArray<plHybridArray<plUInt32, 6>> m_VertexFaces;
};
