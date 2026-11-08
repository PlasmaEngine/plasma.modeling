#include <ModelingPlugin/ModelingPluginPCH.h>

#include <ModelingPlugin/Modeling/BrushComponent.h>
#include <ModelingPlugin/Modeling/BrushGroupComponent.h>
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

void plBrushWorldModule::CaptureState(plBrushComponent* pComponent, BrushState& out_state) const
{
  out_state.m_Transform = pComponent->GetOwner()->GetGlobalTransform();
  out_state.m_uiRevision = pComponent->GetAuthoredRevision();
  out_state.m_bSubtractive = pComponent->GetSubtractive();
  out_state.m_bHasRooms = !out_state.m_bSubtractive && !GetRoomShells(pComponent).m_Faces.IsEmpty();
  out_state.m_pScope = plBrushGroupComponent::FindCutterScope(pComponent->GetOwner());
  out_state.m_WorldBounds = pComponent->GetOwner()->GetGlobalBounds().GetBox();
}

const plEditableMesh& plBrushWorldModule::GetRoomShells(const plBrushComponent* pComponent) const
{
  CachedCutterMesh& cached = m_RoomMeshes[pComponent];

  if (cached.m_uiRevision != pComponent->GetAuthoredRevision())
  {
    plEditableMesh mesh;
    pComponent->BuildEditableMesh(mesh);
    plBrushEvaluator::ExtractRoomShells(mesh, cached.m_Mesh);
    cached.m_uiRevision = pComponent->GetAuthoredRevision();
  }

  return cached.m_Mesh;
}

void plBrushWorldModule::RegisterBrush(plBrushComponent* pComponent)
{
  BrushState& state = m_Brushes[pComponent];
  CaptureState(pComponent, state);

  if (!state.m_WorldBounds.IsValid())
    return;

  // a cutter appearing must carve everything it already overlaps; a room appearing may take over passages
  if (state.m_bSubtractive)
  {
    InvalidateAroundCutters(state.m_WorldBounds, state.m_pScope, true);
  }
  else if (state.m_bHasRooms)
  {
    InvalidateAroundCutters(state.m_WorldBounds, state.m_pScope, false, pComponent);
  }
}

void plBrushWorldModule::DeregisterBrush(plBrushComponent* pComponent)
{
  m_CutterMeshes.Remove(pComponent);
  m_RoomMeshes.Remove(pComponent);

  auto it = m_Brushes.Find(pComponent);

  if (!it.IsValid())
    return;

  const BrushState state = it.Value();
  m_Brushes.Remove(it);

  if (!state.m_WorldBounds.IsValid())
    return;

  if (state.m_bSubtractive)
  {
    InvalidateAroundCutters(state.m_WorldBounds, state.m_pScope, true);
  }
  else if (state.m_bHasRooms)
  {
    InvalidateAroundCutters(state.m_WorldBounds, state.m_pScope, false);
  }
}

void plBrushWorldModule::GatherCutters(const plBrushComponent* pTarget, const plBoundingBox& targetWorldBounds, plDynamicArray<plBrushCutterInput>& out_cutters, bool bForRooms) const
{
  out_cutters.Clear();

  if (!targetWorldBounds.IsValid())
    return;

  const plGameObject* pScope = plBrushGroupComponent::FindCutterScope(pTarget->GetOwner());

  plHybridArray<const plBrushComponent*, 8> found;

  auto gather = [&](const plBoundingBox& bounds)
  {
    found.Clear();

    for (auto it = m_Brushes.GetIterator(); it.IsValid(); ++it)
    {
      const plBrushComponent* pCutter = it.Key();
      const BrushState& state = it.Value();

      if (pCutter == pTarget || !state.m_bSubtractive || state.m_pScope != pScope)
        continue;

      if (!state.m_WorldBounds.IsValid() || !state.m_WorldBounds.Overlaps(bounds))
        continue;

      found.PushBack(pCutter);
    }
  };

  gather(targetWorldBounds);

  if (bForRooms && !found.IsEmpty())
  {
    plBoundingBox reach = targetWorldBounds;

    for (const plBrushComponent* pCutter : found)
    {
      reach.ExpandToInclude(m_Brushes.GetValue(const_cast<plBrushComponent*>(pCutter))->m_WorldBounds);
    }

    gather(reach);
  }

  found.Sort([](const plBrushComponent* pA, const plBrushComponent* pB)
    { return plBrushEvaluator::CutterLess(pA->GetBrushId(), pA->GetOwner()->GetGlobalPosition(), pB->GetBrushId(), pB->GetOwner()->GetGlobalPosition()); });

  for (const plBrushComponent* pCutter : found)
  {
    // a cutter's mesh is rebuilt only when its authored inputs change, not per target
    CachedCutterMesh& cached = m_CutterMeshes[pCutter];

    if (cached.m_uiRevision != pCutter->GetAuthoredRevision())
    {
      pCutter->BuildEditableMesh(cached.m_Mesh);
      cached.m_uiRevision = pCutter->GetAuthoredRevision();
    }

    plBrushCutterInput& input = out_cutters.ExpandAndGetRef();
    input.m_Mesh = cached.m_Mesh;
    input.m_GlobalTransform = pCutter->GetOwner()->GetGlobalTransform();
    input.m_uiBrushId = pCutter->GetBrushId();
    input.m_WorldBounds = m_Brushes.GetValue(const_cast<plBrushComponent*>(pCutter))->m_WorldBounds;
  }
}

