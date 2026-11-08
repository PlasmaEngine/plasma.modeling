#include <ModelingPlugin/ModelingPluginPCH.h>

#include <ModelingPlugin/Modeling/MeshTopology.h>

void plMeshTopology::Build(const plEditableMesh& mesh)
{
  m_HalfEdges.Clear();
  m_FaceFirstHalfEdge.Clear();
  m_FaceCornerCount.Clear();
  m_Directed.Clear();
  m_NonManifold.Clear();
  m_VertexFaces.Clear();

  m_VertexFaces.SetCount(mesh.m_Vertices.GetCount());
  m_FaceFirstHalfEdge.SetCountUninitialized(mesh.m_Faces.GetCount());
  m_FaceCornerCount.SetCountUninitialized(mesh.m_Faces.GetCount());

  plHashTable<plUInt64, plUInt32> undirectedUses;

  for (plUInt32 uiFace = 0; uiFace < mesh.m_Faces.GetCount(); ++uiFace)
  {
    const plBrushFace& face = mesh.m_Faces[uiFace];
    const plUInt32 n = face.m_Corners.GetCount();
    const plUInt32 uiFirst = m_HalfEdges.GetCount();

    m_FaceFirstHalfEdge[uiFace] = uiFirst;
    m_FaceCornerCount[uiFace] = n;

    for (plUInt32 i = 0; i < n; ++i)
    {
      HalfEdge& he = m_HalfEdges.ExpandAndGetRef();
      he.m_uiFrom = face.m_Corners[i];
      he.m_uiTo = face.m_Corners[(i + 1) % n];
      he.m_uiFace = uiFace;
      he.m_uiCorner = i;
      he.m_uiNext = uiFirst + (i + 1) % n;
      he.m_uiPrev = uiFirst + (i + n - 1) % n;

      const plUInt64 uiDirected = (static_cast<plUInt64>(he.m_uiFrom) << 32) | he.m_uiTo;

      if (m_Directed.Contains(uiDirected))
      {
        // two faces running the same way along an edge: inconsistent winding or more than two faces
        m_NonManifold.Insert(MakeKey(he.m_uiFrom, he.m_uiTo));
      }
      else
      {
        m_Directed.Insert(uiDirected, uiFirst + i);
      }

      undirectedUses[MakeKey(he.m_uiFrom, he.m_uiTo)]++;

      auto& vertexFaces = m_VertexFaces[he.m_uiFrom];
      if (vertexFaces.IsEmpty() || vertexFaces.PeekBack() != uiFace)
      {
        if (!vertexFaces.Contains(uiFace))
          vertexFaces.PushBack(uiFace);
      }
    }
  }

  for (auto it : undirectedUses)
  {
    if (it.Value() > 2)
      m_NonManifold.Insert(it.Key());
  }

  for (plUInt32 h = 0; h < m_HalfEdges.GetCount(); ++h)
  {
    HalfEdge& he = m_HalfEdges[h];

    if (m_NonManifold.Contains(MakeKey(he.m_uiFrom, he.m_uiTo)))
      continue;

    plUInt32 uiTwin = c_uiInvalid;
    if (m_Directed.TryGetValue((static_cast<plUInt64>(he.m_uiTo) << 32) | he.m_uiFrom, uiTwin))
    {
      he.m_uiTwin = uiTwin;
    }
  }
}

plUInt32 plMeshTopology::FindHalfEdge(plUInt32 uiFrom, plUInt32 uiTo) const
{
  plUInt32 uiHalfEdge = c_uiInvalid;
  m_Directed.TryGetValue((static_cast<plUInt64>(uiFrom) << 32) | uiTo, uiHalfEdge);
  return uiHalfEdge;
}

plUInt32 plMeshTopology::FindEdge(plUInt32 uiV0, plUInt32 uiV1) const
{
  const plUInt32 uiHalfEdge = FindHalfEdge(uiV0, uiV1);
  return uiHalfEdge != c_uiInvalid ? uiHalfEdge : FindHalfEdge(uiV1, uiV0);
}

bool plMeshTopology::IsEdgeManifold(plUInt32 uiHalfEdge) const
{
  return uiHalfEdge < m_HalfEdges.GetCount() && m_HalfEdges[uiHalfEdge].m_uiTwin != c_uiInvalid;
}

bool plMeshTopology::IsClosedManifold() const
{
  for (const HalfEdge& he : m_HalfEdges)
  {
    if (he.m_uiTwin == c_uiInvalid)
      return false;
  }

  return true;
}

plUInt32 plMeshTopology::GetBoundaryEdgeCount() const
{
  plUInt32 uiCount = 0;

  for (plUInt32 h = 0; h < m_HalfEdges.GetCount(); ++h)
  {
    if (IsBoundary(h))
      ++uiCount;
  }

  return uiCount;
}

void plMeshTopology::GetVertexFaces(plUInt32 uiVertex, plDynamicArray<plUInt32>& out_faces) const
{
  out_faces.Clear();

  if (uiVertex < m_VertexFaces.GetCount())
  {
    out_faces.PushBackRange(m_VertexFaces[uiVertex].GetArrayPtr());
  }
}

