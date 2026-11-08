#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorFramework/DocumentWindow/EngineViewWidget.h>
#include <EditorFramework/EngineShell/EngineViewPanel.h>
#include <EditorPluginModeling/Brush/EngineBrushOverlays.h>
#include <EditorUi/Content.h>
#include <EditorUi/Drawings.h>
#include <EditorUi/KeyChord.h>
#include <EditorUi/PopupMenu.h>
#include <EditorUi/Runtime.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>
#include <EditorUi/WindowHost.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GameUiPlugin/Components/UiMessages.h>
#include <GameUiPlugin/Elements/UiDrawList.h>
#include <GameUiPlugin/Elements/UiElements.h>

namespace
{
  using plModelingEngineUi::MarkingMenuItemCount;

  /// The strip's place over the view slot, from its top-left.
  constexpr float s_fStripMargin = 10.0f;

  // mode strip
  constexpr float s_fPillPadding = 3.0f; ///< 1 px border and 2 px padding
  constexpr float s_fChipHeight = 24.0f;
  constexpr float s_fPillHeight = s_fChipHeight + 2 * s_fPillPadding;
  constexpr float s_fChipGap = 2.0f;
  constexpr float s_fKeyHeight = 16.0f;
  constexpr float s_fInfoGap = 4.0f;
  constexpr float s_fInfoHeight = 20.0f;
  constexpr plUInt32 s_uiChipFontSize = plEditorUi::FontSize - 1;
  constexpr plUInt32 s_uiKeyFontSize = plEditorUi::MinFontSize;
  constexpr plUInt32 s_uiInfoFontSize = plEditorUi::FontSize - 2;

  // marking menu
  constexpr float s_fMenuWidth = 420.0f;
  constexpr float s_fMenuHeight = 280.0f;

  /// clockwise from the top
  const plModelingCommand::Enum c_MenuEntries[MarkingMenuItemCount] = {
    plModelingCommand::ModeFace,
    plModelingCommand::Extrude,
    plModelingCommand::ModeObject,
    plModelingCommand::Clip,
    plModelingCommand::ModeEdge,
    plModelingCommand::Bevel,
    plModelingCommand::ModeVertex,
    plModelingCommand::LoopCut,
  };

  void Place(plUiElement* pElement, float x, float y, float fWidth, float fHeight)
  {
    pElement->m_vAnchorMin = pElement->m_vAnchorMax = plVec2::MakeZero();
    pElement->m_vOffsets = plVec4(x, y, fWidth, fHeight);
  }

  plVec2 GetMenuCenter()
  {
    return plVec2(s_fMenuWidth * 0.5f, s_fMenuHeight * 0.5f);
  }

  bool IsCurrentMode(plModelingCommand::Enum cmd, plBrushSubMode::Enum currentMode)
  {
    return (cmd == plModelingCommand::ModeObject && currentMode == plBrushSubMode::Object) || (cmd == plModelingCommand::ModeVertex && currentMode == plBrushSubMode::Vertex) ||
           (cmd == plModelingCommand::ModeEdge && currentMode == plBrushSubMode::Edge) || (cmd == plModelingCommand::ModeFace && currentMode == plBrushSubMode::Face);
  }

  /// An item's label plate in logical units of the menu, as plEditorUiDraw::MarkingMenu places it, with its shadow.
  plRectFloat GetPlateRect(const plVec2& vCenter, plUInt32 uiItem, plStringView sLabel, plStringView sShortcut)
  {
    constexpr float fPlateHeight = 22.0f;
    const float fStep = 2.0f * plMath::Pi<float>() / MarkingMenuItemCount;
    const float fLabel = plMath::Ceil(plEditorUi::MeasureText(sLabel, plEditorUi::FontSize - 1));
    const float fKey = sShortcut.IsEmpty() ? 0.0f : plMath::Ceil(plEditorUi::MeasureCodeText(sShortcut, plEditorUi::MinFontSize)) + 8.0f;
    const float fWidth = 12.0f + fLabel + (fKey > 0.0f ? 6.0f + fKey : 0.0f);
    const float fAngle = -plMath::Pi<float>() * 0.5f + uiItem * fStep;
    const float fX = vCenter.x + plMath::Cos(plAngle::MakeFromRadian(fAngle)) * 128.0f;
    const float fY = vCenter.y + plMath::Sin(plAngle::MakeFromRadian(fAngle)) * 128.0f * 0.86f;
    return plRectFloat(plMath::Round(fX - fWidth * 0.5f) - 2.0f, plMath::Round(fY - fPlateHeight * 0.5f), fWidth + 4.0f, fPlateHeight + 4.0f);
  }

