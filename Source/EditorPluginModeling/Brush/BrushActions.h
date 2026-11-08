#pragma once

#include <EditorPluginModeling/EditorPluginModelingDLL.h>
#include <Foundation/Types/Status.h>
#include <GuiFoundation/Action/BaseActions.h>

class plDocument;
struct plEditorInputEvent;

/// \brief Every modeling command that has a key. They are registered as editor actions, so the shortcut
/// editor lists them under "Modeling" and remaps them, but they are deliberately not placed in any menu:
/// their default keys (1, 2, 3, G, S, Tab...) clash with editor-wide shortcuts and only apply while the
/// Brush tool is active, where the tool claims them itself.
struct plModelingCommand
{
  enum Enum
  {
    ModeObject,
    ModeVertex,
    ModeEdge,
    ModeFace,
    MarkingMenu,
    Grab,
    Scale,
    Extrude,
    Inset,
    Bevel,
    LoopCut,
    Bridge,
    Weld,
    Flip,
    Subdivide,
    Detach,
    Delete,
    FillHole,
    Connect,
    Triangulate,
    Flatten,
    SelectAll,
    SelectNone,
    SelectInvert,
    SelectGrow,
    SelectShrink,
    SelectLoop,
    SelectRing,
    SelectSameMaterial,
    ToggleSelectThrough,
    ToggleHiddenHandles,
    ToggleGridSnap,
    Clip,
    ToggleCutter,
    BakeCuts,
    Hollow,
    Union,
    Decompose,
    Group,
    EditPivot,
    PivotToCenter,
    PivotToBottom,
    PivotToSelection,
    ToggleFaceGranularity,

    Count
  };
};

class PL_EDITORPLUGINMODELING_DLL plBrushActions
{
public:
  static void RegisterActions();
  static void UnregisterActions();
  static void MapActions();

  static plActionDescriptorHandle s_hBrushTool;

  /// \brief The user-facing name of a command, e.g. "Loop Cut".
  static const char* GetDisplayName(plModelingCommand::Enum cmd);

  /// \brief The command's current key as the user sees it ("Shift+E"), empty if unbound.
  static plString GetShortcutText(plModelingCommand::Enum cmd);

  /// \brief Whether the key event is the current shortcut of a command.
  static bool MatchShortcut(const plEditorInputEvent& e, plModelingCommand::Enum& out_cmd);

  /// \brief Runs a command on a scene document. Sub-object commands switch to the Brush tool first.
  /// Failures are shown in the status bar and returned.
  static plStatus Execute(plDocument* pDocument, plModelingCommand::Enum cmd);

private:
  static plActionDescriptorHandle s_hCommands[plModelingCommand::Count];

  /// The Marking Menu command as a scene toolbar button, without a key of its own.
  static plActionDescriptorHandle s_hMarkingMenuButton;
};

/// \brief The action type behind every plModelingCommand. Triggered from the shortcut editor's list
/// only in theory: the commands are reached through keys, the panel and the marking menu.
class PL_EDITORPLUGINMODELING_DLL plModelingAction : public plButtonAction
{
  PL_ADD_DYNAMIC_REFLECTION(plModelingAction, plButtonAction);

public:
  plModelingAction(const plActionContext& context, const char* szName, plModelingCommand::Enum cmd);

  virtual void Execute(const plVariant& value) override;

private:
  plModelingCommand::Enum m_Command;
};
