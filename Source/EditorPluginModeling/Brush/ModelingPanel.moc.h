#pragma once

#include <EditorPluginModeling/EditorPluginModelingDLL.h>
#include <GuiFoundation/DockPanels/DocumentPanel.moc.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.moc.h>

class plDocument;
class plDocumentObject;
class plQtDocumentWindow;
class QButtonGroup;

struct plGameObjectEvent;
struct plBrushModeEvent;

/// \brief The Modeling dock panel: a primitive palette. Clicking a shape arms the Brush draw tool with
/// it — the next click in the viewport starts drawing that primitive. The checked button doubles as the
/// mode indicator and clears when the tool deactivates.
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
  void OnModeButtonClicked(int iMode);
  void OnToggleCutterClicked();
  void OnCarveClicked();
  void OnHollowClicked();

private:
  void GameObjectEventHandler(const plGameObjectEvent& e);
  void BrushModeEventHandler(const plBrushModeEvent& e);
  void UncheckAllButtons();
  void SetCheckedMode(int iMode); ///< -1 = none (tool inactive)
  void GatherSelectedBrushComponents(plDynamicArray<const plDocumentObject*>& out_components) const;

  plDocument* m_pDocument = nullptr;
  QButtonGroup* m_pShapeButtons = nullptr;
  QButtonGroup* m_pModeButtons = nullptr;
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