  /// One chip of the strip: its label, the key shown as a key cap and the tooltip.
  struct Chip
  {
    plString m_sName;
    plString m_sLabel;
    plString m_sKey;
    plString m_sTooltip;
    bool m_bActive = false;
    bool m_bSeparatorBefore = false;
  };

  float KeyWidth(plStringView sKey)
  {
    return plMath::Ceil(plEditorUi::MeasureCodeText(sKey, s_uiKeyFontSize)) + 10.0f;
  }

  float ChipWidth(const Chip& chip)
  {
    const float fLabel = plMath::Ceil(plEditorUi::MeasureText(chip.m_sLabel, s_uiChipFontSize));
    return 8.0f + fLabel + (chip.m_sKey.IsEmpty() ? 0.0f : 6.0f + KeyWidth(chip.m_sKey)) + 8.0f;
  }

  /// A compact key cap inside a chip: a plate with a 2 px bottom edge and the key in the code face.
  void BuildKeyCap(plUiElement* pParent, plStringView sKey, float x, float y)
  {
    using namespace plEditorUi;

    const float fWidth = KeyWidth(sKey);

    plUiPanel* pEdge = Panel(pParent);
    Place(pEdge, x, y, fWidth, s_fKeyHeight);
    pEdge->m_Color = Border();
    pEdge->m_vCornerRadius = plVec4(3);

    plUiPanel* pFace = Panel(pEdge);
    Place(pFace, 1.0f, 1.0f, fWidth - 2.0f, s_fKeyHeight - 3.0f);
    pFace->m_Color = plEditorUi::Surface();
    pFace->m_vCornerRadius = plVec4(2);

    plUiText* pText = Text(pEdge, sKey);
    Place(pText, 0.0f, 0.0f, fWidth, s_fKeyHeight - 1.0f);
    pText->m_hFont = GetCodeFont();
    pText->m_uiFontSize = s_uiKeyFontSize;
    pText->m_Color = Dim();
    pText->m_HAlign = plUiTextHAlign::Center;
  }

  /// A chip: a borderless button; the active one sits on the selection fill with a 2 px accent bottom edge.
  void BuildChip(plUiElement* pParent, const Chip& chip, float x, float fWidth, plBlackboard& ref_board)
  {
    using namespace plEditorUi;

    plUiButton* pButton = Button(pParent, {}, chip.m_sName);
    pButton->ClearChildren();
    Place(pButton, x, s_fPillPadding, fWidth, s_fChipHeight);
    pButton->m_Color = chip.m_bActive ? Selection() : plColorGammaUB(0, 0, 0, 0);
    pButton->m_HoverColor = chip.m_bActive ? Selection() : Raised();
    pButton->m_PressedColor = pButton->m_HoverColor;
    pButton->m_vCornerRadius = plVec4(4);
    pButton->m_hStyle = GetGhostStyle();

    if (chip.m_bActive)
    {
      plUiPanel* pEdge = Panel(pButton);
      Place(pEdge, 2.0f, s_fChipHeight - 2.0f, fWidth - 4.0f, 2.0f);
      pEdge->m_Color = Accent();
    }

    const float fLabel = plMath::Ceil(MeasureText(chip.m_sLabel, s_uiChipFontSize));
    plUiText* pText = Text(pButton, chip.m_sLabel);
    Place(pText, 8.0f, 0.0f, fLabel + 2.0f, s_fChipHeight);
    pText->m_uiFontSize = s_uiChipFontSize;
    pText->m_Color = chip.m_bActive ? Foreground() : Muted();

    if (!chip.m_sKey.IsEmpty())
      BuildKeyCap(pButton, chip.m_sKey, 8.0f + fLabel + 6.0f, (s_fChipHeight - s_fKeyHeight) * 0.5f);

    SetToolTip(ref_board, chip.m_sName, chip.m_sTooltip);
  }

