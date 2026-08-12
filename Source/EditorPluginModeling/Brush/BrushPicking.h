#pragma once

#include <EditorPluginModeling/EditorPluginModelingDLL.h>
#include <ModelingPlugin/Modeling/EditableMesh.h>

class plDocumentObject;

/// \brief CPU-side face resolution against document brush data. The GPU picking path only carries an
/// 8-bit part index (the submesh), so anything face-granular has to be answered in the editor process.
namespace plBrushPicking
{
  /// \brief Rebuilds a brush component's editable mesh from its reflected document properties.
  /// Fails for Editable shapes (their stored mesh is not mirrored yet) and non-brush objects.
  plResult BuildMeshFromObject(const plDocumentObject* pComponent, plEditableMesh& out_mesh);

  /// \brief Returns the id of the face containing the given local-space surface point, or plInvalidIndex.
  ///
  /// The point is expected to lie ON the surface (it comes from GPU picking), so this matches by
  /// nearest face plane with a point-in-polygon containment test, not by raycast.
  plUInt32 PickFaceAtLocalPos(const plEditableMesh& mesh, const plVec3& vLocalPos);
} // namespace plBrushPicking
