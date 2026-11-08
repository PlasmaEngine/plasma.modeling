#pragma once

#include <ModelingPlugin/ModelingPluginDLL.h>

#include <Core/World/Component.h>
#include <Core/World/World.h>
#include <Foundation/Containers/HashTable.h>
#include <ModelingPlugin/Modeling/BrushFaceAttributes.h>
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

// FreeList, not Compact: plBrushWorldModule keeps pointers to brushes, and compact storage moves a
// component into the slot of every deleted one
using plBrushComponentManager = plComponentManager<class plBrushComponent, plBlockStorageType::FreeList>;

/// \brief Scene-native blockout geometry: parametric shapes with per-face materials, generated from an
/// editable mesh. Renders, collides (via plMsgBuildStaticMesh at scene export), participates in navmesh
/// generation and acts as an occluder — the modeling-tool successor to plGreyBoxComponent.
///
/// Per-face overrides (material slot, UV projection, hard edges) live in the FaceData blob keyed by the
/// generators' SEMANTIC face ids, so assignments survive parameter changes; they apply after CSG, so cut
/// faces take overrides too. One render submesh is emitted per used material slot.
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

  /// \brief Asset guid of the material faces get when their slot has none (the Pattern blockout material).
  static const char* GetDefaultMaterialId();

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

  void SetUseAsOccluder(bool b);                               // [ property ]
  bool GetUseAsOccluder() const { return m_bUseAsOccluder; }   // [ property ]

  /// \brief Corners whose faces meet at less than this angle share a smoothed normal. 0 = flat shading.
  void SetSmoothingAngle(plAngle angle);                            // [ property ]
  plAngle GetSmoothingAngle() const { return m_SmoothingAngle; }    // [ property ]

  /// \brief On: projected UVs follow the brush when it moves. Off: they are computed in world space, so
  /// textures tile seamlessly across neighbouring brushes and slide when a brush moves.
  void SetTextureLock(bool b);                              // [ property ]
  bool GetTextureLock() const { return m_bTextureLock; }    // [ property ]

  /// \brief Static brushes in a merging brush group become part of the group's merged mesh at export.
  void SetMergeOnExport(bool b) { m_bMergeOnExport = b; }       // [ property ]
  bool GetMergeOnExport() const { return m_bMergeOnExport; }    // [ property ]

  /// \brief Persistent identity: cut faces are keyed by the cutter's id, and cutters evaluate in id
  /// order. The editor assigns a unique id when a brush is created, pasted or duplicated.
  void SetBrushId(plUInt32 uiId);                        // [ property ]
  plUInt32 GetBrushId() const { return m_uiBrushId; }    // [ property ]

  /// \brief The per-face attribute blob (plBrushFaceAttributeCodec).
  void SetFaceData(const plDataBuffer& data);                 // [ property ]
  const plDataBuffer& GetFaceData() const { return m_FaceData; } // [ property ]
  const plBrushFaceAttributeMap& GetFaceAttributes() const { return m_FaceAttributes; }

  /// \brief The serialized plEditableMesh, authoritative once Shape is Editable. Empty while parametric.
  void SetStoredMesh(const plDataBuffer& data);                     // [ property ]
  const plDataBuffer& GetStoredMesh() const { return m_StoredMesh; } // [ property ]

  void SetMaterial(plUInt32 uiIndex, const plMaterialResourceHandle& hMaterial);
  plMaterialResourceHandle GetMaterial(plUInt32 uiIndex) const;
  plUInt32 GetMaterialCount() const { return m_Materials.GetCount(); }

  const plBrushGeneratorParams& GetGeneratorParams() const { return m_Params; }

  /// \brief The authored base mesh: generator output, or the stored mesh once Shape is Editable. No
  /// face attributes, no CSG. A damaged stored mesh gives an empty mesh.
  void BuildEditableMesh(plEditableMesh& out_mesh) const;

  /// \brief The final mesh: the base mesh minus every cutter in the brush's scope (for additive brushes),
  /// with the face attributes applied last.
  void BuildEvaluatedMesh(plEditableMesh& out_mesh) const;

  /// \brief Increments whenever an authored input of the geometry changes. The world module's change key.
  plUInt32 GetAuthoredRevision() const { return m_uiAuthoredRevision; }

  /// \brief For cutters: whether every shell is convex. A non-convex cutter cuts nothing.
  bool IsCutterConvex() const { return m_bCutterConvex; }

  /// \brief True when the stored mesh failed to load. The brush then renders nothing.
  bool HasDamagedMesh() const { return m_bDamagedMesh; }

  /// \brief Regenerates the mesh right away. Used by the world module when an overlapping cutter changes.
  void InvalidateBrushMesh();

  /// \brief Regenerates the mesh if a property change invalidated it. Called once per frame by the world
  /// module, so a burst of property changes costs one regeneration.
  void RebuildMeshIfDirty();

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

  void InvalidateMesh(); ///< marks the mesh for regeneration by the next RebuildMeshIfDirty
  void RegenerateMesh();
  void OnAuthoredChange();
  const plTransform* GetUVSpace(plTransform& ref_storage) const;
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
  bool m_bTextureLock = true;
  bool m_bMergeOnExport = true;
  plAngle m_SmoothingAngle = plAngle::MakeFromDegree(40.0f);
  plUInt32 m_uiBrushId = 0;

  plSmallArray<plMaterialResourceHandle, 2> m_Materials;
  plDataBuffer m_FaceData;
  plBrushFaceAttributeMap m_FaceAttributes; ///< decoded m_FaceData
  plDataBuffer m_StoredMesh; ///< authoritative mesh once Shape is Editable; empty while parametric

  plUInt32 m_uiAuthoredRevision = 0;
  mutable bool m_bCutterConvex = true;
  mutable bool m_bDamagedMesh = false;

  plMeshResourceHandle m_hMesh;
  bool m_bMeshDirty = false;

  mutable plSharedPtr<const plRasterizerObject> m_pOccluderObject;
  mutable plInstanceDataOffset m_InstanceDataOffset;
};