  /// The strip's content: what it draws, and in the engine shell the chips' clicks.
  class plEditorUiBrushModeStrip : public plEditorUiContent
  {
  public:
    plEditorUiBrushModeStrip()
      : plEditorUiContent("Modeling.ModeStrip")
    {
    }

    plDelegate<void(plStringView)> m_OnChip;

  protected:
    void OnUiEvent(const plMsgUiEvent& event) override
    {
      const plStringView sId = event.m_sIdentifier.GetView();
      if (sId.StartsWith("Strip.") && m_OnChip.IsValid())
        m_OnChip(sId);
    }
  };
} // namespace

//////////////////////////////////////////////////////////////////////////
// mode strip

plEngineBrushModeStrip::plEngineBrushModeStrip(plEngineViewWidget* pView, plBrushToolContext* pContext)
  : m_pView(pView)
  , m_pContext(pContext)
{
  plBrushToolContext::s_ModeEvents.AddEventHandler(plMakeDelegate(&plEngineBrushModeStrip::BrushModeEventHandler, this));
  m_ViewSubscription = m_pView->m_Destroying.AddEventHandler(plMakeDelegate(&plEngineBrushModeStrip::ViewDestroyingHandler, this));

  // the info line follows the tool's live measurements, handle occlusion follows the camera, and the strip follows the
  // view into the panel that shows it
  m_Timers.Start(plTime::MakeFromMilliseconds(100), [this]()
    { Poll(); }, true);

  FollowPanel();
  Rebuild();
}

plEngineBrushModeStrip::~plEngineBrushModeStrip()
{
  m_Timers.StopAll();
  Detach();

  if (m_pView != nullptr)
    m_pView->m_Destroying.RemoveEventHandler(m_ViewSubscription);

  plBrushToolContext::s_ModeEvents.RemoveEventHandler(plMakeDelegate(&plEngineBrushModeStrip::BrushModeEventHandler, this));
}

void plEngineBrushModeStrip::ViewDestroyingHandler(plEngineViewWidget& ref_view)
{
  // the view goes with its window before the tool does; its event goes with it
  Detach();
  m_pView = nullptr;
  m_ViewSubscription = 0;
}

bool plEngineBrushModeStrip::FollowPanel()
{
  plEditorUiEngineViewPanel* pPanel = m_pView != nullptr ? plEditorUiEngineViewPanel::FindForView(m_pView) : nullptr;
  if (pPanel == m_pPanel)
    return m_pPanel != nullptr;

  // a panel that no longer shows the view may be gone, and its children with it: the strip only forgets it
  m_pPanel = pPanel;
  m_pContent = nullptr;

  if (m_pPanel == nullptr)
    return false;

  // a chip clicked in the strip; the strip is rebuilt after the click's event
  plEditorUiBrushModeStrip* pContent = PL_DEFAULT_NEW(plEditorUiBrushModeStrip);
  pContent->m_OnChip = [this](plStringView sChip)
  {
    m_Timers.Start(plTime::MakeZero(), [this, sChip = plString(sChip)]()
      { ActivateChip(sChip); });
  };

  m_pContent = m_pPanel->AddChild(plUniquePtr<plEditorUiContent>(pContent, plFoundation::GetDefaultAllocator()));
  return true;
}

void plEngineBrushModeStrip::Detach()
{
  // removing the child also ends it being an overlay
  if (m_pPanel != nullptr && m_pContent != nullptr && m_pView != nullptr && plEditorUiEngineViewPanel::FindForView(m_pView) == m_pPanel)
    m_pPanel->RemoveChild(m_pContent);

  m_pPanel = nullptr;
  m_pContent = nullptr;
}

void plEngineBrushModeStrip::ActivateChip(plStringView sChip)
{
  plUInt32 uiValue = 0;

  if (sChip.StartsWith("Strip.mode.") && plConversionUtils::StringToUInt(plStringView(sChip.GetStartPointer() + 11, sChip.GetEndPointer()), uiValue).Succeeded())
  {
    m_pContext->SetSubMode(static_cast<plBrushSubMode::Enum>(uiValue));
  }
  else if (sChip.StartsWith("Strip.faces.") && plConversionUtils::StringToUInt(plStringView(sChip.GetStartPointer() + 12, sChip.GetEndPointer()), uiValue).Succeeded())
  {
    m_pContext->SetFaceGranularity(static_cast<plBrushFaceGranularity::Enum>(uiValue));
  }

  // a refused switch (no brush selected) must not leave a lying chip
  Rebuild();
}

