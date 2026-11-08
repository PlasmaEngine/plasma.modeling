#pragma once

#include <EditorPluginModeling/EditorPluginModelingDLL.h>
#include <Foundation/Containers/HashSet.h>
#include <Foundation/Math/Plane.h>
#include <Foundation/Types/Delegate.h>
#include <Foundation/Types/Status.h>
#include <ModelingPlugin/Modeling/BrushFaceAttributes.h>
#include <ModelingPlugin/Modeling/BrushGenerators.h>

class plDocument;
class plDocumentObject;
class plCommandHistory;

/// \brief Every modeling operation as an undoable document change. The Modeling panel, the viewport
/// tool, the marking menu and the MCP tools all go through here, so they behave identically.
///
/// Brushes are addressed by their plBrushComponent document objects. Each call is one undo step unless
/// a transaction is already open, in which case it joins it. Failures change nothing and return a
/// message meant for the user.
namespace plBrushCommands
{
  using MeshEditFunc = plDelegate<plStatus(plEditableMesh&)>;
  using AttributeEditFunc = plDelegate<void(plUInt32 uiFaceId, plBrushFaceAttributes&)>;

  // ---- queries ----------------------------------------------------------------

  /// \brief The brush components among the objects: selected components themselves, or the brush
  /// component of each selected game object.
  PL_EDITORPLUGINMODELING_DLL void GatherBrushes(plArrayPtr<const plDocumentObject* const> objects, plDynamicArray<const plDocumentObject*>& out_brushes);

  /// \brief The brush components of the document's current selection.
  PL_EDITORPLUGINMODELING_DLL void GatherSelectedBrushes(const plDocument* pDocument, plDynamicArray<const plDocumentObject*>& out_brushes);

  /// \brief The game object carrying the nearest brush group above (or on) the object, or nullptr.
  PL_EDITORPLUGINMODELING_DLL const plDocumentObject* FindGroupObject(const plDocumentObject* pObject);

  // ---- mesh edits ---------------------------------------------------------------

  /// \brief Writes a mesh as the brush's authored mesh (turns it Editable). Must run inside a transaction.
  PL_EDITORPLUGINMODELING_DLL void WriteMesh(plCommandHistory* pHistory, const plDocumentObject* pBrush, const plEditableMesh& mesh);

  /// \brief Gives polygons of the brush's authored mesh their own face ids where they share one with other
  /// polygons (CSG pieces keep the id of the face they were cut from), so each can be selected and edited
  /// alone. A separated polygon gets a copy of its face's attributes, so nothing changes visibly.
  /// out_faceIds receives the id of every listed polygon (face indices), in order. One undo step, and
  /// only when something had to change.
  PL_EDITORPLUGINMODELING_DLL plStatus SeparateFaceIds(plDocument* pDocument, const plDocumentObject* pBrush, plArrayPtr<const plUInt32> faceIndices, plDynamicArray<plUInt32>& out_faceIds);

  /// \brief Runs an operation on the brush's authored mesh and stores the result, as one undo step.
  PL_EDITORPLUGINMODELING_DLL plStatus EditMesh(plDocument* pDocument, const plDocumentObject* pBrush, plStringView sActionName, MeshEditFunc func);

  // ---- face attributes ------------------------------------------------------------

  /// \brief Runs func on the attributes of every face in faceIds and stores the result, as one command.
  PL_EDITORPLUGINMODELING_DLL plStatus EditFaceAttributes(plDocument* pDocument, const plDocumentObject* pBrush, const plHashSet<plUInt32>& faceIds, plStringView sActionName, AttributeEditFunc func);

  /// \brief Assigns a material (asset guid string) to faces, reusing its slot or appending one.
  PL_EDITORPLUGINMODELING_DLL plStatus SetFaceMaterial(plDocument* pDocument, const plDocumentObject* pBrush, const plHashSet<plUInt32>& faceIds, plStringView sMaterialGuid);

  /// \brief Assigns a material to every face: slot 0 becomes the material and face slot overrides are cleared.
  PL_EDITORPLUGINMODELING_DLL plStatus SetBrushMaterial(plDocument* pDocument, const plDocumentObject* pBrush, plStringView sMaterialGuid);

  // ---- CSG ----------------------------------------------------------------------

  PL_EDITORPLUGINMODELING_DLL plStatus ToggleCutter(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> brushes);

  /// \brief Makes the live cut of each selected cutter permanent: the brushes it overlaps (same scope) get
  /// the carved mesh as their authored mesh, then the cutter is deleted. Cutters cut without this.
  PL_EDITORPLUGINMODELING_DLL plStatus BakeCuts(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> cutters);

  /// \brief Turns each brush into a room with walls of its Thickness.
  PL_EDITORPLUGINMODELING_DLL plStatus Hollow(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> brushes);

  /// \brief Merges every other brush into the first one and deletes them. Materials carry over.
  PL_EDITORPLUGINMODELING_DLL plStatus Union(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> brushes);

  /// \brief Splits the brush's non-convex shells into convex pieces, so it can cut.
  PL_EDITORPLUGINMODELING_DLL plStatus Decompose(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> brushes);

  struct ClipKeep
  {
    enum Enum
    {
      Front, ///< the side the plane normal points to
      Back,
      Both, ///< the back half becomes a new brush
    };
  };

  /// \brief Cuts brushes with a world-space plane.
  PL_EDITORPLUGINMODELING_DLL plStatus Clip(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> brushes, const plPlane& worldPlane, ClipKeep::Enum keep);

  /// \brief Moves faces into a new brush object beside the original. out_pNewBrush receives its component.
  PL_EDITORPLUGINMODELING_DLL plStatus Detach(plDocument* pDocument, const plDocumentObject* pBrush, const plHashSet<plUInt32>& faceIds, plUuid* out_pNewBrush = nullptr);

  // ---- pivot --------------------------------------------------------------------

  /// \brief Moves the brush's origin (its owner object) to a world position. The geometry, its textures
  /// and the object's children stay where they are: parametric brushes shift their extents and stay
  /// parametric, editable ones shift their vertices.
  PL_EDITORPLUGINMODELING_DLL plStatus SetPivot(plDocument* pDocument, const plDocumentObject* pBrush, const plVec3& vWorldPivot);

  struct PivotPlacement
  {
    enum Enum
    {
      Center, ///< center of the brush's bounds
      Bottom, ///< center of the bounds' bottom face, for placing brushes on floors
    };
  };

  /// \brief Moves each brush's origin to a point of its own bounds, as one undo step.
  PL_EDITORPLUGINMODELING_DLL plStatus PlacePivot(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> brushes, PivotPlacement::Enum placement);

  // ---- creation -----------------------------------------------------------------

  struct BrushDesc
  {
    plString m_sName = "Brush";
    plUuid m_Parent; ///< invalid: scene root
    plTransform m_GlobalTransform = plTransform::MakeIdentity();
    plBrushShape::Enum m_Shape = plBrushShape::Box;
    plBrushGeneratorParams m_Params;
    bool m_bSubtractive = false;
    plString m_sMaterial; ///< asset guid string, optional
  };

  /// \brief Adds a new brush object. Returns the brush component's guid, or an invalid guid on failure.
  PL_EDITORPLUGINMODELING_DLL plUuid CreateBrush(plDocument* pDocument, const BrushDesc& desc);

  /// \brief Wraps objects into a new brush group beside the first one, keeping their world placement.
  PL_EDITORPLUGINMODELING_DLL plStatus CreateGroup(plDocument* pDocument, plArrayPtr<const plDocumentObject* const> objects, plUuid* out_pGroupObject = nullptr);
} // namespace plBrushCommands
