#pragma once

#include <EditorPluginModeling/Brush/BrushToolContext.h>
#include <EditorPluginModeling/EditorPluginModelingDLL.h>

#include <QWidget>

class QButtonGroup;

/// \brief The floating mode strip in the viewport's top-left corner: Object / Vertex / Edge / Face
/// chips with their hotkeys, the active mode highlighted — the plan mockup's mode indicator.
///
/// One strip is parented to each engine view widget while the Brush tool is active. It is an ordinary
/// child widget, so it takes clicks itself and steals nothing from the viewport around it. Mode state
/// syncs through plBrushToolContext::s_ModeEvents, and a refused switch (no brush selected) snaps the
/// chips back to the mode that actually resulted.
class PL_EDITORPLUGINMODELING_DLL plQtBrushModeStrip : public QWidget
{
  Q_OBJECT

public:
  plQtBrushModeStrip(QWidget* pParentView, plBrushToolContext* pContext);
  ~plQtBrushModeStrip();

  void SyncFromContext();

private Q_SLOTS:
  void OnChipClicked(int iMode);

private:
  void BrushModeEventHandler(const plBrushModeEvent& e);
  void SetCheckedMode(int iMode);

  plBrushToolContext* m_pContext = nullptr;
  QButtonGroup* m_pChips = nullptr;
};
