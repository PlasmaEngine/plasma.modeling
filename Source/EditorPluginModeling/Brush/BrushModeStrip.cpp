#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorPluginModeling/Brush/BrushModeStrip.moc.h>

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QPushButton>

plQtBrushModeStrip::plQtBrushModeStrip(QWidget* pParentView, plBrushToolContext* pContext)
  : QWidget(pParentView)
  , m_pContext(pContext)
{
  setObjectName("BrushModeStrip");
  setAttribute(Qt::WA_StyledBackground, true);

  setStyleSheet(
    "QWidget#BrushModeStrip { background: rgba(20, 20, 20, 219); border: 1px solid #333333; border-radius: 6px; }"
    "QPushButton { background: transparent; color: #b0b0b0; border: none; border-radius: 4px; padding: 4px 11px; font-size: 11px; }"
    "QPushButton:hover { color: #ffffff; background: #2a2a2a; }"
    "QPushButton:checked { background: #2d7f69; color: #ffffff; }");

  QHBoxLayout* pLayout = new QHBoxLayout(this);
  pLayout->setContentsMargins(4, 4, 4, 4);
  pLayout->setSpacing(2);

  m_pChips = new QButtonGroup(this);
  m_pChips->setExclusive(true);

  const char* szLabels[] = {"Object", "Vertex  1", "Edge  2", "Face  3"};
  const char* szHints[] = {"Draw and place brushes (Esc)", "Edit vertices (1)", "Edit edges (2)", "Edit faces (3)"};

  for (int i = 0; i < 4; ++i)
  {
    QPushButton* pChip = new QPushButton(szLabels[i], this);
    pChip->setCheckable(true);
    pChip->setToolTip(szHints[i]);
    pChip->setFocusPolicy(Qt::NoFocus); // keyboard stays with the viewport

    m_pChips->addButton(pChip, i);
    pLayout->addWidget(pChip);
  }

  connect(m_pChips, &QButtonGroup::idClicked, this, &plQtBrushModeStrip::OnChipClicked);

  plBrushToolContext::s_ModeEvents.AddEventHandler(plMakeDelegate(&plQtBrushModeStrip::BrushModeEventHandler, this));

  move(10, 10);
  SyncFromContext();
  show();
  raise();
}

plQtBrushModeStrip::~plQtBrushModeStrip()
{
  plBrushToolContext::s_ModeEvents.RemoveEventHandler(plMakeDelegate(&plQtBrushModeStrip::BrushModeEventHandler, this));
}

void plQtBrushModeStrip::SyncFromContext()
{
  SetCheckedMode(static_cast<int>(m_pContext->GetSubMode()));
  adjustSize();
}

void plQtBrushModeStrip::OnChipClicked(int iMode)
{
  m_pContext->SetSubMode(static_cast<plBrushSubMode::Enum>(iMode));

  // a refused switch (no brush selected) must not leave a lying chip
  SetCheckedMode(static_cast<int>(m_pContext->GetSubMode()));
}

void plQtBrushModeStrip::BrushModeEventHandler(const plBrushModeEvent& e)
{
  if (e.m_pContext != m_pContext)
    return;

  SetCheckedMode(static_cast<int>(e.m_Mode));
}

void plQtBrushModeStrip::SetCheckedMode(int iMode)
{
  if (QAbstractButton* pButton = m_pChips->button(iMode))
  {
    pButton->setChecked(true);
  }
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_BrushModeStrip);
