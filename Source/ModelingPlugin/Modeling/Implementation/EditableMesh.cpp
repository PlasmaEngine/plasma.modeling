#include <ModelingPlugin/ModelingPluginPCH.h>

#include <Core/Graphics/Geometry.h>
#include <Foundation/IO/MemoryStream.h>
#include <ModelingPlugin/Modeling/EditableMesh.h>

void plEditableMesh::Clear()
{
  m_Vertices.Clear();
  m_Faces.Clear();
  m_Shells.Clear();
  m_uiNextFaceId = 1;
}

plUInt32 plEditableMesh::AddVertex(const plVec3& vPos)
{
  m_Vertices.PushBack(vPos);
  return m_Vertices.GetCount() - 1;
}

plUInt32 plEditableMesh::AddFace(plArrayPtr<const plUInt32> corners, plUInt32 uiFaceId, plUInt8 uiMaterialSlot)
{
  PL_ASSERT_DEV(corners.GetCount() >= 3, "A face needs at least 3 corners");

  plBrushFace& face = m_Faces.ExpandAndGetRef();
  face.m_uiFaceId = uiFaceId;
  face.m_uiMaterialSlot = uiMaterialSlot;
  face.m_Corners.SetCount(static_cast<plUInt16>(corners.GetCount()));

  for (plUInt32 i = 0; i < corners.GetCount(); ++i)
  {
    face.m_Corners[i] = corners[i];
  }

  m_uiNextFaceId = plMath::Max(m_uiNextFaceId, uiFaceId + 1);

  if (!m_Shells.IsEmpty())
  {
    m_Shells.PeekBack().m_uiNumFaces++;
  }

  return m_Faces.GetCount() - 1;
}

plUInt32 plEditableMesh::AddFace(plArrayPtr<const plVec3> positions, plUInt32 uiFaceId, plUInt8 uiMaterialSlot)
{
  plSmallArray<plUInt32, 8> corners;

  for (const plVec3& pos : positions)
  {
    corners.PushBack(AddVertex(pos));
  }

  return AddFace(plArrayPtr<const plUInt32>(corners.GetData(), corners.GetCount()), uiFaceId, uiMaterialSlot);
}

void plEditableMesh::BeginShell(bool bConvex, bool bSubtractive)
{
  plBrushShell& shell = m_Shells.ExpandAndGetRef();
  shell.m_uiFirstFace = m_Faces.GetCount();
  shell.m_uiNumFaces = 0;
  shell.m_bConvex = bConvex;
  shell.m_bSubtractive = bSubtractive;
}

plVec3 plEditableMesh::ComputeFaceNormal(plUInt32 uiFaceIndex) const
{
  const plBrushFace& face = m_Faces[uiFaceIndex];

  plVec3 vNormal = plVec3::MakeZero();

  for (plUInt32 i = 0; i < face.m_Corners.GetCount(); ++i)
  {
    const plVec3& cur = m_Vertices[face.m_Corners[i]];
    const plVec3& next = m_Vertices[face.m_Corners[(i + 1) % face.m_Corners.GetCount()]];

    vNormal.x += (cur.y - next.y) * (cur.z + next.z);
    vNormal.y += (cur.z - next.z) * (cur.x + next.x);
    vNormal.z += (cur.x - next.x) * (cur.y + next.y);
  }

  vNormal.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();
  return vNormal;
}

plVec3 plEditableMesh::ComputeFaceCenter(plUInt32 uiFaceIndex) const
{
  const plBrushFace& face = m_Faces[uiFaceIndex];

  plVec3 vCenter = plVec3::MakeZero();

  for (plUInt32 uiCorner : face.m_Corners)
  {
    vCenter += m_Vertices[uiCorner];
  }

  return vCenter / static_cast<float>(face.m_Corners.GetCount());
}

plUInt32 plEditableMesh::GetUsedMaterialSlotCount() const
{
  plUInt32 uiMaxSlot = 0;

  for (const plBrushFace& face : m_Faces)
  {
    uiMaxSlot = plMath::Max<plUInt32>(uiMaxSlot, face.m_uiMaterialSlot);
  }

  return uiMaxSlot + 1;
}

