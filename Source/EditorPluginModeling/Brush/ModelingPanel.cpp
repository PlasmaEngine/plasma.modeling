#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorPluginModeling/Brush/BrushEditTool.h>
#include <EditorPluginModeling/Brush/BrushPicking.h>
#include <EditorPluginModeling/Brush/ModelingPanel.moc.h>
#include <ModelingPlugin/Modeling/BrushGenerators.h>
#include <ModelingPlugin/Modeling/MeshCSG.h>
#include <ToolsFoundation/Command/TreeCommands.h>

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

  // ---- Mode -----------------------------------------------------------------
  QLabel* pModeHeader = new QLabel("Mode", pContent);
  pLayout->addWidget(pModeHeader);

  QGridLayout* pModeGrid = new QGridLayout();
  pModeGrid->setSpacing(3);

  m_pModeButtons = new QButtonGroup(this);
  m_pModeButtons->setExclusive(true);

  const char* szModeLabels[] = {"Object", "Vertex", "Edge", "Face"};
  const char* szModeHints[] = {"Draw and place brushes (Esc)", "Edit vertices (1)", "Edit edges (2)", "Edit faces (3)"};

  for (int i = 0; i < 4; ++i)
  {
    QPushButton* pButton = new QPushButton(szModeLabels[i], pContent);
    pButton->setCheckable(true);
    pButton->setToolTip(szModeHints[i]);

    m_pModeButtons->addButton(pButton, i);
    pModeGrid->addWidget(pButton, i / 2, i % 2);
  }

  pLayout->addLayout(pModeGrid);

  connect(m_pModeButtons, &QButtonGroup::idClicked, this, &plQtModelingPanel::OnModeButtonClicked);

  // ---- Primitive ------------------------------------------------------------
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

  // ---- CSG ------------------------------------------------------------------
  QLabel* pCsgHeader = new QLabel("CSG", pContent);
  pLayout->addWidget(pCsgHeader);

  QGridLayout* pCsgGrid = new QGridLayout();
  pCsgGrid->setSpacing(3);

  QPushButton* pToggleCutter = new QPushButton("Toggle Cutter", pContent);
  pToggleCutter->setToolTip("Make the selected brushes subtractive (or solid again). A cutter carves every brush it overlaps, live.");
  pCsgGrid->addWidget(pToggleCutter, 0, 0);

  QPushButton* pCarve = new QPushButton("Carve", pContent);
  pCarve->setToolTip("Bake the selected cutter into the brushes it overlaps, then delete the cutter.");
  pCsgGrid->addWidget(pCarve, 0, 1);

  QPushButton* pHollow = new QPushButton("Hollow", pContent);
  pHollow->setToolTip("Turn the selected brushes into rooms with walls of their Thickness property.");
  pCsgGrid->addWidget(pHollow, 1, 0);

  pLayout->addLayout(pCsgGrid);

  QLabel* pCsgHint = new QLabel("Cutters must be convex. Targets can be\nany shape.", pContent);
  pCsgHint->setWordWrap(true);
  pCsgHint->setStyleSheet("color: #8a8a8a; font-size: 10px;");
  pLayout->addWidget(pCsgHint);

  connect(pToggleCutter, &QPushButton::clicked, this, &plQtModelingPanel::OnToggleCutterClicked);
  connect(pCarve, &QPushButton::clicked, this, &plQtModelingPanel::OnCarveClicked);
  connect(pHollow, &QPushButton::clicked, this, &plQtModelingPanel::OnHollowClicked);

  pLayout->addStretch();

  setWidget(pContent);

  connect(m_pShapeButtons, &QButtonGroup::idClicked, this, &plQtModelingPanel::OnShapeButtonClicked);

  // the checked buttons are mode indicators, so they have to follow tool and keyboard state
  if (plGameObjectDocument* pGameDoc = plDynamicCast<plGameObjectDocument*>(m_pDocument))
  {
    pGameDoc->m_GameObjectEvents.AddEventHandler(plMakeDelegate(&plQtModelingPanel::GameObjectEventHandler, this));
  }

  plBrushToolContext::s_ModeEvents.AddEventHandler(plMakeDelegate(&plQtModelingPanel::BrushModeEventHandler, this));

  SetCheckedMode(-1);
}

plQtModelingPanel::~plQtModelingPanel()
{
  plBrushToolContext::s_ModeEvents.RemoveEventHandler(plMakeDelegate(&plQtModelingPanel::BrushModeEventHandler, this));

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

    // drawing happens in Object mode
    if (plBrushEditTool* pTool = plDynamicCast<plBrushEditTool*>(pGameDoc->GetActiveEditTool()))
    {
      pTool->GetToolContext().SetSubMode(plBrushSubMode::Object);
      SetCheckedMode(plBrushSubMode::Object);
    }
  }
}

