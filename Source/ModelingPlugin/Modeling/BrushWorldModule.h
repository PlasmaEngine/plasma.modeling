#pragma once

#include <ModelingPlugin/ModelingPluginDLL.h>

#include <Core/World/World.h>
#include <Core/World/WorldModule.h>
#include <Foundation/Containers/Map.h>
#include <ModelingPlugin/Modeling/BrushEvaluator.h>

class plBrushComponent;

/// \brief Keeps non-destructive subtraction live: watches every brush for changes and re-evaluates the
/// additive brushes a moved or edited cutter overlaps.
///
/// Detection runs once per frame by comparing each brush's transform, authored revision, role and cutter
/// scope against the last seen state — components have no reliable transform-changed callback, and the
/// comparison costs the same no matter how large the meshes are. Cutters are handed out in brush-id order
/// (plBrushEvaluator::CutterLess), the same order the editor uses, so cut face ids match across processes.
class PL_MODELINGPLUGIN_DLL plBrushWorldModule : public plWorldModule
{
  PL_DECLARE_WORLD_MODULE();
  PL_ADD_DYNAMIC_REFLECTION(plBrushWorldModule, plWorldModule);

public:
  plBrushWorldModule(plWorld* pWorld);
  ~plBrushWorldModule();

  virtual void Initialize() override;

  void RegisterBrush(plBrushComponent* pComponent);
  void DeregisterBrush(plBrushComponent* pComponent);

  /// \brief Every cutter that can cut pTarget: subtractive, in the same cutter scope, bounds overlapping
  /// the target's base mesh bounds (world space). Sorted into evaluation order. With bForRooms, also the
  /// cutters overlapping those, since a room's passage ends where another cutter begins.
  void GatherCutters(const plBrushComponent* pTarget, const plBoundingBox& targetWorldBounds, plDynamicArray<plBrushCutterInput>& out_cutters, bool bForRooms = false) const;

  /// \brief The room shells of every other additive brush in pTarget's scope that one of the cutters reaches.
  void GatherRooms(const plBrushComponent* pTarget, plArrayPtr<const plBrushCutterInput> cutters, plDynamicArray<plBrushRoomInput>& out_rooms) const;

private:
  void DetectChanges(const plWorldModule::UpdateContext& context);

  /// \brief Invalidates the room brushes overlapping the bounds or any cutter that overlaps them (rooms
  /// sharing a cutter decide together which one draws its passage), and with bAllAdditives every other
  /// additive brush overlapping the bounds.
  void InvalidateAroundCutters(const plBoundingBox& worldBounds, const plGameObject* pScope, bool bAllAdditives, const plBrushComponent* pExclude = nullptr);

  struct BrushState
  {
    plTransform m_Transform = plTransform::MakeIdentity();
    plUInt32 m_uiRevision = 0;
    bool m_bSubtractive = false;
    bool m_bHasRooms = false;
    const plGameObject* m_pScope = nullptr;
    plBoundingBox m_WorldBounds = plBoundingBox::MakeInvalid();
  };

  struct CachedCutterMesh
  {
    plUInt32 m_uiRevision = plInvalidIndex;
    plEditableMesh m_Mesh;
  };

  void CaptureState(plBrushComponent* pComponent, BrushState& out_state) const;

  /// The brush's room shells, rebuilt only when its authored inputs change.
  const plEditableMesh& GetRoomShells(const plBrushComponent* pComponent) const;

  plMap<plBrushComponent*, BrushState> m_Brushes;
  mutable plMap<const plBrushComponent*, CachedCutterMesh> m_CutterMeshes;
  mutable plMap<const plBrushComponent*, CachedCutterMesh> m_RoomMeshes;
};