void plEditableMesh::GetWorldAlignedUVAxes(const plVec3& vNormal, plVec3& out_vUAxis, plVec3& out_vVAxis)
{
  const float fAbsX = plMath::Abs(vNormal.x);
  const float fAbsY = plMath::Abs(vNormal.y);
  const float fAbsZ = plMath::Abs(vNormal.z);

  if (fAbsZ >= fAbsX && fAbsZ >= fAbsY)
  {
    // floor / ceiling
    out_vUAxis = plVec3(1, 0, 0);
    out_vVAxis = plVec3(0, -1, 0);
  }
  else if (fAbsX >= fAbsY)
  {
    // wall facing +/- X
    out_vUAxis = plVec3(0, 1, 0);
    out_vVAxis = plVec3(0, 0, -1);
  }
  else
  {
    // wall facing +/- Y
    out_vUAxis = plVec3(1, 0, 0);
    out_vVAxis = plVec3(0, 0, -1);
  }
}

void plEditableMesh::BuildRenderGeometry(plGeometry& out_geom, plDynamicArray<plBrushSubMeshRange>& out_ranges, const plColor& color) const
{
  out_ranges.Clear();

  const plUInt32 uiNumSlots = GetUsedMaterialSlotCount();
  plUInt32 uiTriangle = 0;

  for (plUInt32 uiSlot = 0; uiSlot < uiNumSlots; ++uiSlot)
  {
    const plUInt32 uiFirstTriangle = uiTriangle;

    for (plUInt32 uiFace = 0; uiFace < m_Faces.GetCount(); ++uiFace)
    {
      const plBrushFace& face = m_Faces[uiFace];

      if (face.m_uiMaterialSlot != uiSlot)
        continue;

      const plVec3 vNormal = ComputeFaceNormal(uiFace);

      plVec3 vUAxis, vVAxis;
      if (face.m_UV.m_bWorldAligned)
      {
        GetWorldAlignedUVAxes(vNormal, vUAxis, vVAxis);
      }
      else
      {
        vUAxis = face.m_UV.m_vUAxis;
        vVAxis = face.m_UV.m_vVAxis;
      }

      if (face.m_UV.m_Rotation != plAngle())
      {
        // rotate the projection axes around the face normal
        const plMat3 rot = plMat3::MakeAxisRotation(vNormal, face.m_UV.m_Rotation);
        vUAxis = rot * vUAxis;
        vVAxis = rot * vVAxis;
      }

      plSmallArray<plUInt32, 8> geomIndices;

      for (plUInt32 uiCorner : face.m_Corners)
      {
        const plVec3& vPos = m_Vertices[uiCorner];

        const plVec2 vTexCoord = plVec2(
          vPos.Dot(vUAxis) * face.m_UV.m_vScale.x + face.m_UV.m_vOffset.x,
          vPos.Dot(vVAxis) * face.m_UV.m_vScale.y + face.m_UV.m_vOffset.y);

        geomIndices.PushBack(out_geom.AddVertex(vPos, vNormal, vTexCoord, color));
      }

      // fan triangulation — faces are convex polygons
      for (plUInt32 i = 2; i < geomIndices.GetCount(); ++i)
      {
        plUInt32 tri[3] = {geomIndices[0], geomIndices[i - 1], geomIndices[i]};
        out_geom.AddPolygon(plArrayPtr<plUInt32>(tri, 3), false);
        ++uiTriangle;
      }
    }

    if (uiTriangle > uiFirstTriangle)
    {
      plBrushSubMeshRange& range = out_ranges.ExpandAndGetRef();
      range.m_uiFirstTriangle = uiFirstTriangle;
      range.m_uiNumTriangles = uiTriangle - uiFirstTriangle;
      range.m_uiMaterialSlot = static_cast<plUInt8>(uiSlot);
    }
  }
}

// Versioned, append-only. The blob travels through the document mirror and undo, so old data must
// always load.
constexpr plUInt8 c_uiMeshFormatVersion = 1;