void plQtModelingPanel::OnModeButtonClicked(int iMode)
{
  plGameObjectDocument* pGameDoc = plDynamicCast<plGameObjectDocument*>(m_pDocument);

  if (pGameDoc == nullptr)
    return;

  pGameDoc->SetActiveEditTool(plGetStaticRTTI<plBrushEditTool>());

  plBrushEditTool* pTool = plDynamicCast<plBrushEditTool*>(pGameDoc->GetActiveEditTool());

  if (pTool == nullptr)
  {
    SetCheckedMode(-1);
    return;
  }

  pTool->GetToolContext().SetSubMode(static_cast<plBrushSubMode::Enum>(iMode));

  // entering a sub-object mode can be refused (no brush selected) — show the mode that resulted
  SetCheckedMode(static_cast<int>(pTool->GetToolContext().GetSubMode()));
}

void plQtModelingPanel::BrushModeEventHandler(const plBrushModeEvent& e)
{
  if (e.m_pDocument != m_pDocument)
    return;

  SetCheckedMode(static_cast<int>(e.m_Mode));
}

void plQtModelingPanel::GameObjectEventHandler(const plGameObjectEvent& e)
{
  if (e.m_Type != plGameObjectEvent::Type::ActiveEditToolChanged)
    return;

  plGameObjectDocument* pGameDoc = plDynamicCast<plGameObjectDocument*>(m_pDocument);

  if (pGameDoc == nullptr)
    return;

  plGameObjectEditTool* pTool = pGameDoc->GetActiveEditTool();

  if (plBrushEditTool* pBrushTool = plDynamicCast<plBrushEditTool*>(pTool))
  {
    SetCheckedMode(static_cast<int>(pBrushTool->GetToolContext().GetSubMode()));
  }
  else
  {
    UncheckAllButtons();
    SetCheckedMode(-1);
  }
}

void plQtModelingPanel::SetCheckedMode(int iMode)
{
  m_pModeButtons->setExclusive(false);

  for (QAbstractButton* pButton : m_pModeButtons->buttons())
  {
    pButton->setChecked(m_pModeButtons->id(pButton) == iMode);
  }

  m_pModeButtons->setExclusive(true);
}

//////////////////////////////////////////////////////////////////////////
// CSG

void plQtModelingPanel::GatherSelectedBrushComponents(plDynamicArray<const plDocumentObject*>& out_components) const
{
  out_components.Clear();

  const plGameObjectDocument* pGameDoc = plDynamicCast<const plGameObjectDocument*>(m_pDocument);

  if (pGameDoc == nullptr)
    return;

  for (const plDocumentObject* pObject : pGameDoc->GetSelectionManager()->GetSelection())
  {
    for (const plDocumentObject* pChild : pObject->GetChildren())
    {
      if (pChild->GetTypeAccessor().GetType()->GetTypeName() == "plBrushComponent")
      {
        out_components.PushBack(pChild);
      }
    }
  }
}

void plQtModelingPanel::OnToggleCutterClicked()
{
  plDynamicArray<const plDocumentObject*> components;
  GatherSelectedBrushComponents(components);

  if (components.IsEmpty())
    return;

  auto* pHistory = m_pDocument->GetCommandHistory();
  pHistory->StartTransaction("Toggle Cutter");

  for (const plDocumentObject* pComponent : components)
  {
    plSetObjectPropertyCommand cmd;
    cmd.m_Object = pComponent->GetGuid();
    cmd.m_sProperty = "Subtractive";
    cmd.m_NewValue = !pComponent->GetTypeAccessor().GetValue("Subtractive").ConvertTo<bool>();
    pHistory->AddCommand(cmd).AssertSuccess();
  }

  pHistory->FinishTransaction();
}

void plQtModelingPanel::OnHollowClicked()
{
  plDynamicArray<const plDocumentObject*> components;
  GatherSelectedBrushComponents(components);

  if (components.IsEmpty())
    return;

  auto* pHistory = m_pDocument->GetCommandHistory();
  bool bAny = false;

  plEditableMesh mesh, hollowed;

  for (const plDocumentObject* pComponent : components)
  {
    if (plBrushPicking::BuildMeshFromObject(pComponent, mesh).Failed())
      continue;

    const float fThickness = pComponent->GetTypeAccessor().GetValue("Thickness").ConvertTo<float>();

    if (plMeshCSG::MakeHollow(mesh, fThickness, 0, hollowed).Failed())
    {
      plLog::Warning("Hollow: the brush is not convex or too small for a {0} thick wall.", fThickness);
      continue;
    }

    if (!bAny)
    {
      pHistory->StartTransaction("Hollow Brush");
      bAny = true;
    }

    plDataBuffer blob;
    hollowed.ToDataBuffer(blob);

    {
      plSetObjectPropertyCommand cmd;
      cmd.m_Object = pComponent->GetGuid();
      cmd.m_sProperty = "Shape";
      cmd.m_NewValue = static_cast<plInt64>(plBrushShape::Editable);
      pHistory->AddCommand(cmd).AssertSuccess();
    }
    {
      plSetObjectPropertyCommand cmd;
      cmd.m_Object = pComponent->GetGuid();
      cmd.m_sProperty = "StoredMesh";
      cmd.m_NewValue = blob;
      pHistory->AddCommand(cmd).AssertSuccess();
    }
  }

  if (bAny)
  {
    pHistory->FinishTransaction();
  }
}