void plEngineBrushModeStrip::BrushModeEventHandler(const plBrushModeEvent& e)
{
  if (e.m_pContext != m_pContext)
    return;

  Rebuild();
}

void plEngineBrushModeStrip::Poll()
{
  m_pContext->UpdateForCamera();

  const plEditorUiContent* pShown = m_pContent;
  if (!FollowPanel())
    return;

  if (m_pContent == pShown && m_pContext->GetInfoText() == m_sInfo)
    return;

  Rebuild();
}

void plEngineBrushModeStrip::Rebuild()
{
  using namespace plEditorUi;

  if (m_pContent == nullptr || !m_pContent->IsReady())
    return;

  m_sInfo = m_pContext->GetInfoText();
  auto& board = m_pContent->GetSurface()->GetBlackboard();

  // the chips show the keys as currently bound in the shortcut editor
  const plModelingCommand::Enum modeCommands[] = {plModelingCommand::ModeObject, plModelingCommand::ModeVertex, plModelingCommand::ModeEdge, plModelingCommand::ModeFace};
  const char* szLabels[] = {"Object", "Vertex", "Edge", "Face"};
  const char* szHints[] = {"Draw and place brushes", "Edit vertices", "Edit edges", "Edit faces"};
  const plBrushSubMode::Enum mode = m_pContext->GetSubMode();

  auto chipName = [](plStringView sKind, plUInt32 uiValue)
  {
    plStringBuilder sName;
    sName.SetFormat("Strip.{}.{}", sKind, uiValue);
    return plString(sName);
  };

  plHybridArray<Chip, 6> chips;
  for (plUInt32 i = 0; i < 4; ++i)
  {
    Chip& chip = chips.ExpandAndGetRef();
    chip.m_sName = chipName("mode", i);
    chip.m_sLabel = szLabels[i];
    chip.m_bActive = static_cast<plUInt32>(mode) == i;

    const plString sKey = plBrushActions::GetShortcutText(modeCommands[i]);
    plStringBuilder sTooltip = szHints[i];
    if (!sKey.IsEmpty())
      sTooltip.AppendFormat(" ({})", sKey);
    chip.m_sTooltip = sTooltip;

    // Object's key (Esc) is in its tooltip only
    if (i != 0)
      chip.m_sKey = sKey;
  }

  // Face mode only: what a click takes, the whole face as rendered or one polygon of the mesh
  if (mode == plBrushSubMode::Face)
  {
    const plString sKey = plBrushActions::GetShortcutText(plModelingCommand::ToggleFaceGranularity);
    plStringBuilder sKeyHint;
    if (!sKey.IsEmpty())
      sKeyHint.SetFormat(" ({} switches)", sKey);

    Chip& csg = chips.ExpandAndGetRef();
    csg.m_sName = chipName("faces", plBrushFaceGranularity::Surface);
    csg.m_sLabel = "CSG";
    csg.m_sTooltip = plStringBuilder("CSG faces: a click selects a whole face as rendered, all its cut pieces and cut faces included", sKeyHint);
    csg.m_bActive = m_pContext->GetFaceGranularity() == plBrushFaceGranularity::Surface;
    csg.m_bSeparatorBefore = true;

    Chip& mesh = chips.ExpandAndGetRef();
    mesh.m_sName = chipName("faces", plBrushFaceGranularity::Polygon);
    mesh.m_sLabel = "Mesh";
    mesh.m_sTooltip = plStringBuilder("Mesh faces: a click selects one polygon of the brush's own mesh", sKeyHint);
    mesh.m_bActive = m_pContext->GetFaceGranularity() == plBrushFaceGranularity::Polygon;
  }

  // the pill: the chips in a row on a plate with a hairline border
  float fPillWidth = s_fPillPadding;
  for (plUInt32 i = 0; i < chips.GetCount(); ++i)
  {
    if (i > 0)
      fPillWidth += chips[i].m_bSeparatorBefore ? 9.0f : s_fChipGap;
    fPillWidth += ChipWidth(chips[i]);
  }
  fPillWidth += s_fPillPadding;

  const float fInfoWidth = m_sInfo.IsEmpty() ? 0.0f : plMath::Ceil(MeasureCodeText(m_sInfo, s_uiInfoFontSize)) + 16.0f;
  const float fWidth = plMath::Max(fPillWidth, fInfoWidth);
  const float fHeight = s_fPillHeight + (m_sInfo.IsEmpty() ? 0.0f : s_fInfoGap + s_fInfoHeight);

  plUiPanel* pRoot = Panel();
  pRoot->m_Color = Background();

  plUiPanel* pPill = Panel(pRoot);
  Place(pPill, 0.0f, 0.0f, fPillWidth, s_fPillHeight);
  pPill->m_Color = Background();
  pPill->m_fBorderWidth = 1.0f;
  pPill->m_BorderColor = Line();
  pPill->m_vCornerRadius = plVec4(6);

  float x = s_fPillPadding;
  for (plUInt32 i = 0; i < chips.GetCount(); ++i)
  {
    if (i > 0 && chips[i].m_bSeparatorBefore)
    {
      plUiPanel* pSeparator = Panel(pPill);
      Place(pSeparator, x + 4.0f, s_fPillPadding + 5.0f, 1.0f, s_fChipHeight - 10.0f);
      pSeparator->m_Color = Line();
      x += 9.0f;
    }
    else if (i > 0)
    {
      x += s_fChipGap;
    }

    const float fChip = ChipWidth(chips[i]);
    BuildChip(pPill, chips[i], x, fChip, board);
    x += fChip;
  }

  // the info line: live measurements in the code face
  if (!m_sInfo.IsEmpty())
  {
    plUiPanel* pInfo = Panel(pRoot);
    Place(pInfo, 0.0f, s_fPillHeight + s_fInfoGap, fInfoWidth, s_fInfoHeight);
    pInfo->m_Color = Background();
    pInfo->m_vCornerRadius = plVec4(4);

    plUiText* pText = Text(pInfo, m_sInfo);
    Place(pText, 8.0f, 0.0f, fInfoWidth - 16.0f, s_fInfoHeight);
    pText->m_hFont = GetCodeFont();
    pText->m_uiFontSize = s_uiInfoFontSize;
    pText->m_Color = Foreground();
  }

  m_pContent->SetDocument(Finish(pRoot));

  // the overlay covers only the two plates: the view shows around them
  const plRectFloat cover[] = {plRectFloat(0.0f, 0.0f, fPillWidth, s_fPillHeight), plRectFloat(0.0f, s_fPillHeight + s_fInfoGap, fInfoWidth, s_fInfoHeight)};
  m_pPanel->SetOverlayChild(*m_pContent, plRectFloat(s_fStripMargin, s_fStripMargin, fWidth, fHeight), plArrayPtr<const plRectFloat>(cover, m_sInfo.IsEmpty() ? 1u : 2u));
}

