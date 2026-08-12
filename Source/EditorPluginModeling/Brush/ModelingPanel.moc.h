#pragma once

#include <EditorPluginModeling/EditorPluginModelingDLL.h>
#include <GuiFoundation/DockPanels/DocumentPanel.moc.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.moc.h>

class plDocument;
class plQtDocumentWindow;
class QButtonGroup;

/// \brief The Modeling dock panel: a primitive palette that presets the shape for the next drawn brush
/// and re-shapes the currently selected ones.
///
/// Docks as a tab beside Properties in scene windows. Created by the panel manager on a window's first
/// BeforeRedraw — plQtDocumentWindow has no window-created event, and the scene window builds its own
/// docks in its constructor where a package cannot reach.
class PL_EDITORPLUGINMODELING_DLL plQtModelingPanel : public plQtDocumentPanel
{
  Q_OBJECT

public:
  plQtModelingPanel(plQtDocumentWindow* pWindow, plDocument* pDocument);
  ~plQtModelingPanel();

private Q_SLOTS:
  void OnShapeButtonClicked(int iShape);

private:
  void ApplyShapeToSelection(int iShape);

  plDocument* m_pDocument = nullptr;
  QButtonGroup* m_pShapeButtons = nullptr;
};

/// \brief Watches document windows and docks a Modeling panel into every scene window.
class PL_EDITORPLUGINMODELING_DLL plModelingPanelManager
{
public:
  static void Startup();
  static void Shutdown();

private:
  static void DocumentWindowEventHandler(const plQtDocumentWindowEvent& e);
};