void plEditableMesh::Serialize(plStreamWriter& inout_stream) const
{
  inout_stream << c_uiMeshFormatVersion;

  inout_stream << m_Vertices.GetCount();
  for (const plVec3& v : m_Vertices)
  {
    inout_stream << v;
  }

  inout_stream << m_Faces.GetCount();
  for (const plBrushFace& face : m_Faces)
  {
    inout_stream << face.m_uiFaceId;
    inout_stream << face.m_uiMaterialSlot;
    inout_stream << static_cast<plUInt8>(face.m_Corners.GetCount());

    for (plUInt32 uiCorner : face.m_Corners)
    {
      inout_stream << uiCorner;
    }

    inout_stream << face.m_UV.m_vUAxis;
    inout_stream << face.m_UV.m_vVAxis;
    inout_stream << face.m_UV.m_vOffset;
    inout_stream << face.m_UV.m_vScale;
    inout_stream << face.m_UV.m_Rotation;
    inout_stream << face.m_UV.m_bWorldAligned;
  }

  inout_stream << m_Shells.GetCount();
  for (const plBrushShell& shell : m_Shells)
  {
    inout_stream << shell.m_uiFirstFace;
    inout_stream << shell.m_uiNumFaces;
    inout_stream << shell.m_bConvex;
    inout_stream << shell.m_bSubtractive;
  }

  inout_stream << m_uiNextFaceId;
}

plResult plEditableMesh::Deserialize(plStreamReader& inout_stream)
{
  Clear();

  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  if (uiVersion == 0 || uiVersion > c_uiMeshFormatVersion)
    return PL_FAILURE;

  plUInt32 uiCount = 0;

  inout_stream >> uiCount;
  m_Vertices.SetCountUninitialized(uiCount);
  for (plVec3& v : m_Vertices)
  {
    inout_stream >> v;
  }

  inout_stream >> uiCount;
  m_Faces.SetCount(uiCount);
  for (plBrushFace& face : m_Faces)
  {
    inout_stream >> face.m_uiFaceId;
    inout_stream >> face.m_uiMaterialSlot;

    plUInt8 uiCorners = 0;
    inout_stream >> uiCorners;
    face.m_Corners.SetCount(uiCorners);

    for (plUInt32 i = 0; i < uiCorners; ++i)
    {
      inout_stream >> face.m_Corners[i];
    }

    inout_stream >> face.m_UV.m_vUAxis;
    inout_stream >> face.m_UV.m_vVAxis;
    inout_stream >> face.m_UV.m_vOffset;
    inout_stream >> face.m_UV.m_vScale;
    inout_stream >> face.m_UV.m_Rotation;
    inout_stream >> face.m_UV.m_bWorldAligned;
  }

  inout_stream >> uiCount;
  m_Shells.SetCount(uiCount);
  for (plBrushShell& shell : m_Shells)
  {
    inout_stream >> shell.m_uiFirstFace;
    inout_stream >> shell.m_uiNumFaces;
    inout_stream >> shell.m_bConvex;
    inout_stream >> shell.m_bSubtractive;
  }

  inout_stream >> m_uiNextFaceId;

  return PL_SUCCESS;
}

void plEditableMesh::ToDataBuffer(plDataBuffer& out_buffer) const
{
  plDefaultMemoryStreamStorage storage;
  plMemoryStreamWriter writer(&storage);

  Serialize(writer);

  out_buffer.SetCountUninitialized(storage.GetStorageSize32());

  plRawMemoryStreamWriter rawWriter(out_buffer.GetData(), out_buffer.GetCount());
  storage.CopyToStream(rawWriter).IgnoreResult();
}

plResult plEditableMesh::FromDataBuffer(const plDataBuffer& buffer)
{
  if (buffer.IsEmpty())
  {
    Clear();
    return PL_FAILURE;
  }

  plRawMemoryStreamReader reader(buffer.GetData(), buffer.GetCount());
  return Deserialize(reader);
}


PL_STATICLINK_FILE(ModelingPlugin, ModelingPlugin_Modeling_Implementation_EditableMesh);
