#pragma once

#include <EditorFramework/DragDrop/AssetDragDropHandler.h>
#include <EditorPluginModeling/EditorPluginModelingDLL.h>
#include <ModelingPlugin/Modeling/EditableMesh.h>

class plCommand;

/// \brief Material drag & drop with face granularity for brushes.
///
/// Outranks the scene plugin's material handler (priority 1.5 vs 1.0) and therefore replicates its
/// mesh-component and grey-box behavior — a handler wins the whole drag, not just brush targets.
/// On a brush the face under the cursor is resolved CPU-side (plBrushPicking) and only that face's
/// slot assignment changes, with live preview while dragging via the temporary-command session.
class plBrushFaceMaterialDragDropHandler : public plAssetDragDropHandler
{
  PL_ADD_DYNAMIC_REFLECTION(plBrushFaceMaterialDragDropHandler, plAssetDragDropHandler);

protected:
  virtual void RequestConfiguration(plDragDropConfig* pConfigToFillOut) override;
  virtual float CanHandle(const plDragDropInfo* pInfo) const override;
  virtual void OnDragBegin(const plDragDropInfo* pInfo) override;
  virtual void OnDragUpdate(const plDragDropInfo* pInfo) override;
  virtual void OnDragCancel() override;
  virtual void OnDrop(const plDragDropInfo* pInfo) override;

private:
  void ApplyToBrushFace(const plDragDropInfo* pInfo, const plDocumentObject* pComponent);

  /// \brief Takes back the assignment previewed on the previous face or part.
  void RevertPreview();

  /// \brief Runs the commands as one transaction; a failure cancels it instead of leaving it open.
  void RunInTransaction(plStringView sName, plArrayPtr<plCommand* const> commands);
  void RunInTransaction(plStringView sName, plCommand& ref_cmd);

  const plDocument* m_pDocument = nullptr;
  plUuid m_AppliedToComponent;
  plInt32 m_iAppliedToSlot = -1;
  plUInt32 m_uiAppliedFaceId = plInvalidIndex;

  // the evaluated mesh of the brush under the cursor: the drag only changes face attributes, never the
  // geometry, so it is built once per brush instead of on every mouse move
  plUuid m_PickMeshComponent;
  plEditableMesh m_PickMesh;
  plTransform m_PickMeshInverseTransform = plTransform::MakeIdentity();
};
