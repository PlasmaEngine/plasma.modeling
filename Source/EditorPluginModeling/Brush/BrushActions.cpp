#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorFramework/Actions/TransformGizmoActions.h>
#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorFramework/DocumentWindow/EngineDocumentWindow.h>
#include <EditorFramework/InputContexts/EditorInputEvent.h>
#include <EditorPluginModeling/Brush/BrushActions.h>
#include <EditorPluginModeling/Brush/BrushCommands.h>
#include <EditorPluginModeling/Brush/BrushEditTool.h>
#include <EditorUi/KeyChord.h>
#include <GuiFoundation/Action/ActionManager.h>
#include <GuiFoundation/Action/ActionMapManager.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plModelingAction, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plActionDescriptorHandle plBrushActions::s_hBrushTool;
plActionDescriptorHandle plBrushActions::s_hMarkingMenuButton;
plActionDescriptorHandle plBrushActions::s_hCommands[plModelingCommand::Count];

namespace
{
  struct CommandInfo
  {
    const char* m_szActionName; ///< unique action name, shown in the shortcut editor
    const char* m_szDisplayName;
    const char* m_szDefaultShortcut;
  };

  // Defaults only apply while the Brush tool is active; the tool claims these keys from the editor then.
  const CommandInfo c_Commands[plModelingCommand::Count] = {
    {"Modeling.Mode.Object", "Object Mode", "Esc"},
    {"Modeling.Mode.Vertex", "Vertex Mode", "1"},
    {"Modeling.Mode.Edge", "Edge Mode", "2"},
    {"Modeling.Mode.Face", "Face Mode", "3"},
    {"Modeling.MarkingMenu", "Marking Menu", "Tab"},
    {"Modeling.Grab", "Grab", "G"},
    {"Modeling.Scale", "Scale", "S"},
    {"Modeling.Extrude", "Extrude", "Shift+E"},
    {"Modeling.Inset", "Inset", "I"},
    {"Modeling.Bevel", "Bevel", "Ctrl+B"},
    {"Modeling.LoopCut", "Loop Cut", "Alt+R"},
    {"Modeling.Bridge", "Bridge", "Alt+B"},
    {"Modeling.Weld", "Weld", "M"},
    {"Modeling.Flip", "Flip", "Alt+F"},
    {"Modeling.Subdivide", "Subdivide", "Alt+S"},
    {"Modeling.Detach", "Detach", "Alt+D"},
    {"Modeling.Delete", "Delete", "Del"},
    {"Modeling.FillHole", "Fill Hole", "Alt+H"},
    {"Modeling.Connect", "Connect", "J"},
    {"Modeling.Triangulate", "Triangulate", "Alt+T"},
    {"Modeling.Flatten", "Flatten", "Alt+P"},
    {"Modeling.Select.All", "Select All", "Ctrl+A"},
    {"Modeling.Select.None", "Select None", "Alt+A"},
    {"Modeling.Select.Invert", "Invert Selection", "Ctrl+Shift+I"},
    {"Modeling.Select.Grow", "Grow Selection", "Ctrl+="},
    {"Modeling.Select.Shrink", "Shrink Selection", "Ctrl+-"},
    {"Modeling.Select.Loop", "Select Loop", "L"},
    {"Modeling.Select.Ring", "Select Ring", "Alt+L"},
    {"Modeling.Select.SameMaterial", "Select Same Material", "Shift+M"},
    {"Modeling.Select.Through", "Select Through", "X"},
    {"Modeling.ShowHiddenHandles", "Show Hidden Handles", "Alt+X"},
    {"Modeling.SnapToGrid", "Snap To Grid", "Alt+G"},
    {"Modeling.Clip", "Clip", "Shift+C"},
    {"Modeling.ToggleCutter", "Make Cutter", "Alt+C"},
    {"Modeling.BakeCuts", "Bake Cuts", ""},
    {"Modeling.Hollow", "Hollow", ""},
    {"Modeling.Union", "Union", ""},
    {"Modeling.Decompose", "Decompose", ""},
    {"Modeling.Group", "Group Brushes", "Ctrl+Shift+G"},
    {"Modeling.Pivot.Edit", "Edit Pivot", "D"},
    {"Modeling.Pivot.Center", "Pivot To Center", ""},
    {"Modeling.Pivot.Bottom", "Pivot To Bottom", ""},
    {"Modeling.Pivot.Selection", "Pivot To Selection", ""},
    {"Modeling.Select.FaceGranularity", "Mesh / CSG Faces", "Alt+3"},
  };

