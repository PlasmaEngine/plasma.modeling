#pragma once

#include <ModelingPlugin/Modeling/EditableMesh.h>

namespace plModelingTestUtils
{
  /// \brief Signed volume by the divergence theorem over every face (internal faces included — coincident
  /// opposite pairs cancel). Positive for outward-facing closed meshes.
  inline double ComputeVolume(const plEditableMesh& mesh)
  {
    double fVolume = 0.0;
    plDynamicArray<plUInt32> tris;

    for (plUInt32 uiFace = 0; uiFace < mesh.m_Faces.GetCount(); ++uiFace)
    {
      mesh.TriangulateFace(uiFace, tris);
      const plBrushFace& face = mesh.m_Faces[uiFace];

      for (plUInt32 t = 0; t + 2 < tris.GetCount(); t += 3)
      {
        const plVec3& a = mesh.m_Vertices[face.m_Corners[tris[t]]];
        const plVec3& b = mesh.m_Vertices[face.m_Corners[tris[t + 1]]];
        const plVec3& c = mesh.m_Vertices[face.m_Corners[tris[t + 2]]];

        fVolume += static_cast<double>(a.Dot(b.CrossRH(c))) / 6.0;
      }
    }

    return fVolume;
  }

  /// \brief True when every edge (by vertex index) is used exactly once in each direction — a closed,
  /// consistently wound surface without T-junctions.
  inline bool IsClosedByIndex(const plEditableMesh& mesh)
  {
    plHashTable<plUInt64, plInt32> directed;

    for (const plBrushFace& face : mesh.m_Faces)
    {
      for (plUInt32 i = 0; i < face.m_Corners.GetCount(); ++i)
      {
        const plUInt32 a = face.m_Corners[i];
        const plUInt32 b = face.m_Corners[(i + 1) % face.m_Corners.GetCount()];
        directed[(static_cast<plUInt64>(a) << 32) | b]++;
      }
    }

    for (auto it : directed)
    {
      const plUInt32 a = static_cast<plUInt32>(it.Key() >> 32);
      const plUInt32 b = static_cast<plUInt32>(it.Key() & 0xFFFFFFFF);

      if (it.Value() != 1)
        return false;

      const plInt32* pReverse = directed.GetValue((static_cast<plUInt64>(b) << 32) | a);

      if (pReverse == nullptr || *pReverse != 1)
        return false;
    }

    return true;
  }

  inline bool HasFaceId(const plEditableMesh& mesh, plUInt32 uiFaceId)
  {
    for (const plBrushFace& face : mesh.m_Faces)
    {
      if (face.m_uiFaceId == uiFaceId)
        return true;
    }

    return false;
  }

  inline plEditableMesh MakeBox(const plVec3& vMin, const plVec3& vMax)
  {
    plEditableMesh mesh;
    mesh.BeginShell(true);

    const plUInt32 c[8] = {
      mesh.AddVertex(plVec3(vMin.x, vMin.y, vMin.z)),
      mesh.AddVertex(plVec3(vMax.x, vMin.y, vMin.z)),
      mesh.AddVertex(plVec3(vMax.x, vMax.y, vMin.z)),
      mesh.AddVertex(plVec3(vMin.x, vMax.y, vMin.z)),
      mesh.AddVertex(plVec3(vMin.x, vMin.y, vMax.z)),
      mesh.AddVertex(plVec3(vMax.x, vMin.y, vMax.z)),
      mesh.AddVertex(plVec3(vMax.x, vMax.y, vMax.z)),
      mesh.AddVertex(plVec3(vMin.x, vMax.y, vMax.z)),
    };

    const plUInt32 faces[6][4] = {
      {c[4], c[5], c[6], c[7]}, {c[3], c[2], c[1], c[0]}, {c[1], c[2], c[6], c[5]},
      {c[3], c[0], c[4], c[7]}, {c[2], c[3], c[7], c[6]}, {c[0], c[1], c[5], c[4]}};

    for (plUInt32 i = 0; i < 6; ++i)
    {
      mesh.AddFace(plArrayPtr<const plUInt32>(faces[i], 4), i + 1);
    }

    return mesh;
  }
} // namespace plModelingTestUtils
