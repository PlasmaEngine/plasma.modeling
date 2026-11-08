#pragma once

#include <EditorEngineProcessFramework/SceneExport/SceneExportModifier.h>
#include <EnginePluginModeling/EnginePluginModelingDLL.h>

/// \brief At scene export, turns live brush CSG into plain geometry: every additive brush stores its
/// final mesh (cuts and face attributes applied), cutters are removed, and static brushes inside a
/// merging brush group are combined into one brush per shared output settings. A loaded level then runs
/// no CSG at all. Play-in-editor keeps the live brushes.
class PL_ENGINEPLUGINMODELING_DLL plSceneExportModifier_BakeBrushes : public plSceneExportModifier
{
  PL_ADD_DYNAMIC_REFLECTION(plSceneExportModifier_BakeBrushes, plSceneExportModifier);

public:
  virtual void ModifyWorld(plWorld& ref_world, plStringView sDocumentType, const plUuid& documentGuid, bool bForExport) override;
};
