#include <ModelingPlugin/ModelingPluginPCH.h>

#include <Foundation/IO/MemoryStream.h>
#include <ModelingPlugin/Modeling/BrushFaceAttributes.h>

namespace
{
  // v1: (faceId u32, slot u8) pairs. v2: faceId u32, mask u8, then the masked fields.
  constexpr plUInt8 c_uiFaceDataVersion = 2;
  constexpr plUInt32 c_uiMaxEntries = 1024 * 1024;
} // namespace

void plBrushFaceAttributeCodec::Parse(const plDataBuffer& data, plBrushFaceAttributeMap& out_attributes)
{
  out_attributes.Clear();

  if (data.IsEmpty())
    return;

  plRawMemoryStreamReader reader(data.GetData(), data.GetCount());

  bool bOk = true;
  auto read = [&](auto& value)
  {
    if (bOk && reader.ReadBytes(&value, sizeof(value)) != sizeof(value))
      bOk = false;
  };

  plUInt8 uiVersion = 0;
  read(uiVersion);

  if (uiVersion == 0 || uiVersion > c_uiFaceDataVersion)
    return;

  plUInt32 uiCount = 0;
  read(uiCount);

  if (!bOk || uiCount > c_uiMaxEntries)
    return;

  out_attributes.Reserve(uiCount);

  for (plUInt32 i = 0; i < uiCount && bOk; ++i)
  {
    plUInt32 uiFaceId = 0;
    read(uiFaceId);

    plBrushFaceAttributes attr;

    if (uiVersion == 1)
    {
      read(attr.m_uiMaterialSlot);
      attr.m_uiMask = plBrushFaceAttributes::Slot;
    }
    else
    {
      read(attr.m_uiMask);

      if (attr.m_uiMask & plBrushFaceAttributes::Slot)
        read(attr.m_uiMaterialSlot);

      if (attr.m_uiMask & plBrushFaceAttributes::Flags)
        read(attr.m_uiFlags);

      if (attr.m_uiMask & plBrushFaceAttributes::UV)
      {
        read(attr.m_UV.m_vUAxis);
        read(attr.m_UV.m_vVAxis);
        read(attr.m_UV.m_vOffset);
        read(attr.m_UV.m_vScale);

        float fRotation = 0.0f;
        read(fRotation);
        attr.m_UV.m_Rotation = plAngle::MakeFromRadian(fRotation);

        plUInt8 uiWorldAligned = 1;
        read(uiWorldAligned);
        attr.m_UV.m_bWorldAligned = uiWorldAligned != 0;
      }
    }

    if (bOk)
    {
      out_attributes[uiFaceId] = attr;
    }
  }

  if (!bOk)
  {
    out_attributes.Clear();
  }
}

void plBrushFaceAttributeCodec::Write(const plBrushFaceAttributeMap& attributes, plDataBuffer& out_data)
{
  out_data.Clear();

  if (attributes.IsEmpty())
    return;

  // sorted by face id: identical overrides always produce identical bytes (stable content hashes)
  plDynamicArray<plUInt32> faceIds;
  faceIds.Reserve(attributes.GetCount());

  for (auto it : attributes)
  {
    if (it.Value().m_uiMask != 0)
      faceIds.PushBack(it.Key());
  }

  if (faceIds.IsEmpty())
    return;

  faceIds.Sort();

  plDefaultMemoryStreamStorage storage;
  plMemoryStreamWriter writer(&storage);

  writer << c_uiFaceDataVersion;
  writer << faceIds.GetCount();

  for (plUInt32 uiFaceId : faceIds)
  {
    const plBrushFaceAttributes& attr = *attributes.GetValue(uiFaceId);

    writer << uiFaceId;
    writer << attr.m_uiMask;

    if (attr.m_uiMask & plBrushFaceAttributes::Slot)
      writer << attr.m_uiMaterialSlot;

    if (attr.m_uiMask & plBrushFaceAttributes::Flags)
      writer << attr.m_uiFlags;

    if (attr.m_uiMask & plBrushFaceAttributes::UV)
    {
      writer << attr.m_UV.m_vUAxis;
      writer << attr.m_UV.m_vVAxis;
      writer << attr.m_UV.m_vOffset;
      writer << attr.m_UV.m_vScale;
      writer << attr.m_UV.m_Rotation.GetRadian();
      writer << static_cast<plUInt8>(attr.m_UV.m_bWorldAligned ? 1 : 0);
    }
  }

  out_data.SetCountUninitialized(storage.GetStorageSize32());

  plRawMemoryStreamWriter rawWriter(out_data.GetData(), out_data.GetCount());
  storage.CopyToStream(rawWriter).IgnoreResult();
}

void plBrushFaceAttributeCodec::Apply(const plBrushFaceAttributeMap& attributes, plEditableMesh& inout_mesh)
{
  if (attributes.IsEmpty())
    return;

  for (plBrushFace& face : inout_mesh.m_Faces)
  {
    const plBrushFaceAttributes* pAttr = attributes.GetValue(face.m_uiFaceId);

    if (pAttr == nullptr)
      continue;

    if (pAttr->m_uiMask & plBrushFaceAttributes::Slot)
      face.m_uiMaterialSlot = pAttr->m_uiMaterialSlot;

    if (pAttr->m_uiMask & plBrushFaceAttributes::Flags)
      face.m_uiFlags = static_cast<plUInt8>((face.m_uiFlags & ~plBrushFaceFlags::HardEdges) | (pAttr->m_uiFlags & plBrushFaceFlags::HardEdges));

    if (pAttr->m_uiMask & plBrushFaceAttributes::UV)
    {
      face.m_UV = pAttr->m_UV;

      // an authored projection replaces imported per-corner UVs
      face.m_uiFlags &= ~plBrushFaceFlags::ExplicitUV;
    }
  }
}


PL_STATICLINK_FILE(ModelingPlugin, ModelingPlugin_Modeling_Implementation_BrushFaceAttributes);