//////////////////////////////////////////////////////////////////////////
// marking menu in the engine shell

namespace
{
  /// The marking menu in a popup window of the engine shell, at the cursor over a view (see plModelingEngineUi). The
  /// pointer is followed wherever it is, so moving toward an item highlights it also over the view; a press anywhere picks the highlighted item or, near the centre, closes the menu.
  /// The keys come through the popup key handler of the windows (and the view slots): Esc or the menu key again close
  /// it, releasing the menu key after moving toward an item picks it.
  class plShellMarkingMenu
  {
  public:
    static bool Show(plDocument* pDocument, plBrushSubMode::Enum currentMode);
    static void Close();
    static plShellMarkingMenu* GetOpen() { return s_pOpen; }

    ~plShellMarkingMenu()
    {
      if (m_pHost != nullptr && m_UpdateSubscription != 0)
        m_pHost->m_BeforeUpdate.RemoveEventHandler(m_UpdateSubscription);
    }

  private:
    void Publish();
    void ApplyShape();
    void FollowPointer();
    void Pick();
    bool IsMenuKey(const plEditorPlatformEvent& event) const;

    void OnBeforeUpdate(plEditorUiWindowHost& ref_host) { FollowPointer(); }
    static bool HandleKey(const plEditorPlatformEvent& event);
    static void OnPointerPressed(plEditorUiWindowHost& ref_host);
    static void OnFocusLost(plEditorUiWindowHost& ref_host);
    static void DeleteLater(plShellMarkingMenu* pMenu);

