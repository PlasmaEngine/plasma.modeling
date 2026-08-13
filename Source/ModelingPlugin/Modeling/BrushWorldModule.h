#pragma once

#include <ModelingPlugin/ModelingPluginDLL.h>

#include <Core/World/World.h>
#include <Core/World/WorldModule.h>
#include <Foundation/Containers/Map.h>

class plBrushComponent;

/// \brief Keeps non-destructive subtraction live: watches every brush for changes and re-evaluates the
/// additive brushes a moved or edited cutter overlaps.
///
/// Detection runs once per frame by comparing each brush's transform and content hash against the last
/// seen state — components have no reliable transform-changed callback, and at blockout brush counts
/// the comparison is trivial. Cutter queries are sorted deterministically (world position), so the
/// engine and the editor evaluate cutters in the same order and cap face ids match across processes.
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

  /// \brief All subtractive brushes whose bounds overlap the given world-space box, in deterministic order.
  void QueryCutters(const plBoundingBox& worldBounds, plDynamicArray<const plBrushComponent*>& out_cutters) const;

private:
  void DetectChanges(const plWorldModule::UpdateContext& context);
  void InvalidateOverlappingAdditives(const plBoundingBox& worldBounds);

  struct BrushState
  {
    plTransform m_Transform = plTransform::MakeIdentity();
    plUInt64 m_uiContentHash = 0;
    bool m_bSubtractive = false;
    plBoundingBox m_WorldBounds = plBoundingBox::MakeInvalid();
  };

  plMap<plBrushComponent*, BrushState> m_Brushes;
};
