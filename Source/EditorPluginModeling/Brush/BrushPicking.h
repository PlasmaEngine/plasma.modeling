#pragma once

#include <EditorPluginModeling/EditorPluginModelingDLL.h>
#include <Foundation/Math/Transform.h>
#include <ModelingPlugin/Modeling/EditableMesh.h>

class plDocumentObject;

/// \brief CPU-side face resolution against document brush data. The GPU picking path only carries an
/// 8-bit part index (the submesh), so anything face-granular has to be answered in the editor process.
namespace plBrushPicking
{
  /// \brief Rebuilds a brush component's editable mesh from its reflected document properties.
  plResult BuildMeshFromObject(const plDocumentObject* pComponent, plEditableMesh& out_mesh);

  /// \brief Like BuildMeshFromObject, but with every overlapping subtractive brush in the document
  /// carved out — the same evaluation the engine performs, with the same deterministic cutter order,
  /// so cap face ids match across processes.
  plResult BuildEvaluatedMeshFromObject(const plDocumentObject* pComponent, plEditableMesh& out_mesh);

  /// \brief All subtractive brush components in the document (excluding pExclude), sorted by world
  /// position for deterministic evaluation order.
  void FindCutterComponents(const plDocumentObject* pAnyDocumentObject, const plDocumentObject* pExclude, plDynamicArray<const plDocumentObject*>& out_cutters);

  /// \brief The owner game object's global transform, resolved through the game object document.
  plTransform GetComponentGlobalTransform(const plDocumentObject* pComponent);

  /// \brief Returns the id of the face containing the given local-space surface point, or plInvalidIndex.
  ///
  /// The point is expected to lie ON the surface (it comes from GPU picking), so this matches by
  /// nearest face plane with a point-in-polygon containment test, not by raycast.
  plUInt32 PickFaceAtLocalPos(const plEditableMesh& mesh, const plVec3& vLocalPos);

  /// \brief Same match, returning the face INDEX instead of its id. -1 when nothing matches.
  plInt32 PickFaceIndexAtLocalPos(const plEditableMesh& mesh, const plVec3& vLocalPos);
} // namespace plBrushPicking