    static plShellMarkingMenu* s_pOpen;
    static bool s_bListening;

    plEditorUiWindowHost* m_pOwner = nullptr;
    plUniquePtr<plEditorUiWindowHost> m_pHost;
    plEventSubscriptionID m_UpdateSubscription = 0;
    plDocument* m_pDocument = nullptr;
    plString m_Labels[MarkingMenuItemCount];
    plString m_Shortcuts[MarkingMenuItemCount];
    bool m_Current[MarkingMenuItemCount] = {};
    plKeyChord m_MenuKey;
    plInt32 m_iHovered = -1;
  };

  plShellMarkingMenu* plShellMarkingMenu::s_pOpen = nullptr;
  bool plShellMarkingMenu::s_bListening = false;

  bool plShellMarkingMenu::Show(plDocument* pDocument, plBrushSubMode::Enum currentMode)
  {
    plEditorUiWindowHost* pOwner = plEditorUiWindowHost::FindHost(plEditorUiWindowHost::GetDialogOwner());
    if (pOwner == nullptr)
      return false;

    Close();
    plEditorUiPopupMenu::CloseAll();

    plEditorPlatformWindowDesc desc;
    desc.m_sTitle = "Marking Menu";
    desc.m_Kind = plEditorPlatformWindowKind::Popup;
    desc.m_pOwner = &pOwner->GetWindow();
    desc.m_Size = plSizeU32(static_cast<plUInt32>(s_fMenuWidth), static_cast<plUInt32>(s_fMenuHeight));

    plShellMarkingMenu* pMenu = PL_DEFAULT_NEW(plShellMarkingMenu);
    pMenu->m_pHost = PL_DEFAULT_NEW(plEditorUiWindowHost, desc, "Modeling.MarkingMenu");
    if (!pMenu->m_pHost->IsReady())
    {
      PL_DEFAULT_DELETE(pMenu);
      return false;
    }

    pMenu->m_pOwner = pOwner;
    pMenu->m_pDocument = pDocument;
    plKeyChord::Parse(plBrushActions::GetShortcutText(plModelingCommand::MarkingMenu), pMenu->m_MenuKey).IgnoreResult();
    for (plUInt32 i = 0; i < MarkingMenuItemCount; ++i)
    {
      const plModelingCommand::Enum cmd = c_MenuEntries[i];
      pMenu->m_Labels[i] = plBrushActions::GetDisplayName(cmd);
      pMenu->m_Shortcuts[i] = plBrushActions::GetShortcutText(cmd);
      pMenu->m_Current[i] = IsCurrentMode(cmd, currentMode);
    }

    pMenu->m_pHost->SetUiScale(pOwner->GetUiScale());

    plUiPanel* pRoot = plEditorUi::Panel();
    pRoot->m_Color = plEditorUi::Background();
    plEditorUi::DrawList(pRoot, "MarkingMenu", "MarkingMenu.Drawing");
    pMenu->m_pHost->SetDocument(plEditorUi::Finish(pRoot));
    pMenu->Publish();

    // centred on the cursor
    const plVec2 vPixels = pOwner->LogicalToPixels(plVec2(s_fMenuWidth, s_fMenuHeight)) - pOwner->LogicalToPixels(plVec2::MakeZero());
    const plInt32 iWidth = static_cast<plInt32>(plMath::Ceil(vPixels.x));
    const plInt32 iHeight = static_cast<plInt32>(plMath::Ceil(vPixels.y));
    const plVec2I32 vCursor = plEditorPlatform::GetCursorScreenPosition();
    pMenu->m_pHost->GetWindow().SetClientScreenRect(plRectI32(vCursor.x - iWidth / 2, vCursor.y - iHeight / 2, iWidth, iHeight));
    pMenu->ApplyShape();
    pMenu->m_pHost->GetWindow().Show(false);

    // presses and focus changes are followed from the first menu on (a handler cannot be added or removed while the
    // event that opens or closes a menu is broadcast); keys go to the menu while it is open
    if (!s_bListening)
    {
      s_bListening = true;
      plEditorUiWindowHost::s_PointerPressed.AddEventHandler(plMakeDelegate(&plShellMarkingMenu::OnPointerPressed));
      plEditorUiWindowHost::s_FocusLost.AddEventHandler(plMakeDelegate(&plShellMarkingMenu::OnFocusLost));
    }
    plEditorUiWindowHost::SetPopupKeyHandler(plEditorUiWindowHost::PopupKeyHandler(&plShellMarkingMenu::HandleKey));

    pMenu->m_UpdateSubscription = pMenu->m_pHost->m_BeforeUpdate.AddEventHandler(plMakeDelegate(&plShellMarkingMenu::OnBeforeUpdate, pMenu));

    s_pOpen = pMenu;
    return true;
  }