void plMeshTopology::GetEdgeRing(plUInt32 uiStartHalfEdge, plDynamicArray<RingEdge>& out_edges, plDynamicArray<plUInt32>& out_quads, bool& out_bClosed) const
{
  out_edges.Clear();
  out_quads.Clear();
  out_bClosed = false;

  if (uiStartHalfEdge >= m_HalfEdges.GetCount())
    return;

  const HalfEdge& start = m_HalfEdges[uiStartHalfEdge];

  {
    RingEdge& e = out_edges.ExpandAndGetRef();
    e.m_uiA = start.m_uiFrom;
    e.m_uiB = start.m_uiTo;
  }

  const plUInt32 uiGuard = m_HalfEdges.GetCount();

  // forwards: the current half-edge always runs from side A to side B
  plUInt32 uiCur = uiStartHalfEdge;

  for (plUInt32 uiStep = 0; uiStep < uiGuard; ++uiStep)
  {
    const HalfEdge& cur = m_HalfEdges[uiCur];

    if (m_FaceCornerCount[cur.m_uiFace] != 4)
      break;

    const plUInt32 uiOpposite = m_HalfEdges[cur.m_uiNext].m_uiNext;
    const HalfEdge& opp = m_HalfEdges[uiOpposite];

    // the opposite edge runs B-side to A-side
    if (opp.m_uiTwin == uiStartHalfEdge)
    {
      out_quads.PushBack(cur.m_uiFace);
      out_bClosed = true;
      return;
    }

    out_quads.PushBack(cur.m_uiFace);

    RingEdge& e = out_edges.ExpandAndGetRef();
    e.m_uiA = opp.m_uiTo;
    e.m_uiB = opp.m_uiFrom;

    if (opp.m_uiTwin == c_uiInvalid)
      break;

    uiCur = opp.m_uiTwin;
  }

  // backwards from the start's other face: the current half-edge runs B-side to A-side
  if (start.m_uiTwin == c_uiInvalid)
    return;

  uiCur = start.m_uiTwin;

  plDynamicArray<RingEdge> backEdges;
  plDynamicArray<plUInt32> backQuads;

  for (plUInt32 uiStep = 0; uiStep < uiGuard; ++uiStep)
  {
    const HalfEdge& cur = m_HalfEdges[uiCur];

    if (m_FaceCornerCount[cur.m_uiFace] != 4)
      break;

    const plUInt32 uiOpposite = m_HalfEdges[cur.m_uiNext].m_uiNext;
    const HalfEdge& opp = m_HalfEdges[uiOpposite];

    backQuads.PushBack(cur.m_uiFace);

    RingEdge& e = backEdges.ExpandAndGetRef();
    e.m_uiA = opp.m_uiFrom;
    e.m_uiB = opp.m_uiTo;

    if (opp.m_uiTwin == c_uiInvalid)
      break;

    uiCur = opp.m_uiTwin;
  }

  // prepend in reverse so the ring reads end to end
  plDynamicArray<RingEdge> edges;
  plDynamicArray<plUInt32> quads;

  for (plUInt32 i = backEdges.GetCount(); i > 0; --i)
    edges.PushBack(backEdges[i - 1]);
  for (plUInt32 i = backQuads.GetCount(); i > 0; --i)
    quads.PushBack(backQuads[i - 1]);

  edges.PushBackRange(out_edges.GetArrayPtr());
  quads.PushBackRange(out_quads.GetArrayPtr());

  out_edges.Swap(edges);
  out_quads.Swap(quads);
}

void plMeshTopology::GetEdgeLoop(plUInt32 uiStartHalfEdge, plDynamicArray<plUInt32>& out_halfEdges) const
{
  out_halfEdges.Clear();

  if (uiStartHalfEdge >= m_HalfEdges.GetCount())
    return;

  out_halfEdges.PushBack(uiStartHalfEdge);

  const plUInt32 uiGuard = m_HalfEdges.GetCount();

  // continues straight through a valence-4 vertex: next -> twin -> next
  auto step = [&](plUInt32 uiHalfEdge) -> plUInt32
  {
    const HalfEdge& he = m_HalfEdges[uiHalfEdge];

    if (m_VertexFaces[he.m_uiTo].GetCount() != 4)
      return c_uiInvalid;

    const plUInt32 uiNext = he.m_uiNext;
    const plUInt32 uiTwin = m_HalfEdges[uiNext].m_uiTwin;

    if (uiTwin == c_uiInvalid)
      return c_uiInvalid;

    return m_HalfEdges[uiTwin].m_uiNext;
  };

  plUInt32 uiCur = uiStartHalfEdge;

  for (plUInt32 uiStep = 0; uiStep < uiGuard; ++uiStep)
  {
    const plUInt32 uiNext = step(uiCur);

    // closed loop: done
    if (uiNext == uiStartHalfEdge)
      return;

    if (uiNext == c_uiInvalid)
      break;

    out_halfEdges.PushBack(uiNext);
    uiCur = uiNext;
  }

  // open loop: also extend backwards from the start's origin
  const plUInt32 uiStartTwin = m_HalfEdges[uiStartHalfEdge].m_uiTwin;

  if (uiStartTwin == c_uiInvalid)
    return;

  plDynamicArray<plUInt32> backwards;
  uiCur = uiStartTwin;

  for (plUInt32 uiStep = 0; uiStep < uiGuard; ++uiStep)
  {
    const plUInt32 uiNext = step(uiCur);

    if (uiNext == c_uiInvalid || uiNext == uiStartTwin)
      break;

    // stored in the forward direction of the loop
    const plUInt32 uiTwin = m_HalfEdges[uiNext].m_uiTwin;
    backwards.PushBack(uiTwin != c_uiInvalid ? uiTwin : uiNext);
    uiCur = uiNext;
  }

  plDynamicArray<plUInt32> ordered;
  for (plUInt32 i = backwards.GetCount(); i > 0; --i)
    ordered.PushBack(backwards[i - 1]);
  ordered.PushBackRange(out_halfEdges.GetArrayPtr());
  out_halfEdges.Swap(ordered);
}


PL_STATICLINK_FILE(ModelingPlugin, ModelingPlugin_Modeling_Implementation_MeshTopology);
