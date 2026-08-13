#pragma once

#include <ModelingPlugin/ModelingPluginDLL.h>

#include <Core/World/Component.h>
#include <Core/World/World.h>
#include <Foundation/Containers/HashTable.h>
#include <ModelingPlugin/Modeling/BrushGenerators.h>
#include <RendererCore/Components/RenderComponent.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <RendererCore/Rasterizer/RasterizerObject.h>

class plGeometry;
class plMeshResourceDescriptor;
struct plMsgExtractRenderData;
struct plMsgBuildStaticMesh;
struct plMsgExtractGeometry;
struct plMsgExtractOccluderData;
struct plMsgSetMeshMaterial;
struct plMsgSetColor;
struct plMsgSetCustomData;

using plMeshResourceHandle = plTypedResourceHandle<class plMeshResource>;
using plMaterialResourceHandle = plTypedResourceHandle<class plMaterialResource>;

using plBrushComponentManager = plComponentManager<class plBrushComponent, plBlockStorageType::Compact>;

/// \brief Scene-native blockout geometry: parametric shapes with per-face materials, generated from an
/// editable mesh. Renders, collides (via plMsgBuildStaticMesh at scene export), participates in navmesh
/// generation and acts as an occluder — the modeling-tool successor to plGreyBoxComponent.
///
/// Per-face material overrides live in the FaceData blob as (faceId -> slot) pairs, keyed by the
/// generators' SEMANTIC face ids so assignments survive parameter changes. One render submesh is emitted
/// per used material slot.
class PL_MODELINGPLUGIN_DLL plBrushComponent : public plRenderComponent
{
  PL_DECLARE_COMPONENT_TYPE(plBrushComponent, plRenderComponent, plBrushComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

public:
  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

protected:
  virtual void OnActivated() override;
  virtual void OnDeactivated() override;

  //////////////////////////////////////////////////////////////////////////
  // plRenderComponent
protected:
  virtual plResult GetLocalBounds(plBoundingBoxSphere& bounds, bool& bAlwaysVisible, plMsgUpdateLocalBounds& msg) override;
  void OnMsgExtractRenderData(plMsgExtractRenderData& msg) const;

  //////////////////////////////////////////////////////////////////////////
  // plBrushComponent

public:
  plBrushComponent();
  ~plBrushComponent();

  void SetShape(plEnum<plBrushShape> shape);                // [ property ]
  plEnum<plBrushShape> GetShape() const { return m_Shape; } // [ property ]

  void SetColor(const plColor& color);     // [ property ]
  const plColor& GetColor() const;         // [ property ]

  void SetCustomData(const plVec4& vData); // [ property ]
  const plVec4& GetCustomData() const;     // [ property ]

  void SetSizeNegX(float f);                        // [ property ]
  float GetSizeNegX() const { return m_Params.m_fSizeNegX; } // [ property ]
  void SetSizePosX(float f);                        // [ property ]
  float GetSizePosX() const { return m_Params.m_fSizePosX; } // [ property ]
  void SetSizeNegY(float f);                        // [ property ]
  float GetSizeNegY() const { return m_Params.m_fSizeNegY; } // [ property ]
  void SetSizePosY(float f);                        // [ property ]
  float GetSizePosY() const { return m_Params.m_fSizePosY; } // [ property ]
  void SetSizeNegZ(float f);                        // [ property ]
  float GetSizeNegZ() const { return m_Params.m_fSizeNegZ; } // [ property ]
  void SetSizePosZ(float f);                        // [ property ]
  float GetSizePosZ() const { return m_Params.m_fSizePosZ; } // [ property ]

  void SetDetail(plUInt32 uiDetail);                       // [ property ]
  plUInt32 GetDetail() const { return m_Params.m_uiDetail; } // [ property ]

  void SetCurvature(plAngle curvature);                       // [ property ]
  plAngle GetCurvature() const { return m_Params.m_Curvature; } // [ property ]

  void SetThickness(float f);                                // [ property ]
  float GetThickness() const { return m_Params.m_fThickness; } // [ property ]

  /// \brief A subtractive brush is a live cutter: it carves every additive brush it overlaps, emits no
  /// collision, and only renders while the world is not simulating (edit mode).
  void SetSubtractive(bool b);                             // [ property ]
  bool GetSubtractive() const { return m_bSubtractive; }   // [ property ]

  void SetGenerateCollision(bool b);                                 // [ property ]
  bool GetGenerateCollision() const { return m_bGenerateCollision; } // [ property ]

  void SetIncludeInNavmesh(bool b);                                // [ property ]
  bool GetIncludeInNavmesh() const { return m_bIncludeInNavmesh; } // [ property ]

  /// \brief The per-face attribute blob: version byte + count + (faceId u32, materialSlot u8) pairs.
  void SetFaceData(const plDataBuffer& data);                 // [ property ]
  const plDataBuffer& GetFaceData() const { return m_FaceData; } // [ property ]

  /// \brief The serialized plEditableMesh, authoritative once Shape is Editable. Empty while parametric.
  void SetStoredMesh(const plDataBuffer& data);                     // [ property ]
  const plDataBuffer& GetStoredMesh() const { return m_StoredMesh; } // [ property ]

  void SetMaterial(plUInt32 uiIndex, const plMaterialResourceHandle& hMaterial);
  plMaterialResourceHandle GetMaterial(plUInt32 uiIndex) const;

  /// \brief Builds the current editable mesh: generator output with the FaceData overrides applied.
  void BuildEditableMesh(plEditableMesh& out_mesh) const;

  /// \brief The mesh after non-destructive CSG: the base mesh minus every overlapping subtractive
  /// brush (world-module query). For subtractive brushes this is just the base mesh.
  void BuildEvaluatedMesh(plEditableMesh& out_mesh) const;

  /// \brief Hash over everything that shapes the geometry (not materials/colors) — the world module's
  /// change detection key.
  plUInt64 ComputeContentHash() const;

  /// \brief Public mesh invalidation, used by the world module when an overlapping cutter changes.
  void InvalidateBrushMesh() { InvalidateMesh(); }

  /// \brief Decodes a FaceData blob into a (faceId -> materialSlot) map. Tolerates empty and unknown versions.
  static void ParseFaceData(const plDataBuffer& data, plHashTable<plUInt32, plUInt8>& out_overrides);

  /// \brief Encodes the override map into the FaceData blob format (both sides of the mirror share this codec).
  static void WriteFaceData(const plHashTable<plUInt32, plUInt8>& overrides, plDataBuffer& out_data);

protected:
  void OnBuildStaticMesh(plMsgBuildStaticMesh& msg) const;
  void OnMsgExtractGeometry(plMsgExtractGeometry& msg) const;
  void OnMsgExtractOccluderData(plMsgExtractOccluderData& msg) const;
  void OnMsgSetMeshMaterial(plMsgSetMeshMaterial& ref_msg); // [ msg handler ]
  void OnMsgSetColor(plMsgSetColor& ref_msg);               // [ msg handler ]
  void OnMsgSetCustomData(plMsgSetCustomData& ref_msg);     // [ msg handler ]

  plUInt32 Materials_GetCount() const;                        // [ property ]
  plString Materials_GetValue(plUInt32 uiIndex) const;        // [ property ]
  void Materials_SetValue(plUInt32 uiIndex, plString sValue); // [ property ]
  void Materials_Insert(plUInt32 uiIndex, plString sValue);   // [ property ]
  void Materials_Remove(plUInt32 uiIndex);                    // [ property ]

  void InvalidateMesh();
  void GenerateMeshName(const plEditableMesh& mesh, plStringBuilder& out_sName) const;
  void GenerateMeshResourceDescriptor(plMeshResourceDescriptor& desc, const plEditableMesh& mesh) const;
  plMaterialResourceHandle ResolveMaterial(plUInt32 uiSlot) const;
  const char* GetMaterialIdOrDefault(plUInt32 uiSlot, plStringBuilder& ref_sTemp) const;

  template <typename ResourceType>
  plTypedResourceHandle<ResourceType> GenerateMesh() const;

  plEnum<plBrushShape> m_Shape;
  plBrushGeneratorParams m_Params;
  plColor m_Color = plColor::White;
  plVec4 m_vCustomData = plVec4(0, 1, 0, 1);
  bool m_bSubtractive = false;
  bool m_bGenerateCollision = true;
  bool m_bIncludeInNavmesh = true;
  bool m_bUseAsOccluder = true;

  plSmallArray<plMaterialResourceHandle, 2> m_Materials;
  plDataBuffer m_FaceData;
  plDataBuffer m_StoredMesh; ///< authoritative mesh once Shape is Editable; empty while parametric

  plMeshResourceHandle m_hMesh;

  mutable plSharedPtr<const plRasterizerObject> m_pOccluderObject;
  mutable plInstanceDataOffset m_InstanceDataOffset;
};
