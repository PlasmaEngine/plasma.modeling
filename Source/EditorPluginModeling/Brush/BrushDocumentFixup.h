#pragma once

#include <EditorPluginModeling/EditorPluginModelingDLL.h>
#include <Foundation/Types/Uuid.h>

class plDocument;
struct plDocumentObjectStructureEvent;
struct plCommandHistoryEvent;

/// \brief Keeps every brush's BrushId unique within its scene. Brushes added by a transaction (drawn,
/// pasted, duplicated, instantiated) are collected as they arrive and, just before the transaction ends,
/// any that has no id or shares one gets a fresh id — inside the same undo step.
class PL_EDITORPLUGINMODELING_DLL plBrushDocumentFixup
{
public:
  plBrushDocumentFixup(plDocument* pDocument);
  ~plBrushDocumentFixup();

  /// \brief One-off pass for scenes saved before brush ids existed. Adds a single undo step, only when
  /// something needed fixing.
  void AssignMissingIds();

private:
  void StructureEventHandler(const plDocumentObjectStructureEvent& e);
  void CommandHistoryEventHandler(const plCommandHistoryEvent& e);

  plDocument* m_pDocument = nullptr;
  plHybridArray<plUuid, 8> m_Pending;
};