void plQtModelingPanel::OnCarveClicked()
{
  plDynamicArray<const plDocumentObject*> components;
  GatherSelectedBrushComponents(components);

  // the selected cutters get baked into everything they overlap, then removed
  plHybridArray<const plDocumentObject*, 4> cutters;

  for (const plDocumentObject* pComponent : components)
  {
    if (pComponent->GetTypeAccessor().GetValue("Subtractive").ConvertTo<bool>())
    {
      cutters.PushBack(pComponent);
    }
  }

  if (cutters.IsEmpty())
  {
    plLog::Warning("Carve: select a subtractive brush (Toggle Cutter) first.");
    return;
  }

  auto* pHistory = m_pDocument->GetCommandHistory();
  pHistory->StartTransaction("Carve");

  plEditableMesh cutterMesh, targetMesh, result;

  for (const plDocumentObject* pCutter : cutters)
  {
    if (plBrushPicking::BuildMeshFromObject(pCutter, cutterMesh).Failed() || cutterMesh.m_Faces.IsEmpty())
      continue;

    const plTransform cutterTransform = plBrushPicking::GetComponentGlobalTransform(pCutter);

    plBoundingBox cutterWorldBounds = cutterMesh.ComputeBounds();
    cutterWorldBounds.TransformFromOrigin(cutterTransform.GetAsMat4());

    // every additive brush in the document is a potential target
    plDynamicArray<const plDocumentObject*> targets;
    {
      plDynamicArray<const plDocumentObject*> stack;
      stack.PushBack(pCutter->GetDocumentObjectManager()->GetRootObject());

      while (!stack.IsEmpty())
      {
        const plDocumentObject* pCurrent = stack.PeekBack();
        stack.PopBack();

        for (const plDocumentObject* pChild : pCurrent->GetChildren())
        {
          if (pChild != pCutter && pChild->GetTypeAccessor().GetType()->GetTypeName() == "plBrushComponent" &&
              !pChild->GetTypeAccessor().GetValue("Subtractive").ConvertTo<bool>())
          {
            targets.PushBack(pChild);
          }

          stack.PushBack(pChild);
        }
      }
    }

    for (const plDocumentObject* pTarget : targets)
    {
      if (plBrushPicking::BuildMeshFromObject(pTarget, targetMesh).Failed() || targetMesh.m_Faces.IsEmpty())
        continue;

      const plTransform targetTransform = plBrushPicking::GetComponentGlobalTransform(pTarget);

      plBoundingBox targetWorldBounds = targetMesh.ComputeBounds();
      targetWorldBounds.TransformFromOrigin(targetTransform.GetAsMat4());

      if (!targetWorldBounds.Overlaps(cutterWorldBounds))
        continue;

      const plTransform cutterToTarget = plTransform::MakeLocalTransform(targetTransform, cutterTransform);

      if (plMeshCSG::SubtractCutter(targetMesh, cutterMesh, cutterToTarget, 0, result).Failed())
      {
        plLog::Warning("Carve: the cutter is not convex - decompose it into convex shells first.");
        continue;
      }

      // only brushes the cutter actually intersects get converted
      plDataBuffer before, after;
      targetMesh.ToDataBuffer(before);
      result.ToDataBuffer(after);

      if (before == after)
        continue;

      {
        plSetObjectPropertyCommand cmd;
        cmd.m_Object = pTarget->GetGuid();
        cmd.m_sProperty = "Shape";
        cmd.m_NewValue = static_cast<plInt64>(plBrushShape::Editable);
        pHistory->AddCommand(cmd).AssertSuccess();
      }
      {
        plSetObjectPropertyCommand cmd;
        cmd.m_Object = pTarget->GetGuid();
        cmd.m_sProperty = "StoredMesh";
        cmd.m_NewValue = after;
        pHistory->AddCommand(cmd).AssertSuccess();
      }
    }

    // the cutter's job is done — remove its whole object
    {
      plRemoveObjectCommand cmd;
      cmd.m_Object = pCutter->GetParent()->GetGuid();
      pHistory->AddCommand(cmd).AssertSuccess();
    }
  }

  pHistory->FinishTransaction();
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
