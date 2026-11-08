#pragma once

#include <ModelingPlugin/ModelingPluginDLL.h>

#include <Foundation/Math/BoundingBox.h>
#include <Foundation/Math/Transform.h>
#include <ModelingPlugin/Modeling/BrushFaceAttributes.h>
#include <ModelingPlugin/Modeling/EditableMesh.h>

/// \brief One cutter as the evaluator sees it. The engine fills these from the world module, the editor
/// from the document — both then run the same code, so both processes produce the same mesh.
struct PL_MODELINGPLUGIN_DLL plBrushCutterInput
{
  plEditableMesh m_Mesh;                                    ///< cutter-local
  plTransform m_GlobalTransform = plTransform::MakeIdentity();
  plUInt32 m_uiBrushId = 0;
  plBoundingBox m_WorldBounds = plBoundingBox::MakeInvalid();
};

/// \brief The room shells (inside-out shells) of another brush near the target's cutters.
struct PL_MODELINGPLUGIN_DLL plBrushRoomInput
{
  plEditableMesh m_Mesh;                                    ///< only the room shells, brush-local
  plTransform m_GlobalTransform = plTransform::MakeIdentity();
  plUInt32 m_uiBrushId = 0;
};

/// \brief What a target with room shells needs besides its cutters: its own identity and the other rooms
/// its cutters reach, so that every room agrees which one draws each cutter's passage.
struct PL_MODELINGPLUGIN_DLL plBrushRoomContext
{
  plUInt32 m_uiTargetBrushId = 0;
  plArrayPtr<const plBrushRoomInput> m_OtherRooms;
};

namespace plBrushEvaluator
{
  /// \brief The room shells of a mesh, or an empty mesh when it has none.
  PL_MODELINGPLUGIN_DLL void ExtractRoomShells(const plEditableMesh& mesh, plEditableMesh& out_rooms);

  /// \brief The one evaluation order: ascending brush id, world position only to break ties between
  /// brushes that share an id. Moving a cutter never reorders it.
  PL_MODELINGPLUGIN_DLL bool CutterLess(plUInt32 uiIdA, const plVec3& vPosA, plUInt32 uiIdB, const plVec3& vPosB);

  /// \brief Carves the cutters (already sorted with CutterLess) out of the target, then applies the
  /// target's face attributes, so overrides reach cut faces too.
  ///
  /// Cutters whose bounds miss the target are skipped. A non-convex cutter is skipped and its brush id
  /// appended to out_pRefusedCutters. Room shells are carved as rooms (plMeshCSG::CarveRooms); without a
  /// room context the target's own rooms are the only rooms there are.
  PL_MODELINGPLUGIN_DLL void Evaluate(plEditableMesh& inout_mesh, const plTransform& targetGlobalTransform, plArrayPtr<const plBrushCutterInput> cutters,
    const plBrushFaceAttributeMap& attributes, plDynamicArray<plUInt32>* out_pRefusedCutters = nullptr, const plBrushRoomContext* pRooms = nullptr);

  /// \brief Evaluate without applying face attributes: the carved base mesh, as a bake writes it.
  PL_MODELINGPLUGIN_DLL void Carve(plEditableMesh& inout_mesh, const plTransform& targetGlobalTransform, plArrayPtr<const plBrushCutterInput> cutters,
    plDynamicArray<plUInt32>* out_pRefusedCutters = nullptr, const plBrushRoomContext* pRooms = nullptr);
} // namespace plBrushEvaluator