  template <plModelingCommand::Enum Cmd>
  plAction* CreateModelingAction(const plActionContext& context)
  {
    return PL_DEFAULT_NEW(plModelingAction, context, c_Commands[Cmd].m_szActionName, Cmd);
  }

  template <plModelingCommand::Enum Cmd>
  plActionDescriptorHandle RegisterCommand()
  {
    return plActionManager::RegisterAction(plActionDescriptor(
      plActionType::Action, plActionScope::Document, c_Commands[Cmd].m_szActionName, "Modeling", c_Commands[Cmd].m_szDefaultShortcut, &CreateModelingAction<Cmd>));
  }

  template <plUInt32 Index>
  void RegisterAll(plActionDescriptorHandle* pHandles)
  {
    if constexpr (Index < plModelingCommand::Count)
    {
      pHandles[Index] = RegisterCommand<static_cast<plModelingCommand::Enum>(Index)>();
      RegisterAll<Index + 1>(pHandles);
    }
  }

  /// A shortcut as the key chord writes it ("Ctrl+B"); an empty text for none or one that does not parse.
  plString NormalizeShortcut(plStringView sShortcut)
  {
    plKeyChord chord;
    if (sShortcut.IsEmpty() || plKeyChord::Parse(sShortcut, chord).Failed())
      return plString();

    return chord.ToString();
  }

  plBrushToolContext* ActivateBrushTool(plGameObjectDocument* pGameDoc)
  {
    if (!pGameDoc->GetActiveEditTool() || !pGameDoc->GetActiveEditTool()->IsInstanceOf<plBrushEditTool>())
    {
      pGameDoc->SetActiveEditTool(plGetStaticRTTI<plBrushEditTool>());
    }

    plBrushEditTool* pTool = plDynamicCast<plBrushEditTool*>(pGameDoc->GetActiveEditTool());
    return pTool ? &pTool->GetToolContext() : nullptr;
  }

  void ShowStatus(plDocument* pDocument, const plStatus& status)
  {
    if (status.Succeeded() || status.m_sMessage.IsEmpty())
      return;

    if (plDocumentWindow* pWindow = plDocumentWindow::FindWindowByDocument(pDocument))
    {
      pWindow->ShowTemporaryStatusBarMsg(plFmt("{}", status.m_sMessage), plTime::MakeFromSeconds(6));
    }

    plLog::Warning("{}", status.m_sMessage);
  }
} // namespace

//////////////////////////////////////////////////////////////////////////

void plBrushActions::RegisterActions()
{
  // "B" — freed from grey boxing by the one-line change in the scene plugin's GizmoActions
  s_hBrushTool = PL_REGISTER_ACTION_1("Gizmo.Mode.Brush", plActionScope::Document, "Gizmo", "B", plGizmoAction, plGetStaticRTTI<plBrushEditTool>());

  RegisterAll<0>(s_hCommands);

  // the toolbar button has no key of its own: Tab stays with the Brush tool, which claims it only while active
  s_hMarkingMenuButton = plActionManager::RegisterAction(plActionDescriptor(
    plActionType::Action, plActionScope::Document, "Modeling.MarkingMenu.Button", "Modeling", "", &CreateModelingAction<plModelingCommand::MarkingMenu>));
}

void plBrushActions::UnregisterActions()
{
  plActionManager::UnregisterAction(s_hBrushTool);
  plActionManager::UnregisterAction(s_hMarkingMenuButton);

  for (plActionDescriptorHandle& hCommand : s_hCommands)
  {
    plActionManager::UnregisterAction(hCommand);
  }
}

void plBrushActions::MapActions()
{
  // Injecting into another plugin's maps: they exist only if the scene plugin loaded first, so a
  // missing map is a logged degradation rather than an assert.
  const char* szMenuBars[] = {"EditorPluginScene_DocumentMenuBar", "EditorPluginScene_Scene2MenuBar"};

  for (const char* szMapping : szMenuBars)
  {
    if (plActionMap* pMap = plActionMapManager::GetActionMap(szMapping))
    {
      pMap->MapAction(s_hBrushTool, "G.Gizmos", 5.5f);
    }
    else
    {
      plLog::Warning("Action map '{0}' not found - the Brush tool will not appear in that menu.", szMapping);
    }
  }

  const char* szToolBars[] = {"EditorPluginScene_DocumentToolBar", "EditorPluginScene_Scene2ToolBar"};

  for (const char* szMapping : szToolBars)
  {
    if (plActionMap* pMap = plActionMapManager::GetActionMap(szMapping))
    {
      pMap->MapAction(s_hBrushTool, "GizmoCategory", 5.5f);

      // opens the marking menu over the view, as its key does
      pMap->MapAction(s_hMarkingMenuButton, "GizmoCategory", 5.6f);
    }
    else
    {
      plLog::Warning("Action map '{0}' not found - the Brush tool will not appear in that toolbar.", szMapping);
    }
  }
}