  void plShellMarkingMenu::Close()
  {
    plShellMarkingMenu* pMenu = s_pOpen;
    if (pMenu == nullptr)
      return;

    s_pOpen = nullptr;
    plEditorUiWindowHost::SetPopupKeyHandler({});
    pMenu->m_pHost->GetWindow().Hide();
    DeleteLater(pMenu);
  }

  void plShellMarkingMenu::DeleteLater(plShellMarkingMenu* pMenu)
  {
    // after the frame, or later while its window is still delivering an event
    plEditorUiRuntime::RunAfterFrame([pMenu]() mutable
      {
        if (pMenu->m_pHost->GetWindow().IsDeliveringEvent())
          DeleteLater(pMenu);
        else
          PL_DEFAULT_DELETE(pMenu);
      });
  }

  void plShellMarkingMenu::Publish()
  {
    plHybridArray<plEditorUiDraw::MarkingMenuItem, MarkingMenuItemCount> items;
    for (plUInt32 i = 0; i < MarkingMenuItemCount; ++i)
    {
      plEditorUiDraw::MarkingMenuItem& item = items.ExpandAndGetRef();
      item.m_sLabel = m_Labels[i];
      item.m_sShortcut = m_Shortcuts[i];
      item.m_bCurrent = m_Current[i];
    }

    plUiDrawListWriter writer;
    plEditorUiDraw::MarkingMenu(writer, GetMenuCenter(), items, m_iHovered);
    writer.Publish(m_pHost->GetSurface().GetBlackboard(), plMakeHashedString("MarkingMenu.Drawing"));
    m_pHost->RequestRedraw();
  }

  void plShellMarkingMenu::ApplyShape()
  {
    // the window keeps the ring of wedges, the centre dot and the label plates, row by row; the view shows elsewhere
    const plVec2 vCenter = GetMenuCenter();
    const float fPixels = (m_pHost->LogicalToPixels(plVec2(1, 0)) - m_pHost->LogicalToPixels(plVec2::MakeZero())).x;
    const plRectI32 client = m_pHost->GetWindow().GetClientScreenRect();

    plDynamicArray<plRectI32> shape;
    auto addSpan = [&](plInt32 y, float fLeft, float fRight)
    {
      const plInt32 x0 = static_cast<plInt32>(plMath::Floor(fLeft * fPixels));
      const plInt32 x1 = static_cast<plInt32>(plMath::Ceil(fRight * fPixels));
      if (x1 > x0)
        shape.PushBack(plRectI32(x0, y, x1 - x0, 1));
    };

    const float fOuter = plEditorUiDraw::MarkingMenuOuterRadius + 1.5f;
    const float fInner = plEditorUiDraw::MarkingMenuInnerRadius - 1.5f;
    constexpr float fDot = 8.0f;
    for (plInt32 y = 0; y < client.height; ++y)
    {
      const float fDy = (y + 0.5f) / fPixels - vCenter.y;
      if (plMath::Abs(fDy) >= fOuter)
        continue;

      const float fOut = plMath::Sqrt(fOuter * fOuter - fDy * fDy);
      if (plMath::Abs(fDy) < fInner)
      {
        const float fIn = plMath::Sqrt(fInner * fInner - fDy * fDy);
        addSpan(y, vCenter.x - fOut, vCenter.x - fIn);
        addSpan(y, vCenter.x + fIn, vCenter.x + fOut);
      }
      else
      {
        addSpan(y, vCenter.x - fOut, vCenter.x + fOut);
      }

      if (plMath::Abs(fDy) < fDot)
      {
        const float fHalf = plMath::Sqrt(fDot * fDot - fDy * fDy);
        addSpan(y, vCenter.x - fHalf, vCenter.x + fHalf);
      }
    }

    for (plUInt32 k = 0; k < MarkingMenuItemCount; ++k)
    {
      const plRectFloat plate = GetPlateRect(vCenter, k, m_Labels[k], m_Shortcuts[k]);
      const plInt32 x = static_cast<plInt32>(plMath::Floor(plate.x * fPixels));
      const plInt32 y = static_cast<plInt32>(plMath::Floor(plate.y * fPixels));
      shape.PushBack(plRectI32(x, y, static_cast<plInt32>(plMath::Ceil(plate.Right() * fPixels)) - x, static_cast<plInt32>(plMath::Ceil(plate.Bottom() * fPixels)) - y));
    }

    m_pHost->GetWindow().SetShape(shape);
  }

