#include <ModelingPlugin/ModelingPluginPCH.h>

#include <ModelingPlugin/Modeling/BrushComponent.h>
#include <ModelingPlugin/Modeling/BrushWorldModule.h>

// clang-format off
PL_IMPLEMENT_WORLD_MODULE(plBrushWorldModule);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plBrushWorldModule, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plBrushWorldModule::plBrushWorldModule(plWorld* pWorld)
  : plWorldModule(pWorld)
{
}

plBrushWorldModule::~plBrushWorldModule() = default;

void plBrushWorldModule::Initialize()
{
  SUPER::Initialize();

  {
    auto updateDesc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plBrushWorldModule::DetectChanges, this);
    updateDesc.m_Phase = plWorldModule::UpdateFunctionDesc::Phase::PreAsync;

    RegisterUpdateFunction(updateDesc);
  }
}

void plBrushWorldModule::RegisterBrush(plBrushComponent* pComponent)
{
  BrushState& state = m_Brushes[pComponent];
  state.m_Transform = pComponent->GetOwner()->GetGlobalTransform();
  state.m_uiContentHash = pComponent->ComputeContentHash();
  state.m_bSubtractive = pComponent->GetSubtractive();
  state.m_WorldBounds = pComponent->GetOwner()->GetGlobalBounds().GetBox();

  // a cutter appearing must carve everything it already overlaps
  if (state.m_bSubtractive && state.m_WorldBounds.IsValid())
  {
    InvalidateOverlappingAdditives(state.m_WorldBounds);
  }
}

void plBrushWorldModule::DeregisterBrush(plBrushComponent* pComponent)
{
  auto it = m_Brushes.Find(pComponent);

  if (!it.IsValid())
    return;

  const BrushState state = it.Value();
  m_Brushes.Remove(it);

  if (state.m_bSubtractive && state.m_WorldBounds.IsValid())
  {
    InvalidateOverlappingAdditives(state.m_WorldBounds);
  }
}

void plBrushWorldModule::QueryCutters(const plBoundingBox& worldBounds, plDynamicArray<const plBrushComponent*>& out_cutters) const
{
  out_cutters.Clear();

  for (auto it = m_Brushes.GetIterator(); it.IsValid(); ++it)
  {
    if (!it.Value().m_bSubtractive)
      continue;

    if (!it.Value().m_WorldBounds.IsValid() || !it.Value().m_WorldBounds.Overlaps(worldBounds))
      continue;

    out_cutters.PushBack(it.Key());
  }

  // deterministic evaluation order — the editor-side evaluation sorts the same way, which keeps cap
  // face ids identical across processes
  out_cutters.Sort([](const plBrushComponent* pA, const plBrushComponent* pB)
    {
      const plVec3 a = pA->GetOwner()->GetGlobalPosition();
      const plVec3 b = pB->GetOwner()->GetGlobalPosition();

      if (a.x != b.x)
        return a.x < b.x;
      if (a.y != b.y)
        return a.y < b.y;
      return a.z < b.z;
    });
}

void plBrushWorldModule::InvalidateOverlappingAdditives(const plBoundingBox& worldBounds)
{
  for (auto it = m_Brushes.GetIterator(); it.IsValid(); ++it)
  {
    if (it.Value().m_bSubtractive)
      continue;

    if (it.Value().m_WorldBounds.IsValid() && it.Value().m_WorldBounds.Overlaps(worldBounds))
    {
      it.Key()->InvalidateBrushMesh();

      // the re-generated mesh may grow or shrink — refresh the cached bounds
      it.Value().m_WorldBounds = it.Key()->GetOwner()->GetGlobalBounds().GetBox();
    }
  }
}

void plBrushWorldModule::DetectChanges(const plWorldModule::UpdateContext& context)
{
  PL_IGNORE_UNUSED(context);

  for (auto it = m_Brushes.GetIterator(); it.IsValid(); ++it)
  {
    plBrushComponent* pComponent = it.Key();
    BrushState& state = it.Value();

    const plTransform transform = pComponent->GetOwner()->GetGlobalTransform();
    const plUInt64 uiContentHash = pComponent->ComputeContentHash();
    const bool bSubtractive = pComponent->GetSubtractive();

    const bool bTransformChanged = transform != state.m_Transform;
    const bool bContentChanged = uiContentHash != state.m_uiContentHash;
    const bool bRoleChanged = bSubtractive != state.m_bSubtractive;

    if (!bTransformChanged && !bContentChanged && !bRoleChanged)
      continue;

    const plBoundingBox oldBounds = state.m_WorldBounds;

    state.m_Transform = transform;
    state.m_uiContentHash = uiContentHash;
    state.m_bSubtractive = bSubtractive;
    state.m_WorldBounds = pComponent->GetOwner()->GetGlobalBounds().GetBox();

    if (bSubtractive || bRoleChanged)
    {
      // a changed cutter re-carves everything it used to touch and everything it touches now
      if (oldBounds.IsValid())
      {
        InvalidateOverlappingAdditives(oldBounds);
      }

      if (state.m_WorldBounds.IsValid() && (!oldBounds.IsValid() || !oldBounds.Contains(state.m_WorldBounds)))
      {
        InvalidateOverlappingAdditives(state.m_WorldBounds);
      }

      pComponent->InvalidateBrushMesh();
    }
    else if (bTransformChanged)
    {
      // a moved additive brush intersects a different set of cutters
      pComponent->InvalidateBrushMesh();
    }
  }
}


PL_STATICLINK_FILE(ModelingPlugin, ModelingPlugin_Modeling_Implementation_BrushWorldModule);
