#pragma once

#include <EditorPluginModeling/EditorPluginModelingDLL.h>
#include <Foundation/Types/Status.h>

class plDocument;
class plDocumentObject;

/// \brief Getting geometry out of brushes and into them.
namespace plBrushExport
{
  /// \brief Writes the brushes as rendered (cuts, materials, UVs, smoothing) to an OBJ file with an MTL
  /// sidecar, in world space and in the OBJ convention (right-handed, Y up) that DCC tools and the mesh
  /// importer's default settings expect. With an empty path the file goes next to the scene.
  PL_EDITORPLUGINMODELING_DLL plStatus ExportObj(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> brushes, plStringView sPath, plString* out_pWrittenFile = nullptr);

  /// \brief Writes each brush to <project>/Blockout/<scene>_<name>.obj in its own space, creates a mesh
  /// asset for it with the brush's materials in its slots, and (bReplace) swaps the brush for a mesh
  /// component using that asset, as one undo step.
  PL_EDITORPLUGINMODELING_DLL plStatus BakeToMeshAsset(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> brushes, bool bReplace);

  /// \brief Replaces every Grey Boxing component in the scene with an equivalent brush, as one undo step.
  PL_EDITORPLUGINMODELING_DLL plStatus ConvertGreyBoxes(plDocument* pDocument);

  /// \brief Replaces the mesh components on the objects with editable brushes that keep the mesh's
  /// triangles, UVs and materials.
  PL_EDITORPLUGINMODELING_DLL plStatus ConvertMeshesToBrushes(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> objects);
} // namespace plBrushExport