void plBrushWorldModule::GatherRooms(const plBrushComponent* pTarget, plArrayPtr<const plBrushCutterInput> cutters, plDynamicArray<plBrushRoomInput>& out_rooms) const
{
  out_rooms.Clear();

  if (cutters.IsEmpty())
    return;

  const plGameObject* pScope = plBrushGroupComponent::FindCutterScope(pTarget->GetOwner());

  for (auto it = m_Brushes.GetIterator(); it.IsValid(); ++it)
  {
    const plBrushComponent* pRoom = it.Key();
    const BrushState& state = it.Value();

    if (pRoom == pTarget || !state.m_bHasRooms || state.m_pScope != pScope || !state.m_WorldBounds.IsValid())
      continue;

    bool bReached = false;

    for (const plBrushCutterInput& cutter : cutters)
    {
      bReached |= cutter.m_WorldBounds.IsValid() && cutter.m_WorldBounds.Overlaps(state.m_WorldBounds);
    }

    if (!bReached)
      continue;

    plBrushRoomInput& input = out_rooms.ExpandAndGetRef();
    input.m_Mesh = GetRoomShells(pRoom);
    input.m_GlobalTransform = pRoom->GetOwner()->GetGlobalTransform();
    input.m_uiBrushId = pRoom->GetBrushId();
  }
}

void plBrushWorldModule::InvalidateAroundCutters(const plBoundingBox& worldBounds, const plGameObject* pScope, bool bAllAdditives, const plBrushComponent* pExclude)
{
  // the region rooms can be affected in: the bounds plus every cutter reaching into them
  plBoundingBox reach = worldBounds;

  for (auto it = m_Brushes.GetIterator(); it.IsValid(); ++it)
  {
    const BrushState& state = it.Value();

    if (state.m_bSubtractive && state.m_pScope == pScope && state.m_WorldBounds.IsValid() && state.m_WorldBounds.Overlaps(worldBounds))
      reach.ExpandToInclude(state.m_WorldBounds);
  }

  for (auto it = m_Brushes.GetIterator(); it.IsValid(); ++it)
  {
    BrushState& state = it.Value();

    if (it.Key() == pExclude || state.m_bSubtractive || state.m_pScope != pScope || !state.m_WorldBounds.IsValid())
      continue;

    const bool bAffected = (bAllAdditives && state.m_WorldBounds.Overlaps(worldBounds)) || (state.m_bHasRooms && state.m_WorldBounds.Overlaps(reach));

    if (!bAffected)
      continue;

    it.Key()->InvalidateBrushMesh();

    // the re-generated mesh may grow or shrink — refresh the cached bounds
    state.m_WorldBounds = it.Key()->GetOwner()->GetGlobalBounds().GetBox();
  }
}

void plBrushWorldModule::DetectChanges(const plWorldModule::UpdateContext& context)
{
  PL_IGNORE_UNUSED(context);

  for (auto it = m_Brushes.GetIterator(); it.IsValid(); ++it)
  {
    plBrushComponent* pComponent = it.Key();
    BrushState& state = it.Value();

    // first, so the state captured below sees the new bounds
    pComponent->RebuildMeshIfDirty();

    const plTransform transform = pComponent->GetOwner()->GetGlobalTransform();
    const plUInt32 uiRevision = pComponent->GetAuthoredRevision();
    const bool bSubtractive = pComponent->GetSubtractive();
    const plGameObject* pScope = plBrushGroupComponent::FindCutterScope(pComponent->GetOwner());

    const bool bTransformChanged = transform != state.m_Transform;
    const bool bContentChanged = uiRevision != state.m_uiRevision;
    const bool bRoleChanged = bSubtractive != state.m_bSubtractive;
    const bool bScopeChanged = pScope != state.m_pScope;

    if (!bTransformChanged && !bContentChanged && !bRoleChanged && !bScopeChanged)
      continue;

    const BrushState oldState = state;
    CaptureState(pComponent, state);

    if (bSubtractive || bRoleChanged)
    {
      // a changed cutter re-carves everything it used to touch and everything it touches now
      if (oldState.m_WorldBounds.IsValid() && (oldState.m_bSubtractive || bRoleChanged))
      {
        InvalidateAroundCutters(oldState.m_WorldBounds, oldState.m_pScope, true);
      }

      if (state.m_WorldBounds.IsValid())
      {
        InvalidateAroundCutters(state.m_WorldBounds, state.m_pScope, true);
      }

      pComponent->InvalidateBrushMesh();
    }
    else if (bTransformChanged || bScopeChanged)
    {
      // a moved or regrouped additive brush intersects a different set of cutters
      pComponent->InvalidateBrushMesh();
      state.m_WorldBounds = pComponent->GetOwner()->GetGlobalBounds().GetBox();
    }

    // rooms sharing a cutter with this one may gain or lose its passage
    if (!bSubtractive && (state.m_bHasRooms || oldState.m_bHasRooms))
    {
      if (oldState.m_WorldBounds.IsValid())
        InvalidateAroundCutters(oldState.m_WorldBounds, oldState.m_pScope, false, pComponent);

      if (state.m_WorldBounds.IsValid())
        InvalidateAroundCutters(state.m_WorldBounds, state.m_pScope, false, pComponent);
    }
  }
}


PL_STATICLINK_FILE(ModelingPlugin, ModelingPlugin_Modeling_Implementation_BrushWorldModule);
