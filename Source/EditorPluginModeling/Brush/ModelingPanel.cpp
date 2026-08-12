#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorPluginModeling/Brush/BrushEditTool.h>
#include <EditorPluginModeling/Brush/ModelingPanel.moc.h>
#include <ModelingPlugin/Modeling/BrushGenerators.h>

#include <QButtonGroup>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace
{
  struct ShapeEntry
  {
    const char* m_szLabel;
    plBrushShape::Enum m_Shape;
  };

  // the palette order mirrors the plan's mockup
  const ShapeEntry c_Shapes[] = {
    {"Box", plBrushShape::Box},
    {"Ramp X", plBrushShape::RampX},
    {"Ramp Y", plBrushShape::RampY},
    {"Column", plBrushShape::Column},
    {"Stairs X", plBrushShape::StairsX},
    {"Stairs Y", plBrushShape::StairsY},
    {"Arch X", plBrushShape::ArchX},
    {"Arch Y", plBrushShape::ArchY},
    {"Spiral", plBrushShape::SpiralStairs},
  };

  plHashSet<plQtDocumentWindow*> s_HandledWindows;
} // namespace

plQtModelingPanel::plQtModelingPanel(plQtDocumentWindow* pWindow, plDocument* pDocument)
  : plQtDocumentPanel(pWindow, pDocument)
  , m_pDocument(pDocument)
{
  setObjectName("ModelingPanel");
  setWindowTitle("Modeling");

  QWidget* pContent = new QWidget(this);
  QVBoxLayout* pLayout = new QVBoxLayout(pContent);
  pLayout->setContentsMargins(6, 6, 6, 6);
  pLayout->setSpacing(6);

  QLabel* pHeader = new QLabel("Primitive", pContent);
  pLayout->addWidget(pHeader);

  QGridLayout* pGrid = new QGridLayout();
  pGrid->setSpacing(3);

  m_pShapeButtons = new QButtonGroup(this);
  m_pShapeButtons->setExclusive(true);

  for (plUInt32 i = 0; i < PL_ARRAY_SIZE(c_Shapes); ++i)
  {
    QPushButton* pButton = new QPushButton(c_Shapes[i].m_szLabel, pContent);
    pButton->setCheckable(true);

    m_pShapeButtons->addButton(pButton, static_cast<int>(c_Shapes[i].m_Shape));
    pGrid->addWidget(pButton, i / 3, i % 3);
  }

  pLayout->addLayout(pGrid);

  QLabel* pHint = new QLabel("Click a shape, then drag in the viewport\nto draw it. Esc or a gizmo leaves draw\nmode. Shape edits: Properties panel.", pContent);
  pHint->setWordWrap(true);
  pHint->setStyleSheet("color: #8a8a8a; font-size: 10px;");
  pLayout->addWidget(pHint);

  pLayout->addStretch();

  setWidget(pContent);

  connect(m_pShapeButtons, &QButtonGroup::idClicked, this, &plQtModelingPanel::OnShapeButtonClicked);

  // the checked button is a mode indicator, so it has to clear when draw mode ends
  if (plGameObjectDocument* pGameDoc = plDynamicCast<plGameObjectDocument*>(m_pDocument))
  {
    pGameDoc->m_GameObjectEvents.AddEventHandler(plMakeDelegate(&plQtModelingPanel::GameObjectEventHandler, this));
  }
}

plQtModelingPanel::~plQtModelingPanel()
{
  if (plGameObjectDocument* pGameDoc = plDynamicCast<plGameObjectDocument*>(m_pDocument))
  {
    pGameDoc->m_GameObjectEvents.RemoveEventHandler(plMakeDelegate(&plQtModelingPanel::GameObjectEventHandler, this));
  }
}

void plQtModelingPanel::OnShapeButtonClicked(int iShape)
{
  plBrushToolSettings::s_iPendingShape = iShape;

  // arm the draw tool: the next click in the viewport starts drawing this primitive
  if (plGameObjectDocument* pGameDoc = plDynamicCast<plGameObjectDocument*>(m_pDocument))
  {
    pGameDoc->SetActiveEditTool(plGetStaticRTTI<plBrushEditTool>());
  }
}

void plQtModelingPanel::GameObjectEventHandler(const plGameObjectEvent& e)
{
  if (e.m_Type != plGameObjectEvent::Type::ActiveEditToolChanged)
    return;

  plGameObjectDocument* pGameDoc = plDynamicCast<plGameObjectDocument*>(m_pDocument);

  if (pGameDoc == nullptr)
    return;

  plGameObjectEditTool* pTool = pGameDoc->GetActiveEditTool();

  if (pTool == nullptr || !pTool->GetDynamicRTTI()->IsDerivedFrom<plBrushEditTool>())
  {
    UncheckAllButtons();
  }
}

void plQtModelingPanel::UncheckAllButtons()
{
  m_pShapeButtons->setExclusive(false);

  for (QAbstractButton* pButton : m_pShapeButtons->buttons())
  {
    pButton->setChecked(false);
  }

  m_pShapeButtons->setExclusive(true);
}

//////////////////////////////////////////////////////////////////////////

void plModelingPanelManager::Startup()
{
  plQtDocumentWindow::s_Events.AddEventHandler(plMakeDelegate(&plModelingPanelManager::DocumentWindowEventHandler));
}

void plModelingPanelManager::Shutdown()
{
  plQtDocumentWindow::s_Events.RemoveEventHandler(plMakeDelegate(&plModelingPanelManager::DocumentWindowEventHandler));
  s_HandledWindows.Clear();
}

void plModelingPanelManager::DocumentWindowEventHandler(const plQtDocumentWindowEvent& e)
{
  if (e.m_Type == plQtDocumentWindowEvent::WindowClosed)
  {
    s_HandledWindows.Remove(e.m_pWindow);
    return;
  }

  // there is no window-created event, so the first BeforeRedraw is the earliest safe moment to dock:
  // the window is fully constructed and its own layout has been restored
  if (e.m_Type != plQtDocumentWindowEvent::BeforeRedraw)
    return;

  if (s_HandledWindows.Contains(e.m_pWindow))
    return;

  s_HandledWindows.Insert(e.m_pWindow);

  plDocument* pDocument = e.m_pWindow->GetDocument();

  if (pDocument == nullptr || e.m_pWindow->m_pDockManager == nullptr)
    return;

  // scene documents only — walk the type chain by name, the scene types live in a plugin this
  // package does not link
  bool bIsScene = false;

  for (const plRTTI* pRtti = pDocument->GetDynamicRTTI(); pRtti != nullptr; pRtti = pRtti->GetParentType())
  {
    if (pRtti->GetTypeName() == "plSceneDocument")
    {
      bIsScene = true;
      break;
    }
  }

  if (!bIsScene)
    return;

  plQtModelingPanel* pPanel = new plQtModelingPanel(e.m_pWindow, pDocument);
  e.m_pWindow->m_pDockManager->addDockWidgetTab(ads::RightDockWidgetArea, pPanel);
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_ModelingPanel);
