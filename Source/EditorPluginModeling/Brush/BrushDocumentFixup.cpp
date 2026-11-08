#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorPluginModeling/Brush/BrushDocumentFixup.h>
#include <EditorPluginModeling/Brush/BrushPicking.h>
#include <Foundation/Math/Random.h>
#include <Foundation/Time/Time.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <ToolsFoundation/CommandHistory/CommandHistory.h>
#include <ToolsFoundation/Document/Document.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

namespace
{
  void CollectBrushes(const plDocumentObject* pObject, plHybridArray<plUuid, 8>& ref_brushes)
  {
    if (plBrushPicking::IsBrushComponent(pObject))
    {
      if (!ref_brushes.Contains(pObject->GetGuid()))
        ref_brushes.PushBack(pObject->GetGuid());
    }

    for (const plDocumentObject* pChild : pObject->GetChildren())
    {
      CollectBrushes(pChild, ref_brushes);
    }
  }

  plUInt32 MakeUniqueId(const plHashSet<plUInt32>& used)
  {
    static plRandom s_Random;
    static bool s_bSeeded = false;

    if (!s_bSeeded)
    {
      s_Random.Initialize(static_cast<plUInt64>(plTime::Now().GetNanoseconds()));
      s_bSeeded = true;
    }

    while (true)
    {
      // 0 means "unassigned"
      const plUInt32 uiId = s_Random.UInt();

      if (uiId != 0 && !used.Contains(uiId))
        return uiId;
    }
  }

  /// Gives a fresh id to every brush in `candidates` that has none or shares one. Returns whether any
  /// command was issued. Must run inside an open transaction.
  bool FixIds(plDocument* pDocument, plArrayPtr<const plUuid> candidates)
  {
    plDynamicArray<const plDocumentObject*> brushes;
    plBrushPicking::FindAllBrushComponents(pDocument->GetObjectManager()->GetRootObject(), brushes);

    // ids held by brushes that keep theirs
    plHashTable<plUInt32, plUInt32> useCount;
    for (const plDocumentObject* pBrush : brushes)
    {
      useCount[pBrush->GetTypeAccessor().GetValue("BrushId").ConvertTo<plUInt32>()]++;
    }

    plHashSet<plUInt32> used;
    for (auto it : useCount)
    {
      used.Insert(it.Key());
    }

    bool bAny = false;

    for (const plUuid& guid : candidates)
    {
      const plDocumentObject* pBrush = pDocument->GetObjectManager()->GetObject(guid);

      if (pBrush == nullptr)
        continue;

      const plUInt32 uiId = pBrush->GetTypeAccessor().GetValue("BrushId").ConvertTo<plUInt32>();

      if (uiId != 0 && useCount[uiId] <= 1)
        continue;

      const plUInt32 uiNewId = MakeUniqueId(used);
      used.Insert(uiNewId);

      if (uiId != 0)
        useCount[uiId]--;

      plSetObjectPropertyCommand cmd;
      cmd.m_Object = guid;
      cmd.m_sProperty = "BrushId";
      cmd.m_NewValue = uiNewId;
      pDocument->GetCommandHistory()->AddCommand(cmd).LogFailure();

      bAny = true;
    }

    return bAny;
  }
} // namespace

plBrushDocumentFixup::plBrushDocumentFixup(plDocument* pDocument)
  : m_pDocument(pDocument)
{
  m_pDocument->GetObjectManager()->m_StructureEvents.AddEventHandler(plMakeDelegate(&plBrushDocumentFixup::StructureEventHandler, this));
  m_pDocument->GetCommandHistory()->m_Events.AddEventHandler(plMakeDelegate(&plBrushDocumentFixup::CommandHistoryEventHandler, this));
}

plBrushDocumentFixup::~plBrushDocumentFixup()
{
  m_pDocument->GetCommandHistory()->m_Events.RemoveEventHandler(plMakeDelegate(&plBrushDocumentFixup::CommandHistoryEventHandler, this));
  m_pDocument->GetObjectManager()->m_StructureEvents.RemoveEventHandler(plMakeDelegate(&plBrushDocumentFixup::StructureEventHandler, this));
}

void plBrushDocumentFixup::AssignMissingIds()
{
  plDynamicArray<const plDocumentObject*> brushes;
  plBrushPicking::FindAllBrushComponents(m_pDocument->GetObjectManager()->GetRootObject(), brushes);

  plHashTable<plUInt32, plUInt32> useCount;
  bool bNeedsFix = false;

  for (const plDocumentObject* pBrush : brushes)
  {
    const plUInt32 uiId = pBrush->GetTypeAccessor().GetValue("BrushId").ConvertTo<plUInt32>();

    if (uiId == 0 || ++useCount[uiId] > 1)
    {
      bNeedsFix = true;
    }
  }

  if (!bNeedsFix)
    return;

  plHybridArray<plUuid, 8> guids;
  for (const plDocumentObject* pBrush : brushes)
  {
    guids.PushBack(pBrush->GetGuid());
  }

  auto* pHistory = m_pDocument->GetCommandHistory();
  pHistory->StartTransaction("Assign Brush Ids");
  FixIds(m_pDocument, guids);
  pHistory->FinishTransaction();

  plLog::Info("Assigned brush ids to brushes saved before ids existed. Save the scene to keep them.");
}

void plBrushDocumentFixup::StructureEventHandler(const plDocumentObjectStructureEvent& e)
{
  if (e.m_EventType != plDocumentObjectStructureEvent::Type::AfterObjectAdded || e.m_pObject == nullptr)
    return;

  const plCommandHistory* pHistory = m_pDocument->GetCommandHistory();

  // undo/redo restores ids exactly; only new objects need checking
  if (pHistory->IsInUndoRedo() || !pHistory->IsInTransaction())
    return;

  CollectBrushes(e.m_pObject, m_Pending);
}

void plBrushDocumentFixup::CommandHistoryEventHandler(const plCommandHistoryEvent& e)
{
  switch (e.m_Type)
  {
    case plCommandHistoryEvent::Type::BeforeTransactionEnded:
    {
      if (m_Pending.IsEmpty())
        break;

      // swap out first: the commands below raise structure/property events of their own
      plHybridArray<plUuid, 8> pending;
      pending.Swap(m_Pending);

      FixIds(m_pDocument, pending);
      break;
    }

    case plCommandHistoryEvent::Type::BeforeTransactionCanceled:
    case plCommandHistoryEvent::Type::TransactionCanceled:
    case plCommandHistoryEvent::Type::TransactionEnded:
      m_Pending.Clear();
      break;

    default:
      break;
  }
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_BrushDocumentFixup);