  void plShellMarkingMenu::FollowPointer()
  {
    const plRectI32 client = m_pHost->GetWindow().GetClientScreenRect();
    const plVec2I32 vCursor = plEditorPlatform::GetCursorScreenPosition();
    const plVec2 vLocal = m_pHost->PixelsToLogical(plVec2(static_cast<float>(vCursor.x - client.x), static_cast<float>(vCursor.y - client.y)));

    const plInt32 iHovered = plEditorUiDraw::MarkingMenuItemAt(GetMenuCenter(), MarkingMenuItemCount, vLocal);
    if (iHovered == m_iHovered)
      return;

    m_iHovered = iHovered;
    Publish();
  }

  void plShellMarkingMenu::Pick()
  {
    FollowPointer();

    // run once the menu is gone: the command may change the selection and the views
    plDocument* pDocument = m_pDocument;
    const plInt32 iItem = m_iHovered;
    Close();

    if (iItem >= 0)
    {
      const plModelingCommand::Enum cmd = c_MenuEntries[iItem];
      plEditorUiRuntime::RunAfterFrame([pDocument, cmd]()
        { plBrushActions::Execute(pDocument, cmd).IgnoreResult(); });
    }
  }

  bool plShellMarkingMenu::IsMenuKey(const plEditorPlatformEvent& event) const
  {
    // a release's modifiers can differ from the press's
    return m_MenuKey.IsValid() && event.m_Key == m_MenuKey.m_Key;
  }

  bool plShellMarkingMenu::HandleKey(const plEditorPlatformEvent& event)
  {
    plShellMarkingMenu* pMenu = s_pOpen;
    if (pMenu == nullptr)
      return false;

    // the menu has the keyboard: every key stops here
    if (event.m_bRepeat)
      return true;

    if (event.m_Type == plEditorPlatformEvent::Type::KeyDown && (event.m_Key == plEditorPlatformKey::Escape || pMenu->IsMenuKey(event)))
    {
      Close();
    }
    else if (event.m_Type == plEditorPlatformEvent::Type::KeyUp && pMenu->IsMenuKey(event))
    {
      // a flick: hold the menu key, move toward an item, let go
      pMenu->FollowPointer();
      if (pMenu->m_iHovered >= 0)
        pMenu->Pick();
    }

    return true;
  }

  void plShellMarkingMenu::OnPointerPressed(plEditorUiWindowHost& ref_host)
  {
    // a press toward an item picks it, wherever it is; one near the centre closes the menu
    if (s_pOpen != nullptr)
      s_pOpen->Pick();
  }

  void plShellMarkingMenu::OnFocusLost(plEditorUiWindowHost& ref_host)
  {
    // the window the menu belongs to lost the keyboard to another application or window
    if (s_pOpen != nullptr && &ref_host == s_pOpen->m_pOwner)
      Close();
  }
} // namespace

bool plModelingEngineUi::ShowShellMarkingMenu(plDocument* pDocument, plBrushSubMode::Enum currentMode)
{
  return plShellMarkingMenu::Show(pDocument, currentMode);
}

bool plModelingEngineUi::IsShellMarkingMenuOpen()
{
  return plShellMarkingMenu::GetOpen() != nullptr;
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_EngineBrushOverlays);
