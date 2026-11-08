#pragma once

#include <EditorPluginModeling/EditorPluginModelingDLL.h>
#include <Foundation/Math/Transform.h>
#include <ModelingPlugin/Modeling/BrushEvaluator.h>
#include <ModelingPlugin/Modeling/EditableMesh.h>

class plDocumentObject;

/// \brief Brush geometry resolved from document data. Everything face-granular has to be answered in the
/// editor process (GPU picking only carries an 8-bit part index), and it must match what the engine
/// renders, so evaluation goes through the same plBrushEvaluator with the same cutter order.
namespace plBrushPicking
{
  /// \brief True for a plBrushComponent document object.
  PL_EDITORPLUGINMODELING_DLL bool IsBrushComponent(const plDocumentObject* pObject);

  /// \brief The authored base mesh of a brush component (generator output or stored mesh), no CSG, no
  /// face attributes.
  PL_EDITORPLUGINMODELING_DLL plResult BuildMeshFromObject(const plDocumentObject* pComponent, plEditableMesh& out_mesh);

  /// \brief The decoded FaceData of a brush component.
  PL_EDITORPLUGINMODELING_DLL void ReadFaceAttributes(const plDocumentObject* pComponent, plBrushFaceAttributeMap& out_attributes);

  /// \brief The final mesh as the engine renders it: every cutter in scope carved out, attributes applied.
  PL_EDITORPLUGINMODELING_DLL plResult BuildEvaluatedMeshFromObject(const plDocumentObject* pComponent, plEditableMesh& out_mesh, plDynamicArray<plUInt32>* out_pRefusedCutters = nullptr);

  /// \brief The cutters that can cut pTarget (same scope, overlapping targetWorldBounds), sorted into
  /// evaluation order. With bForRooms, also the cutters overlapping those. Mirrors plBrushWorldModule.
  PL_EDITORPLUGINMODELING_DLL void GatherCutters(const plDocumentObject* pTarget, const plBoundingBox& targetWorldBounds, plDynamicArray<plBrushCutterInput>& out_cutters, plDynamicArray<const plDocumentObject*>* out_pCutterObjects = nullptr, bool bForRooms = false);

  /// \brief The room shells of every other additive brush in pTarget's scope that one of the cutters
  /// reaches. Mirrors plBrushWorldModule::GatherRooms.
  PL_EDITORPLUGINMODELING_DLL void GatherRooms(const plDocumentObject* pTarget, plArrayPtr<const plBrushCutterInput> cutters, plDynamicArray<plBrushRoomInput>& out_rooms);

  /// \brief Carves the cutters out of a brush's mesh the way the engine does, rooms included, without
  /// applying face attributes. The mesh must be pComponent's authored mesh (or a carve of it).
  PL_EDITORPLUGINMODELING_DLL void CarveMesh(const plDocumentObject* pComponent, plEditableMesh& inout_mesh, plArrayPtr<const plBrushCutterInput> cutters, plDynamicArray<plUInt32>* out_pRefusedCutters = nullptr);

  /// \brief The game object whose subtree scopes the brush's cutters (nearest isolating brush group), or
  /// nullptr for the scene-wide scope. Mirrors plBrushGroupComponent::FindCutterScope.
  PL_EDITORPLUGINMODELING_DLL const plDocumentObject* FindCutterScope(const plDocumentObject* pComponent);

  /// \brief Every brush component in the document.
  PL_EDITORPLUGINMODELING_DLL void FindAllBrushComponents(const plDocumentObject* pAnyDocumentObject, plDynamicArray<const plDocumentObject*>& out_brushes);

  /// \brief The owner game object's global transform, resolved through the game object document.
  PL_EDITORPLUGINMODELING_DLL plTransform GetComponentGlobalTransform(const plDocumentObject* pComponent);

  /// \brief Returns the id of the face containing the given local-space surface point, or plInvalidIndex.
  ///
  /// The point is expected to lie ON the surface (it comes from GPU picking), so this matches by
  /// nearest face plane with a point-in-polygon containment test, not by raycast. Internal faces are
  /// never picked.
  PL_EDITORPLUGINMODELING_DLL plUInt32 PickFaceAtLocalPos(const plEditableMesh& mesh, const plVec3& vLocalPos);

  /// \brief Same match, returning the face INDEX instead of its id. -1 when nothing matches.
  PL_EDITORPLUGINMODELING_DLL plInt32 PickFaceIndexAtLocalPos(const plEditableMesh& mesh, const plVec3& vLocalPos);
} // namespace plBrushPicking
