#pragma once

#include <EditorPluginModeling/Brush/BrushActions.h>
#include <EditorPluginModeling/Brush/BrushToolContext.h>
#include <EditorPluginModeling/EditorPluginModelingDLL.h>
#include <EditorUi/Runtime.h>

class plDocument;
class plEditorUiContent;
class plEditorUiEngineViewPanel;
class plEngineViewWidget;

/// \brief The marking menu, drawn by the engine UI as wedges around the cursor (W5-D11): Face Mode, Extrude, Object
/// Mode, Clip, Edge Mode, Bevel, Vertex Mode and Loop Cut, clockwise from the top, each with its current key; the
/// current mode has the accent edge. It shows in a popup window of the engine shell, shaped to its wedges and labels, so
/// the view shows between them.
///
/// Moving toward an item highlights it. A press picks the highlighted item, and so does releasing the key that opened
/// the menu after moving toward an item; a press near the centre, Esc or the menu key again close it.
namespace plModelingEngineUi
{
  /// \brief Opens the marking menu at the cursor. Picking an item runs its command on the document. False when there
  /// is no engine window.
  PL_EDITORPLUGINMODELING_DLL bool ShowShellMarkingMenu(plDocument* pDocument, plBrushSubMode::Enum currentMode);

  /// Whether the marking menu is open.
  PL_EDITORPLUGINMODELING_DLL bool IsShellMarkingMenuOpen();

  /// The items of the marking menu.
  constexpr plUInt32 MarkingMenuItemCount = 8;
} // namespace plModelingEngineUi

/// \brief The mode strip in a viewport's top-left corner, drawn by the engine UI as an overlay of the view: Object,
/// Vertex, Edge and Face chips with their keys as key caps, CSG / Mesh chips in Face mode, and an info line with live
/// measurements under them (box size while drawing, edge length, operation amounts).
///
/// One strip follows each engine view while the Brush tool is active: a child content of the view's panel in the engine
/// shell, set as the panel's overlay over the view slot and covering only its two plates, so the view shows around
/// them. Chips act on the click and take no keyboard focus, so the keys stay with the view.
class PL_EDITORPLUGINMODELING_DLL plEngineBrushModeStrip
{
  PL_DISALLOW_COPY_AND_ASSIGN(plEngineBrushModeStrip);

public:
  plEngineBrushModeStrip(plEngineViewWidget* pView, plBrushToolContext* pContext);
  ~plEngineBrushModeStrip();

  /// Does what clicking a chip does: "Strip.mode.<n>" (plBrushSubMode) or "Strip.faces.<n>" (plBrushFaceGranularity).
  void ActivateChip(plStringView sChip);

private:
  void BrushModeEventHandler(const plBrushModeEvent& e);
  void ViewDestroyingHandler(plEngineViewWidget& ref_view);
  void Poll();

  /// Shows the strip in the panel that shows the view now, when that is another panel than before; false without one.
  bool FollowPanel();
  void Detach();
  void Rebuild();

  plEngineViewWidget* m_pView = nullptr;
  plBrushToolContext* m_pContext = nullptr;
  plEditorUiEngineViewPanel* m_pPanel = nullptr; ///< the view's panel the strip shows in
  plEditorUiContent* m_pContent = nullptr;       ///< a child of m_pPanel
  plEventSubscriptionID m_ViewSubscription = 0;
  plEditorUiTimerGroup m_Timers; ///< the poll every 100 ms and deferred chip clicks
  plString m_sInfo;
};
