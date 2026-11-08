#include <ModelingTest/ModelingTestPCH.h>

#include <Core/Graphics/Geometry.h>
#include <ModelingPlugin/Modeling/BrushFaceAttributes.h>
#include <ModelingTest/TestHelpers.h>

PL_CREATE_SIMPLE_TEST_GROUP(MeshCore);

using namespace plModelingTestUtils;

PL_CREATE_SIMPLE_TEST(MeshCore, Serialization)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Round trip keeps every field")
  {
    plEditableMesh mesh = MakeBox(plVec3(-1), plVec3(1));
    mesh.m_Faces[2].m_uiMaterialSlot = 3;
    mesh.m_Faces[2].m_uiFlags = plBrushFaceFlags::HardEdges;
    mesh.m_Faces[3].m_uiFlags = plBrushFaceFlags::ExplicitUV;
    mesh.m_Faces[3].m_CornerUVs.PushBack(plVec2(0, 0));
    mesh.m_Faces[3].m_CornerUVs.PushBack(plVec2(1, 0));
    mesh.m_Faces[3].m_CornerUVs.PushBack(plVec2(1, 1));
    mesh.m_Faces[3].m_CornerUVs.PushBack(plVec2(0, 1));
    mesh.m_Faces[4].m_UV.m_Rotation = plAngle::MakeFromDegree(30);

    plDataBuffer blob;
    mesh.ToDataBuffer(blob);

    plEditableMesh loaded;
    PL_TEST_BOOL(loaded.FromDataBuffer(blob).Succeeded());
    PL_TEST_INT(loaded.m_Faces.GetCount(), 6);
    PL_TEST_INT(loaded.m_Faces[2].m_uiMaterialSlot, 3);
    PL_TEST_INT(loaded.m_Faces[2].m_uiFlags, plBrushFaceFlags::HardEdges);
    PL_TEST_INT(loaded.m_Faces[3].m_CornerUVs.GetCount(), 4);
    PL_TEST_VEC2(loaded.m_Faces[3].m_CornerUVs[2], plVec2(1, 1), 0.0f);
    PL_TEST_FLOAT(loaded.m_Faces[4].m_UV.m_Rotation.GetDegree(), 30.0f, 0.001f);

    plDataBuffer blob2;
    loaded.ToDataBuffer(blob2);
    PL_TEST_BOOL(blob == blob2);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Faces with more than 255 corners survive")
  {
    plEditableMesh mesh;
    mesh.BeginShell(false);

    plDynamicArray<plVec3> ring;
    for (plUInt32 i = 0; i < 300; ++i)
    {
      const plAngle a = plAngle::MakeFromDegree(360.0f * i / 300.0f);
      ring.PushBack(plVec3(plMath::Cos(a), plMath::Sin(a), 0));
    }
    mesh.AddFace(plArrayPtr<const plVec3>(ring.GetData(), ring.GetCount()), 1);

    plDataBuffer blob;
    mesh.ToDataBuffer(blob);

    plEditableMesh loaded;
    PL_TEST_BOOL(loaded.FromDataBuffer(blob).Succeeded());
    PL_TEST_INT(loaded.m_Faces[0].m_Corners.GetCount(), 300);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Damaged blobs are refused, never crash")
  {
    plEditableMesh mesh = MakeBox(plVec3(-1), plVec3(1));

    plDataBuffer blob;
    mesh.ToDataBuffer(blob);

    // every truncation must fail cleanly
    for (plUInt32 uiLength = 0; uiLength < blob.GetCount(); ++uiLength)
    {
      plDataBuffer truncated;
      truncated.PushBackRange(plArrayPtr<const plUInt8>(blob.GetData(), uiLength));

      plEditableMesh loaded;
      PL_TEST_BOOL(loaded.FromDataBuffer(truncated).Failed());
      PL_TEST_BOOL(loaded.m_Faces.IsEmpty());
    }

    // a corner index pointing past the vertex array
    {
      plEditableMesh bad = mesh;
      bad.m_Faces[0].m_Corners[0] = 999;

      plDataBuffer badBlob;
      bad.ToDataBuffer(badBlob);

      plEditableMesh loaded;
      PL_TEST_BOOL(loaded.FromDataBuffer(badBlob).Failed());
    }

    // random bytes after a valid version byte
    {
      plDataBuffer garbage;
      garbage.PushBack(2);
      for (plUInt32 i = 0; i < 64; ++i)
        garbage.PushBack(static_cast<plUInt8>(i * 37 + 11));

      plEditableMesh loaded;
      PL_TEST_BOOL(loaded.FromDataBuffer(garbage).Failed());
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Version 1 blobs still load")
  {
    // v1: u8 corner count, no face flags
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter w(&storage);

    w << static_cast<plUInt8>(1);
    w << static_cast<plUInt32>(3);
    w << plVec3(0, 0, 0) << plVec3(1, 0, 0) << plVec3(0, 1, 0);
    w << static_cast<plUInt32>(1);
    w << static_cast<plUInt32>(7) << static_cast<plUInt8>(2) << static_cast<plUInt8>(3);
    w << static_cast<plUInt32>(0) << static_cast<plUInt32>(1) << static_cast<plUInt32>(2);
    w << plVec3(1, 0, 0) << plVec3(0, 1, 0) << plVec2(0, 0) << plVec2(1, 1) << 0.0f << true;
    w << static_cast<plUInt32>(1);
    w << static_cast<plUInt32>(0) << static_cast<plUInt32>(1) << false << false;
    w << static_cast<plUInt32>(8);

    plDataBuffer blob;
    blob.SetCountUninitialized(storage.GetStorageSize32());
    plRawMemoryStreamWriter raw(blob.GetData(), blob.GetCount());
    storage.CopyToStream(raw).IgnoreResult();

    plEditableMesh loaded;
    PL_TEST_BOOL(loaded.FromDataBuffer(blob).Succeeded());
    PL_TEST_INT(loaded.m_Faces.GetCount(), 1);
    PL_TEST_INT(loaded.m_Faces[0].m_uiFaceId, 7);
    PL_TEST_INT(loaded.m_Faces[0].m_uiMaterialSlot, 2);
    PL_TEST_INT(loaded.m_uiNextFaceId, 8);
  }
}

PL_CREATE_SIMPLE_TEST(MeshCore, Triangulation)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Concave L face keeps its area")
  {
    plEditableMesh mesh;
    mesh.BeginShell(false);

    // an L in the XY plane, counter-clockwise from +Z: area 3
    const plVec3 corners[] = {plVec3(0, 0, 0), plVec3(2, 0, 0), plVec3(2, 1, 0), plVec3(1, 1, 0), plVec3(1, 2, 0), plVec3(0, 2, 0)};
    mesh.AddFace(plArrayPtr<const plVec3>(corners), 1);

    plDynamicArray<plUInt32> tris;
    mesh.TriangulateFace(0, tris);
    PL_TEST_INT(tris.GetCount(), 12);

    float fArea = 0.0f;
    for (plUInt32 t = 0; t < tris.GetCount(); t += 3)
    {
      const plVec3 a = corners[tris[t]], b = corners[tris[t + 1]], c = corners[tris[t + 2]];
      const plVec3 n = (b - a).CrossRH(c - a);
      PL_TEST_BOOL(n.z > 0.0f); // every triangle faces the same way as the face
      fArea += n.GetLength() * 0.5f;
    }

    PL_TEST_FLOAT(fArea, 3.0f, 0.0001f);
    PL_TEST_FLOAT(mesh.ComputeFaceArea(0), 3.0f, 0.0001f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Planarity error")
  {
    plEditableMesh mesh = MakeBox(plVec3(-1), plVec3(1));
    PL_TEST_FLOAT(mesh.ComputeFacePlanarityError(0), 0.0f, 0.0001f);

    mesh.m_Vertices[mesh.m_Faces[0].m_Corners[0]].z += 0.4f;
    PL_TEST_BOOL(mesh.ComputeFacePlanarityError(0) > 0.05f);
  }
}

PL_CREATE_SIMPLE_TEST(MeshCore, RenderGeometry)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Internal faces are not rendered")
  {
    plEditableMesh mesh = MakeBox(plVec3(-1), plVec3(1));
    mesh.m_Faces[0].m_uiFlags |= plBrushFaceFlags::Internal;

    plGeometry geom;
    plDynamicArray<plBrushSubMeshRange> ranges;
    mesh.BuildRenderGeometry(geom, ranges);

    PL_TEST_INT(geom.GetPolygons().GetCount(), 10);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Smoothing shares normals below the angle only")
  {
    plEditableMesh mesh = MakeBox(plVec3(-1), plVec3(1));

    plGeometry flat, smooth;
    plDynamicArray<plBrushSubMeshRange> ranges;

    // box edges are 90 degrees: a 40 degree threshold keeps them hard, 100 smooths them
    mesh.BuildRenderGeometry(flat, ranges, plAngle::MakeFromDegree(40));
    for (const auto& v : flat.GetVertices())
    {
      const float fMax = plMath::Max(plMath::Abs(v.m_vNormal.x), plMath::Abs(v.m_vNormal.y), plMath::Abs(v.m_vNormal.z));
      PL_TEST_FLOAT(fMax, 1.0f, 0.0001f);
    }

    mesh.BuildRenderGeometry(smooth, ranges, plAngle::MakeFromDegree(100));
    for (const auto& v : smooth.GetVertices())
    {
      PL_TEST_FLOAT(plMath::Abs(v.m_vNormal.x), 0.57735f, 0.001f);
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Materials sort into one range per slot")
  {
    plEditableMesh mesh = MakeBox(plVec3(-1), plVec3(1));
    mesh.m_Faces[1].m_uiMaterialSlot = 2;
    mesh.m_Faces[4].m_uiMaterialSlot = 2;

    plGeometry geom;
    plDynamicArray<plBrushSubMeshRange> ranges;
    mesh.BuildRenderGeometry(geom, ranges);

    PL_TEST_INT(ranges.GetCount(), 2);
    PL_TEST_INT(ranges[0].m_uiMaterialSlot, 0);
    PL_TEST_INT(ranges[0].m_uiNumTriangles, 8);
    PL_TEST_INT(ranges[1].m_uiMaterialSlot, 2);
    PL_TEST_INT(ranges[1].m_uiNumTriangles, 4);
  }
}

PL_CREATE_SIMPLE_TEST(MeshCore, FaceAttributes)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Codec round trip")
  {
    plBrushFaceAttributeMap attributes;

    plBrushFaceAttributes& a = attributes[5];
    a.m_uiMask = plBrushFaceAttributes::Slot | plBrushFaceAttributes::UV;
    a.m_uiMaterialSlot = 4;
    a.m_UV.m_vScale = plVec2(0.5f, 2.0f);
    a.m_UV.m_Rotation = plAngle::MakeFromDegree(45);
    a.m_UV.m_bWorldAligned = false;

    plBrushFaceAttributes& b = attributes[plBrushFaceIds::MakeCutFaceId(77, 0, 3, true)];
    b.m_uiMask = plBrushFaceAttributes::Flags;
    b.m_uiFlags = plBrushFaceFlags::HardEdges;

    plDataBuffer blob;
    plBrushFaceAttributeCodec::Write(attributes, blob);

    plBrushFaceAttributeMap loaded;
    plBrushFaceAttributeCodec::Parse(blob, loaded);

    PL_TEST_INT(loaded.GetCount(), 2);
    PL_TEST_INT(loaded[5].m_uiMaterialSlot, 4);
    PL_TEST_VEC2(loaded[5].m_UV.m_vScale, plVec2(0.5f, 2.0f), 0.0f);
    PL_TEST_FLOAT(loaded[5].m_UV.m_Rotation.GetDegree(), 45.0f, 0.001f);
    PL_TEST_BOOL(!loaded[5].m_UV.m_bWorldAligned);
    PL_TEST_INT(loaded[plBrushFaceIds::MakeCutFaceId(77, 0, 3, true)].m_uiFlags, plBrushFaceFlags::HardEdges);

    // identical content, identical bytes
    plDataBuffer blob2;
    plBrushFaceAttributeCodec::Write(loaded, blob2);
    PL_TEST_BOOL(blob == blob2);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Version 1 blobs read as slot overrides")
  {
    plDataBuffer blob;
    blob.PushBack(1);                                      // version
    blob.PushBack(1); blob.PushBack(0); blob.PushBack(0); blob.PushBack(0); // count
    blob.PushBack(9); blob.PushBack(0); blob.PushBack(0); blob.PushBack(0); // face id 9
    blob.PushBack(3);                                      // slot

    plBrushFaceAttributeMap loaded;
    plBrushFaceAttributeCodec::Parse(blob, loaded);

    PL_TEST_INT(loaded.GetCount(), 1);
    PL_TEST_INT(loaded[9].m_uiMask, plBrushFaceAttributes::Slot);
    PL_TEST_INT(loaded[9].m_uiMaterialSlot, 3);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Truncated attribute data gives an empty map")
  {
    plBrushFaceAttributeMap attributes;
    attributes[1].m_uiMask = plBrushFaceAttributes::UV;

    plDataBuffer blob;
    plBrushFaceAttributeCodec::Write(attributes, blob);
    blob.PopBack(3);

    plBrushFaceAttributeMap loaded;
    plBrushFaceAttributeCodec::Parse(blob, loaded);
    PL_TEST_BOOL(loaded.IsEmpty());
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "UV overrides reach the rendered texture coordinates")
  {
    auto uvExtent = [](const plEditableMesh& mesh)
    {
      plGeometry geom;
      plDynamicArray<plBrushSubMeshRange> ranges;
      mesh.BuildRenderGeometry(geom, ranges);

      float fMin = plMath::MaxValue<float>(), fMax = -plMath::MaxValue<float>();
      for (const auto& v : geom.GetVertices())
      {
        fMin = plMath::Min(fMin, v.m_vTexCoord.x);
        fMax = plMath::Max(fMax, v.m_vTexCoord.x);
      }
      return fMax - fMin;
    };

    plEditableMesh mesh = MakeBox(plVec3(-1), plVec3(1));

    // imported per-corner UVs on every face; a projection override must replace them
    for (plBrushFace& face : mesh.m_Faces)
    {
      face.m_uiFlags |= plBrushFaceFlags::ExplicitUV;
      face.m_CornerUVs.SetCount(face.m_Corners.GetCount());
    }

    PL_TEST_FLOAT(uvExtent(mesh), 0.0f, 0.0001f);

    plBrushFaceAttributeMap attributes;
    for (const plBrushFace& face : mesh.m_Faces)
    {
      plBrushFaceAttributes& attr = attributes[face.m_uiFaceId];
      attr.m_uiMask = plBrushFaceAttributes::UV;
      attr.m_UV.m_vScale = plVec2(3.0f); // one repeat per third of a metre: the 2 m box spans 6
    }

    plBrushFaceAttributeCodec::Apply(attributes, mesh);
    PL_TEST_FLOAT(uvExtent(mesh), 6.0f, 0.0001f);
  }
}
