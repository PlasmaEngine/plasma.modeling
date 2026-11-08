#include <ModelingPlugin/ModelingPluginPCH.h>

#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <ModelingPlugin/Modeling/BrushComponent.h>
#include <ModelingPlugin/Modeling/BrushGroupComponent.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plBrushGroupComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ACCESSOR_PROPERTY("IsolateCutters", GetIsolateCutters, SetIsolateCutters)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_ACCESSOR_PROPERTY("MergeOnExport", GetMergeOnExport, SetMergeOnExport)->AddAttributes(new plDefaultValueAttribute(true)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Construction"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE;
// clang-format on

plBrushGroupComponent::plBrushGroupComponent() = default;
plBrushGroupComponent::~plBrushGroupComponent() = default;

void plBrushGroupComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  plStreamWriter& s = inout_stream.GetStream();

  s << m_bIsolateCutters;
  s << m_bMergeOnExport;
}

void plBrushGroupComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  plStreamReader& s = inout_stream.GetStream();

  s >> m_bIsolateCutters;
  s >> m_bMergeOnExport;
}

void plBrushGroupComponent::SetIsolateCutters(bool b)
{
  m_bIsolateCutters = b;
}

const plBrushGroupComponent* plBrushGroupComponent::FindGroup(const plGameObject* pObject)
{
  for (const plGameObject* pCurrent = pObject; pCurrent != nullptr; pCurrent = pCurrent->GetParent())
  {
    const plBrushGroupComponent* pGroup = nullptr;

    if (pCurrent->TryGetComponentOfBaseType(pGroup) && pGroup->IsActive())
      return pGroup;
  }

  return nullptr;
}

const plGameObject* plBrushGroupComponent::FindCutterScope(const plGameObject* pObject)
{
  for (const plGameObject* pCurrent = pObject; pCurrent != nullptr; pCurrent = pCurrent->GetParent())
  {
    const plBrushGroupComponent* pGroup = nullptr;

    if (pCurrent->TryGetComponentOfBaseType(pGroup) && pGroup->IsActive() && pGroup->GetIsolateCutters())
      return pCurrent;
  }

  return nullptr;
}


PL_STATICLINK_FILE(ModelingPlugin, ModelingPlugin_Modeling_Implementation_BrushGroupComponent);
