#pragma once

#include <ModelingPlugin/ModelingPluginDLL.h>

#include <Foundation/Containers/HashTable.h>
#include <Foundation/Types/VariantType.h>
#include <ModelingPlugin/Modeling/EditableMesh.h>

/// \brief Per-face overrides authored on a brush, keyed by face id. They apply on top of whatever produced
/// the mesh (generator, stored mesh, CSG), so a parametric brush can be textured without freezing it.
struct PL_MODELINGPLUGIN_DLL plBrushFaceAttributes
{
  enum Mask : plUInt8
  {
    Slot = PL_BIT(0),
    Flags = PL_BIT(1),
    UV = PL_BIT(2),
  };

  plUInt8 m_uiMask = 0; ///< which of the fields below are set
  plUInt8 m_uiMaterialSlot = 0;
  plUInt8 m_uiFlags = 0; ///< only plBrushFaceFlags::HardEdges is applied
  plBrushFaceUV m_UV;
};

using plBrushFaceAttributeMap = plHashTable<plUInt32, plBrushFaceAttributes>;

namespace plBrushFaceAttributeCodec
{
  /// \brief Decodes the FaceData blob. Reads v1 (slot only) and v2; unknown versions and damaged data give
  /// an empty map.
  PL_MODELINGPLUGIN_DLL void Parse(const plDataBuffer& data, plBrushFaceAttributeMap& out_attributes);

  PL_MODELINGPLUGIN_DLL void Write(const plBrushFaceAttributeMap& attributes, plDataBuffer& out_data);

  /// \brief Applies the overrides to every face with a matching id.
  PL_MODELINGPLUGIN_DLL void Apply(const plBrushFaceAttributeMap& attributes, plEditableMesh& inout_mesh);
} // namespace plBrushFaceAttributeCodec