const char* plBrushActions::GetDisplayName(plModelingCommand::Enum cmd)
{
  return c_Commands[cmd].m_szDisplayName;
}

plString plBrushActions::GetShortcutText(plModelingCommand::Enum cmd)
{
  if (const plActionDescriptor* pDesc = plActionManager::GetActionDescriptor(s_hCommands[cmd]))
  {
    return NormalizeShortcut(pDesc->m_sShortcut);
  }

  return plString();
}

bool plBrushActions::MatchShortcut(const plEditorInputEvent& e, plModelingCommand::Enum& out_cmd)
{
  // keypad digits arrive as plain digits
  plKeyChord pressed;
  pressed.m_Key = e.m_Key;
  pressed.m_uiCharacter = e.m_Key == plEditorPlatformKey::Letter ? e.m_uiCharacter : 0;
  pressed.m_uiModifiers = e.m_uiModifiers;

  if (!pressed.IsValid())
    return false;

  for (plUInt32 i = 0; i < plModelingCommand::Count; ++i)
  {
    const plActionDescriptor* pDesc = plActionManager::GetActionDescriptor(s_hCommands[i]);

    plKeyChord bound;
    if (pDesc == nullptr || pDesc->m_sShortcut.IsEmpty() || plKeyChord::Parse(pDesc->m_sShortcut, bound).Failed())
      continue;

    if (bound == pressed)
    {
      out_cmd = static_cast<plModelingCommand::Enum>(i);
      return true;
    }
  }

  return false;
}

