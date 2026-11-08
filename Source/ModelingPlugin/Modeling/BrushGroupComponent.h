#pragma once

#include <ModelingPlugin/ModelingPluginDLL.h>

#include <Core/World/Component.h>
#include <Core/World/World.h>

using plBrushGroupComponentManager = plComponentManager<class plBrushGroupComponent, plBlockStorageType::Compact>;

/// \brief Groups the brushes below its object. With IsolateCutters, cutters inside the group only cut
/// brushes inside it, and brushes inside it are only cut by its own cutters. At scene export the group's
/// static brushes merge into one mesh per material when MergeOnExport is set.
class PL_MODELINGPLUGIN_DLL plBrushGroupComponent : public plComponent
{
  PL_DECLARE_COMPONENT_TYPE(plBrushGroupComponent, plComponent, plBrushGroupComponentManager);

public:
  plBrushGroupComponent();
  ~plBrushGroupComponent();

  virtual void SerializeComponent(plWorldWriter& inout_stream) const override;
  virtual void DeserializeComponent(plWorldReader& inout_stream) override;

  void SetIsolateCutters(bool b);                                // [ property ]
  bool GetIsolateCutters() const { return m_bIsolateCutters; }   // [ property ]

  void SetMergeOnExport(bool b) { m_bMergeOnExport = b; }        // [ property ]
  bool GetMergeOnExport() const { return m_bMergeOnExport; }     // [ property ]

  /// \brief The object whose subtree forms the cutting scope of a brush on pObject: the nearest ancestor
  /// (or the object itself) carrying an isolating group, nullptr for the scene-wide scope.
  static const plGameObject* FindCutterScope(const plGameObject* pObject);

  /// \brief The nearest group component on the object or its ancestors, isolating or not.
  static const plBrushGroupComponent* FindGroup(const plGameObject* pObject);

private:
  bool m_bIsolateCutters = true;
  bool m_bMergeOnExport = true;
};