plStatus plBrushActions::Execute(plDocument* pDocument, plModelingCommand::Enum cmd)
{
  plGameObjectDocument* pGameDoc = plDynamicCast<plGameObjectDocument*>(pDocument);

  if (pGameDoc == nullptr)
    return plStatus("Modeling commands work in scenes.");

  plStatus result(PL_SUCCESS);

  // whole-brush commands work on the selection, with or without the Brush tool
  plDynamicArray<const plDocumentObject*> brushes;
  plBrushCommands::GatherSelectedBrushes(pDocument, brushes);

  switch (cmd)
  {
    case plModelingCommand::ToggleCutter:
      result = plBrushCommands::ToggleCutter(pDocument, brushes);
      break;
    case plModelingCommand::BakeCuts:
      result = plBrushCommands::BakeCuts(pDocument, brushes);
      break;
    case plModelingCommand::ToggleGridSnap:
      plBrushToolSettings::SetSnapToGrid(!plBrushToolSettings::GetSnapToGrid());
      break;
    case plModelingCommand::Hollow:
      result = plBrushCommands::Hollow(pDocument, brushes);
      break;
    case plModelingCommand::Union:
      result = plBrushCommands::Union(pDocument, brushes);
      break;
    case plModelingCommand::Decompose:
      result = plBrushCommands::Decompose(pDocument, brushes);
      break;
    case plModelingCommand::Group:
    {
      plHybridArray<const plDocumentObject*, 16> selection;
      for (const plDocumentObject* pObject : pDocument->GetSelectionManager()->GetSelection())
        selection.PushBack(pObject);

      result = plBrushCommands::CreateGroup(pDocument, selection);
      break;
    }
    case plModelingCommand::PivotToCenter:
      result = plBrushCommands::PlacePivot(pDocument, brushes, plBrushCommands::PivotPlacement::Center);
      break;
    case plModelingCommand::PivotToBottom:
      result = plBrushCommands::PlacePivot(pDocument, brushes, plBrushCommands::PivotPlacement::Bottom);
      break;

    default:
    {
      plBrushToolContext* pContext = ActivateBrushTool(pGameDoc);

      if (pContext == nullptr)
        return plStatus("The Brush tool is not available.");

      switch (cmd)
      {
        case plModelingCommand::ModeObject:
          pContext->SetSubMode(plBrushSubMode::Object);
          break;
        case plModelingCommand::ModeVertex:
          pContext->SetSubMode(plBrushSubMode::Vertex);
          break;
        case plModelingCommand::ModeEdge:
          pContext->SetSubMode(plBrushSubMode::Edge);
          break;
        case plModelingCommand::ModeFace:
          pContext->SetSubMode(plBrushSubMode::Face);
          break;
        case plModelingCommand::MarkingMenu:
          pContext->OpenMarkingMenu();
          break;
        case plModelingCommand::Grab:
          result = pContext->RunModalTransform(false);
          break;
        case plModelingCommand::Scale:
          result = pContext->RunModalTransform(true);
          break;
        case plModelingCommand::Extrude:
          result = pContext->RunMeshOp(plBrushMeshOp::Extrude);
          break;
        case plModelingCommand::Inset:
          result = pContext->RunMeshOp(plBrushMeshOp::Inset);
          break;
        case plModelingCommand::Bevel:
          result = pContext->RunMeshOp(plBrushMeshOp::Bevel);
          break;
        case plModelingCommand::LoopCut:
          result = pContext->RunMeshOp(plBrushMeshOp::LoopCut);
          break;
        case plModelingCommand::Bridge:
          result = pContext->RunMeshOp(plBrushMeshOp::Bridge);
          break;
        case plModelingCommand::Weld:
          result = pContext->RunMeshOp(plBrushMeshOp::Weld);
          break;
        case plModelingCommand::Flip:
          result = pContext->RunMeshOp(plBrushMeshOp::Flip);
          break;
        case plModelingCommand::Subdivide:
          result = pContext->RunMeshOp(plBrushMeshOp::Subdivide);
          break;
        case plModelingCommand::Detach:
          result = pContext->RunMeshOp(plBrushMeshOp::Detach);
          break;
        case plModelingCommand::Delete:
          result = pContext->RunMeshOp(plBrushMeshOp::Delete);
          break;
        case plModelingCommand::FillHole:
          result = pContext->RunMeshOp(plBrushMeshOp::FillHole);
          break;
        case plModelingCommand::Connect:
          result = pContext->RunMeshOp(plBrushMeshOp::Connect);
          break;
        case plModelingCommand::Triangulate:
          result = pContext->RunMeshOp(plBrushMeshOp::Triangulate);
          break;
        case plModelingCommand::Flatten:
          result = pContext->RunMeshOp(plBrushMeshOp::Flatten);
          break;
        case plModelingCommand::SelectAll:
          pContext->ApplySelectionOp(plBrushSelectionOp::All);
          break;
        case plModelingCommand::SelectNone:
          pContext->ApplySelectionOp(plBrushSelectionOp::None);
          break;
        case plModelingCommand::SelectInvert:
          pContext->ApplySelectionOp(plBrushSelectionOp::Invert);
          break;
        case plModelingCommand::SelectGrow:
          pContext->ApplySelectionOp(plBrushSelectionOp::Grow);
          break;
        case plModelingCommand::SelectShrink:
          pContext->ApplySelectionOp(plBrushSelectionOp::Shrink);
          break;
        case plModelingCommand::SelectLoop:
          pContext->ApplySelectionOp(plBrushSelectionOp::Loop);
          break;
        case plModelingCommand::SelectRing:
          pContext->ApplySelectionOp(plBrushSelectionOp::Ring);
          break;
        case plModelingCommand::SelectSameMaterial:
          pContext->ApplySelectionOp(plBrushSelectionOp::SameMaterial);
          break;
        case plModelingCommand::ToggleSelectThrough:
          pContext->SetSelectThrough(!pContext->GetSelectThrough());
          break;
        case plModelingCommand::ToggleHiddenHandles:
          pContext->SetShowHiddenHandles(!pContext->GetShowHiddenHandles());
          break;
        case plModelingCommand::Clip:
          result = pContext->StartClip();
          break;
        case plModelingCommand::EditPivot:
          result = pContext->SetPivotEdit(!pContext->IsPivotEditActive());
          break;
        case plModelingCommand::PivotToSelection:
          result = pContext->MovePivotToSelection();
          break;
        case plModelingCommand::ToggleFaceGranularity:
          pContext->SetFaceGranularity(pContext->GetFaceGranularity() == plBrushFaceGranularity::Surface ? plBrushFaceGranularity::Polygon : plBrushFaceGranularity::Surface);
          break;
        default:
          break;
      }
      break;
    }
  }

  ShowStatus(pDocument, result);
  return result;
}

//////////////////////////////////////////////////////////////////////////

plModelingAction::plModelingAction(const plActionContext& context, const char* szName, plModelingCommand::Enum cmd)
  : plButtonAction(context, szName, false, "")
  , m_Command(cmd)
{
  // the marking menu is also a toolbar button
  if (cmd == plModelingCommand::MarkingMenu)
    SetIconPath(":/ModelingIcons/Line/MarkingMenu.svg");
}

void plModelingAction::Execute(const plVariant& value)
{
  PL_IGNORE_UNUSED(value);
  plBrushActions::Execute(GetContext().m_pDocument, m_Command);
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_BrushActions);
