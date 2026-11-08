#include <EditorPluginModeling/EditorPluginModelingPCH.h>

#include <EditorPluginModeling/Brush/BrushActions.h>
#include <EditorPluginModeling/Brush/BrushEditTool.h>
#include <EditorPluginModeling/Brush/BrushToolContext.h>
#include <EditorPluginModeling/Brush/EngineModelingPanel.h>
#include <EditorPluginModeling/Brush/ModelingPanel.h>
#include <EditorPluginModeling/Brush/ModelingPanelContent.h>
#include <EditorUi/Menu.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GameUiPlugin/Elements/UiElements.h>

namespace
{
  constexpr float s_fSide = 10.0f;             ///< left and right padding of the sections
  constexpr float s_fHeaderHeight = 24.0f;     ///< a section header
  constexpr float s_fGridButtonWidth = 32.0f;  ///< an icon button of a grid
  constexpr float s_fGridButtonHeight = 28.0f;
  constexpr float s_fGridGap = 4.0f;
  constexpr float s_fHintLine = 16.0f;         ///< line height of hints and labels
  constexpr float s_fScrollbarSpace = 6.0f;    ///< kept free for the scroll bar on the right
  constexpr float s_fLabelWidth = 80.0f;       ///< the label column of the Texture section
  constexpr float s_fNumberWidth = 96.0f;      ///< a number field, narrower when two do not fit
  constexpr float s_fDisabledOpacity = 0.45f;
  constexpr plUInt32 s_uiLabelSize = plEditorUi::FontSize - 1;
  constexpr plUInt32 s_uiHintSize = plEditorUi::FontSize - 2;
  constexpr plUInt32 s_uiTagSize = plEditorUi::MinFontSize;

  constexpr const char* s_szDisabled = "Mo.Disabled"; ///< always true; disabled buttons bind it

  /// The Texture section's number fields.
  struct TextureField
  {
    const char* m_szLetter;
    const char* m_szUnit;
    const char* m_szTooltip;
    double m_fMin;
    double m_fMax;
    plUInt8 m_uiDecimals;
  };

  constexpr TextureField s_TextureFields[] = {
    {"U", "m", "World size of one texture repeat along U.", 0.01, 1000.0, 3},
    {"V", "m", "World size of one texture repeat along V.", 0.01, 1000.0, 3},
    {"U", "", "Shift along U, in texture repeats.", -1000.0, 1000.0, 3},
    {"V", "", "Shift along V, in texture repeats.", -1000.0, 1000.0, 3},
    {"", "\xC2\xB0", "", -360.0, 360.0, 1},
  };

  double& FieldValue(plModelingTextureSettings& ref_settings, plUInt32 uiField)
  {
    switch (uiField)
    {
      case 0:
        return ref_settings.m_fTileU;
      case 1:
        return ref_settings.m_fTileV;
      case 2:
        return ref_settings.m_fOffsetU;
      case 3:
        return ref_settings.m_fOffsetV;
      default:
        return ref_settings.m_fRotation;
    }
  }

  plString FormatField(plUInt32 uiField, double fValue)
  {
    plStringBuilder sText;
    sText.SetFormat("{}", plArgF(fValue, s_TextureFields[uiField].m_uiDecimals));
    return sText;
  }

  plString Name(plStringView sKind, plUInt32 uiIndex)
  {
    plStringBuilder sName;
    sName.SetFormat("Mo.{}.{}", sKind, uiIndex);
    return sName;
  }

  plString FieldBinding(plUInt32 uiField)
  {
    plStringBuilder sName;
    sName.SetFormat("Mo.texfield.{}.text", uiField);
    return sName;
  }

  /// The index after a prefix ("Mo.cmd.12" with "Mo.cmd."); false when the identifier has another prefix.
  bool ParseIndex(plStringView sIdentifier, plStringView sPrefix, plUInt32& out_uiIndex)
  {
    if (!sIdentifier.StartsWith(sPrefix))
      return false;

    const plStringView sRest(sIdentifier.GetStartPointer() + sPrefix.GetElementCount(), sIdentifier.GetEndPointer());
    return plConversionUtils::StringToUInt(sRest, out_uiIndex).Succeeded();
  }

  void Place(plUiElement* pElement, float x, float y, float fWidth, float fHeight)
  {
    pElement->m_vAnchorMin = pElement->m_vAnchorMax = plVec2::MakeZero();
    pElement->m_vOffsets = plVec4(x, y, fWidth, fHeight);
  }

  /// An opaque mix of two theme colours, as the style pass tints plates.
  plColorGammaUB Mix(const plColorGammaUB& a, const plColorGammaUB& b, float t)
  {
    auto channel = [t](plUInt8 x, plUInt8 y)
    { return static_cast<plUInt8>(plMath::Clamp(x + (y - x) * t + 0.5f, 0.0f, 255.0f)); };

    return plColorGammaUB(channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), 255);
  }

  /// Hovered rows: between Plate and Raised.
  plColorGammaUB Hover()
  {
    return Mix(plEditorUi::Plate(), plEditorUi::Raised(), 0.5f);
  }

  /// The edge of an active button: the accent faded into the plate.
  plColorGammaUB ActiveEdge()
  {
    return Mix(plEditorUi::Plate(), plEditorUi::Accent(), 0.45f);
  }

  /// Greedy word wrap at fWidth; line breaks in the text start new lines.
  void WrapLines(plStringView sText, plUInt32 uiFontSize, float fWidth, plDynamicArray<plString>& out_lines)
  {
    out_lines.Clear();

    plHybridArray<plStringView, 4> paragraphs;
    sText.Split(true, paragraphs, "\n");

    for (plStringView sParagraph : paragraphs)
    {
      plHybridArray<plStringView, 32> words;
      sParagraph.Split(false, words, " ");

      plStringBuilder sLine;
      for (plStringView sWord : words)
      {
        plStringBuilder sCandidate = sLine;
        if (!sCandidate.IsEmpty())
          sCandidate.Append(" ");
        sCandidate.Append(sWord);

        if (!sLine.IsEmpty() && plEditorUi::MeasureText(sCandidate, uiFontSize) > fWidth)
        {
          out_lines.PushBack(sLine);
          sLine = sWord;
        }
        else
        {
          sLine = sCandidate;
        }
      }

      out_lines.PushBack(sLine);
    }
  }

  plUiText* AddText(plUiElement* pParent, plStringView sText, float x, float y, float fWidth, float fHeight, plUInt32 uiFontSize, const plColorGammaUB& color)
  {
    plUiText* pText = plEditorUi::Text(pParent, sText);
    Place(pText, x, y, fWidth, fHeight);
    pText->m_uiFontSize = uiFontSize;
    pText->m_Color = color;
    return pText;
  }

  /// A borderless button with no plate of its own: rows, headers and check boxes.
  plUiButton* AddGhostButton(plUiElement* pParent, plStringView sName, float x, float y, float fWidth, float fHeight)
  {
    plUiButton* pButton = plEditorUi::Button(pParent, {}, sName);
    pButton->ClearChildren();
    Place(pButton, x, y, fWidth, fHeight);
    pButton->m_Color = plColorGammaUB(0, 0, 0, 0);
    pButton->m_HoverColor = plColorGammaUB(0, 0, 0, 0);
    pButton->m_PressedColor = plColorGammaUB(0, 0, 0, 0);
    pButton->m_hStyle = plEditorUi::GetGhostStyle();
    return pButton;
  }

  /// A text button (plate, hairline border); the active one sits on the selection fill with an accent edge.
  plUiButton* AddTextButton(plUiElement* pParent, plStringView sLabel, plStringView sName, float x, float y, float fWidth, float fHeight, bool bActive, bool bCentred)
  {
    plUiButton* pButton = plEditorUi::TextButton(pParent, sLabel, sName);
    Place(pButton, x, y, fWidth, fHeight);

    if (bActive)
    {
      pButton->m_Color = plEditorUi::Selection();
      pButton->m_HoverColor = plEditorUi::Selection();
      pButton->m_BorderColor = ActiveEdge();
    }

    if (!bCentred)
    {
      plUiText* pText = static_cast<plUiText*>(pButton->m_Children[0]);
      pText->m_HAlign = plUiTextHAlign::Left;
      pText->m_vOffsets.x = 10.0f;
    }

    return pButton;
  }

  float TextButtonWidth(plStringView sLabel)
  {
    return plMath::Ceil(plEditorUi::MeasureText(sLabel, s_uiLabelSize)) + 20.0f;
  }
} // namespace

plEngineModelingView::plEngineModelingView(plUniquePtr<plModelingPanel>&& pPanel)
  : plEditorUiContent("Modeling.Panel")
  , m_pPanel(std::move(pPanel))
{
  for (bool& bOpen : m_SectionOpen)
    bOpen = true;

  // Output starts collapsed
  m_SectionOpen[Output] = false;

  m_pPanel->SetView(this);
}

plEngineModelingView::~plEngineModelingView()
{
  m_pPanel->SetView(nullptr);
}

void plEngineModelingView::SetSectionOpen(Section section, bool bOpen)
{
  m_SectionOpen[section] = bOpen;
  m_bRebuild = true;
}

// Document.

void plEngineModelingView::OnUiUpdate()
{
  if (m_bRebuild || GetSurface()->GetLogicalSize().x != m_fBuiltWidth)
    BuildDocument();

  CommitFocusedFieldOnFocusChange();
}

void plEngineModelingView::BuildDocument()
{
  using namespace plEditorUi;

  m_bRebuild = false;
  m_fBuiltWidth = GetSurface()->GetLogicalSize().x;
  m_fContentWidth = plMath::Max(m_fBuiltWidth - s_fScrollbarSpace, 220.0f);

  // a rebuild keeps the keyboard focus where it was (a number field being edited, a grid button)
  const plString sFocused = GetSurface()->GetCanvas().GetFocusedElementName();

  GetSurface()->GetBlackboard().SetEntryValue(s_szDisabled, true);

  plUiPanel* pRoot = Panel();
  pRoot->m_Color = plEditorUi::Surface();

  plUiScrollView* pScroll = PL_DEFAULT_NEW(plUiScrollView);
  pRoot->m_Children.PushBack(pScroll);
  Fill(pScroll);
  pScroll->m_sName = "Modeling.Sections";
  pScroll->m_Color = plColorGammaUB(0, 0, 0, 0);
  pScroll->m_fSpacing = 0.0f;
  pScroll->m_vPadding = plVec4(0, 2, 0, 8);

  if (m_pPanel->GetSceneDocument() != nullptr)
  {
    BuildSection(pScroll, Mode, "Mode");
    BuildSection(pScroll, Draw, "Draw");

    const auto groups = plModelingPanelContent::GetCommandGroups();
    for (plUInt32 i = 0; i < groups.GetCount(); ++i)
      BuildSection(pScroll, static_cast<Section>(Select + i), groups[i].m_szTitle);

    BuildSection(pScroll, Texture, "Texture");
    BuildSection(pScroll, Output, "Output");
    BuildSection(pScroll, Health, "Health");
  }

  SetDocument(Finish(pRoot));

  if (!sFocused.IsEmpty())
    GetSurface()->GetCanvas().FocusElement(sFocused, GetSurface()->GetLogicalSize());
}

float plEngineModelingView::BuildSection(plUiElement* pScroll, Section section, const char* szTitle)
{
  plUiPanel* pSection = plEditorUi::Panel(pScroll);
  pSection->m_Color = plColorGammaUB(0, 0, 0, 0);

  float y = BuildHeader(pSection, section, szTitle);

  if (m_SectionOpen[section])
  {
    switch (section)
    {
      case Mode:
        y = BuildModeButtons(pSection, y);
        break;

      case Draw:
        y = BuildShapeGrid(pSection, y);
        y += BuildHint(pSection, plModelingPanelContent::GetDrawHint(), s_fSide, y, m_fContentWidth - 2 * s_fSide) + 8.0f;
        break;

      case Select:
      case Edit:
      case Csg:
      case Pivot:
      {
        const plUInt32 uiGroup = section - Select;
        y = BuildCommandGrid(pSection, uiGroup, y);

        if (section == Select)
          y = BuildSelectToggles(pSection, y);

        y += BuildHint(pSection, plModelingPanelContent::GetCommandGroups()[uiGroup].m_szHint, s_fSide, y, m_fContentWidth - 2 * s_fSide) + 8.0f;
        break;
      }

      case Texture:
        y = m_pPanel->HasTextureSelection() ? BuildTexture(pSection, y) : BuildTextureEmpty(pSection, y);
        break;

      case Output:
        y = BuildOutput(pSection, y);
        break;

      case Health:
        y = BuildHealth(pSection, y);
        break;

      default:
        break;
    }
  }

  // inside the scroll view the height is the section's preferred size
  pSection->m_vOffsets.w = y;
  return y;
}

float plEngineModelingView::BuildHeader(plUiElement* pSection, Section section, const char* szTitle)
{
  using namespace plEditorUi;

  plUiButton* pHeader = AddGhostButton(pSection, Name("section", section), 0, 0, m_fContentWidth, s_fHeaderHeight);

  // a chevron, the uppercase title and a hairline to the right edge
  plUiImage* pChevron = SvgImage(pHeader, IconPath(m_SectionOpen[section] ? Icon::Expand : Icon::Next));
  Place(pChevron, 12.0f, (s_fHeaderHeight - 11.0f) * 0.5f, 11.0f, 11.0f);
  pChevron->m_Color = Muted();

  plStringBuilder sTitle = szTitle;
  sTitle.ToUpper();
  const float fTitle = plMath::Ceil(MeasureText(sTitle, s_uiTagSize));
  AddText(pHeader, sTitle, 29.0f, 0.0f, fTitle + 2.0f, s_fHeaderHeight, s_uiTagSize, Muted());

  const float fLine = 29.0f + fTitle + 6.0f;
  plUiPanel* pLine = Panel(pHeader);
  Place(pLine, fLine, s_fHeaderHeight * 0.5f, plMath::Max(m_fContentWidth - 8.0f - fLine, 0.0f), 1.0f);
  pLine->m_Color = Line();

  return s_fHeaderHeight;
}

float plEngineModelingView::BuildModeButtons(plUiElement* pSection, float y)
{
  using namespace plEditorUi;

  const char* szLabels[] = {"Object", "Vertex", "Edge", "Face"};
  const plModelingCommand::Enum modeCommands[] = {plModelingCommand::ModeObject, plModelingCommand::ModeVertex, plModelingCommand::ModeEdge, plModelingCommand::ModeFace};

  y += 4.0f;

  const float fGap = 4.0f;
  const float fWidth = (m_fContentWidth - 2 * s_fSide - 3 * fGap) / 4.0f;

  for (plUInt32 i = 0; i < 4; ++i)
  {
    const plString sName = Name("mode", i);
    AddTextButton(pSection, szLabels[i], sName, s_fSide + i * (fWidth + fGap), y, fWidth, ControlHeight, m_pPanel->GetCheckedMode() == static_cast<plInt32>(i), true);
    SetToolTip(GetSurface()->GetBlackboard(), sName, plModelingPanelContent::GetCommandTooltip(modeCommands[i]));
  }

  return y + ControlHeight + 8.0f;
}

plUiButton* plEngineModelingView::BuildGridButton(plUiElement* pParent, plStringView sName, plStringView sIcon, plStringView sTooltip, float x, float y, bool bEnabled, bool bChecked)
{
  using namespace plEditorUi;

  plUiButton* pButton = Button(pParent, {}, sName);
  pButton->ClearChildren();
  Place(pButton, x, y, s_fGridButtonWidth, s_fGridButtonHeight);
  pButton->m_Color = bChecked ? Selection() : Plate();
  pButton->m_HoverColor = bChecked ? Selection() : Raised();
  pButton->m_PressedColor = Selection();
  pButton->m_fBorderWidth = 1.0f;
  pButton->m_BorderColor = bChecked ? ActiveEdge() : Line();
  pButton->m_vCornerRadius = plVec4(4);

  plUiImage* pIcon = ButtonIcon(pButton, sIcon);
  pIcon->m_Color = bChecked ? Accent() : Muted();

  // a disabled button fades as a whole (to 45 % over the panel) and ignores the pointer
  if (!bEnabled)
  {
    pButton->m_sDisabledBinding = s_szDisabled;
    pButton->m_Color = Mix(plEditorUi::Surface(), pButton->m_Color, s_fDisabledOpacity);
    pButton->m_DisabledColor = pButton->m_Color;
    pButton->m_BorderColor = Mix(plEditorUi::Surface(), pButton->m_BorderColor, s_fDisabledOpacity);
    pIcon->m_Color = Mix(plEditorUi::Surface(), pIcon->m_Color, s_fDisabledOpacity);
  }
  else
  {
    pButton->m_DisabledColor = pButton->m_Color;
  }

  SetToolTip(GetSurface()->GetBlackboard(), sName, sTooltip);
  return pButton;
}

float plEngineModelingView::BuildShapeGrid(plUiElement* pSection, float y)
{
  const auto shapes = plModelingPanelContent::GetShapes();

  y += 4.0f;

  for (plUInt32 i = 0; i < shapes.GetCount(); ++i)
  {
    const float x = s_fSide + (i % plModelingPanelContent::IconColumns) * (s_fGridButtonWidth + s_fGridGap);
    const float fRow = y + (i / plModelingPanelContent::IconColumns) * (s_fGridButtonHeight + s_fGridGap);
    BuildGridButton(pSection, Name("shape", i), plModelingPanelContent::GetLineIconPath(shapes[i].m_szIcon), shapes[i].m_szLabel, x, fRow, true, m_pPanel->GetArmedShape() == shapes[i].m_iShape);
  }

  const plUInt32 uiRows = (shapes.GetCount() + plModelingPanelContent::IconColumns - 1) / plModelingPanelContent::IconColumns;
  return y + uiRows * s_fGridButtonHeight + (uiRows - 1) * s_fGridGap + 6.0f;
}

float plEngineModelingView::BuildCommandGrid(plUiElement* pSection, plUInt32 uiGroup, float y)
{
  const plModelingPanelContent::CommandGroup& group = plModelingPanelContent::GetCommandGroups()[uiGroup];

  y += 4.0f;

  for (plUInt32 i = 0; i < group.m_Commands.GetCount(); ++i)
  {
    const plModelingCommand::Enum cmd = group.m_Commands[i];
    const float x = s_fSide + (i % plModelingPanelContent::IconColumns) * (s_fGridButtonWidth + s_fGridGap);
    const float fRow = y + (i / plModelingPanelContent::IconColumns) * (s_fGridButtonHeight + s_fGridGap);

    // Edit Pivot is a toggle: lit while the pivot gizmo is up
    const bool bChecked = cmd == plModelingCommand::EditPivot && m_pPanel->IsPivotEditActive();
    const char* szIcon = plModelingPanelContent::GetCommandIcon(cmd);

    BuildGridButton(pSection, Name("cmd", cmd), plModelingPanelContent::GetLineIconPath(szIcon), plModelingPanelContent::GetCommandTooltip(cmd), x, fRow, m_pPanel->IsCommandEnabled(cmd), bChecked);
  }

  const plUInt32 uiRows = (group.m_Commands.GetCount() + plModelingPanelContent::IconColumns - 1) / plModelingPanelContent::IconColumns;
  return y + uiRows * s_fGridButtonHeight + (uiRows - 1) * s_fGridGap + 6.0f;
}

float plEngineModelingView::BuildSelectToggles(plUiElement* pSection, float y)
{
  using namespace plEditorUi;

  auto& board = GetSurface()->GetBlackboard();
  const float fWidth = m_fContentWidth - 2 * s_fSide;

  // what a click takes in Face mode: whole faces as rendered, or single polygons of the brush's mesh
  {
    y += 2.0f;

    const float fLabel = plMath::Ceil(MeasureText("Faces", s_uiLabelSize)) + 2.0f;
    AddText(pSection, "Faces", s_fSide, y, fLabel, ControlHeight - 2.0f, s_uiLabelSize, Muted());

    const plString sKey = plBrushActions::GetShortcutText(plModelingCommand::ToggleFaceGranularity);
    plStringBuilder sKeyHint;
    if (!sKey.IsEmpty())
      sKeyHint.SetFormat(" ({} switches)", sKey);

    const char* szLabels[] = {"CSG", "Mesh"};
    const char* szTooltips[] = {"A click selects a whole face as rendered: every piece cutters split it into, and the faces cutters leave behind",
      "A click selects one polygon of the brush's own mesh"};

    float x = s_fSide + fLabel + 8.0f;
    for (plUInt32 i = 0; i < 2; ++i)
    {
      const plString sName = Name("faces", i);
      const float fButton = TextButtonWidth(szLabels[i]);
      AddTextButton(pSection, szLabels[i], sName, x, y, fButton, ControlHeight - 2.0f, m_pPanel->GetFaceGranularity() == static_cast<plInt32>(i), true);
      SetToolTip(board, sName, plStringBuilder(szTooltips[i], sKeyHint));
      x += fButton + 4.0f;
    }

    y += ControlHeight + 4.0f;
  }

  {
    // the step is the editor's translation snap, so it follows the toolbar's snap setting
    plStringBuilder sLabel;
    sLabel.SetFormat("Snap to grid ({} m steps: drawing, moves, extrude, pivot, clip)", plArgF(plBrushToolSettings::GetGridStep(), 3));

    const plString sKey = plBrushActions::GetShortcutText(plModelingCommand::ToggleGridSnap);
    if (!sKey.IsEmpty())
      sLabel.AppendFormat(" - {}", sKey);

    y += BuildCheckBox(pSection, "Mo.grid", sLabel, plBrushToolSettings::GetSnapToGrid(), s_fSide, y, fWidth) + 4.0f;
  }

  y += BuildCheckBox(pSection, "Mo.through", "Select through (box selection includes hidden sides)", m_pPanel->GetSelectThrough(), s_fSide, y, fWidth) + 4.0f;
  y += BuildCheckBox(pSection, "Mo.hidden", "Show hidden handles (boxes behind the brush stay visible)", m_pPanel->GetShowHiddenHandles(), s_fSide, y, fWidth) + 4.0f;

  return y + 2.0f;
}

float plEngineModelingView::BuildCheckBox(plUiElement* pParent, plStringView sName, plStringView sLabel, bool bChecked, float x, float y, float fWidth)
{
  using namespace plEditorUi;

  constexpr float fBox = 14.0f;
  constexpr float fTextX = fBox + 8.0f;

  plHybridArray<plString, 4> lines;
  WrapLines(sLabel, s_uiLabelSize, fWidth - fTextX, lines);
  const float fHeight = plMath::Max(18.0f, lines.GetCount() * s_fHintLine + 2.0f);

  plUiButton* pButton = AddGhostButton(pParent, sName, x, y, fWidth, fHeight);

  // the box: a field well, or the selection fill with an accent edge and tick while checked
  plUiPanel* pBox = Panel(pButton);
  Place(pBox, 0.0f, 2.0f, fBox, fBox);
  pBox->m_Color = bChecked ? Selection() : Field();
  pBox->m_fBorderWidth = 1.0f;
  pBox->m_BorderColor = bChecked ? Mix(Selection(), Accent(), 0.55f) : Border();
  pBox->m_vCornerRadius = plVec4(3);

  if (bChecked)
  {
    plUiImage* pTick = SvgImage(pBox, IconPath(Icon::Check));
    Fill(pTick, plVec4(1));
    pTick->m_Color = Accent();
  }

  for (plUInt32 i = 0; i < lines.GetCount(); ++i)
    AddText(pButton, lines[i], fTextX, 1.0f + i * s_fHintLine, fWidth - fTextX, s_fHintLine, s_uiLabelSize, Muted());

  return fHeight;
}

float plEngineModelingView::BuildHint(plUiElement* pParent, plStringView sText, float x, float y, float fWidth)
{
  plHybridArray<plString, 8> lines;
  WrapLines(sText, s_uiHintSize, fWidth, lines);

  for (plUInt32 i = 0; i < lines.GetCount(); ++i)
    AddText(pParent, lines[i], x, y + i * s_fHintLine, fWidth, s_fHintLine, s_uiHintSize, plEditorUi::Dim());

  return lines.GetCount() * s_fHintLine;
}

float plEngineModelingView::BuildTexture(plUiElement* pSection, float y)
{
  using namespace plEditorUi;

  auto& board = GetSurface()->GetBlackboard();
  const plModelingTextureSettings& settings = m_pPanel->GetTextureSettings();

  const float fFieldX = s_fSide + s_fLabelWidth + 10.0f;
  const float fFieldWidth = m_fContentWidth - fFieldX - s_fSide;
  const float fNumberWidth = plMath::Min(s_fNumberWidth, plMath::Floor((fFieldWidth - 4.0f) * 0.5f));
  constexpr float fRowGap = 6.0f;

  auto label = [&](plStringView sText)
  { AddText(pSection, sText, s_fSide, y, s_fLabelWidth, ControlHeight, s_uiLabelSize, Muted()); };

  auto numberField = [&](plUInt32 uiField, float x)
  {
    const TextureField& field = s_TextureFields[uiField];
    const plString sName = Name("texfield", uiField);

    // a field being edited keeps what the user typed
    if (m_iFocusedField != static_cast<plInt32>(uiField))
    {
      plModelingTextureSettings values = settings;
      board.SetEntryValue(FieldBinding(uiField), FormatField(uiField, FieldValue(values, uiField)));
    }

    NumberField(pSection, plRectFloat(x, y, fNumberWidth, ControlHeight), sName, FieldBinding(uiField), Name("texsubmit", uiField), field.m_szLetter, field.m_szUnit);
    SetToolTip(board, sName, field.m_szTooltip);
  };

  y += 8.0f;

  // Alignment: a field well with the choice right-aligned before a chevron
  {
    label("Alignment");

    plUiButton* pChoice = Button(pSection, {}, "Mo.tex.align");
    pChoice->ClearChildren();
    Place(pChoice, fFieldX, y, fFieldWidth, ControlHeight);
    pChoice->m_Color = Field();
    pChoice->m_HoverColor = Field();
    pChoice->m_PressedColor = Field();
    pChoice->m_fBorderWidth = 1.0f;
    pChoice->m_BorderColor = Line();
    pChoice->m_vCornerRadius = plVec4(4);

    plUiText* pValue = AddText(pChoice, settings.m_bWorldAligned ? "World (seamless)" : "Face", 8.0f, 0.0f, fFieldWidth - 8.0f - 27.0f, ControlHeight, s_uiLabelSize, Foreground());
    pValue->m_HAlign = plUiTextHAlign::Right;

    plUiImage* pChevron = SvgImage(pChoice, IconPath(Icon::Expand));
    Place(pChevron, fFieldWidth - 21.0f, (ControlHeight - 13.0f) * 0.5f, 13.0f, 13.0f);
    pChevron->m_Color = Dim();

    SetToolTip(board, "Mo.tex.align", "World: every face projects from the nearest axis, so neighbouring faces tile seamlessly. Face: the texture follows the face's own orientation.");
    y += ControlHeight + fRowGap;
  }

  label("Tile size");
  numberField(0, fFieldX);
  numberField(1, fFieldX + fNumberWidth + 4.0f);
  y += ControlHeight + fRowGap;

  label("Offset");
  numberField(2, fFieldX);
  numberField(3, fFieldX + fNumberWidth + 4.0f);
  y += ControlHeight + fRowGap;

  label("Rotation");
  numberField(4, fFieldX);
  y += ControlHeight + fRowGap;

  // Justify: borderless icon buttons
  {
    label("Justify");

    const auto modes = plModelingPanelContent::GetJustifyModes();
    for (plUInt32 i = 0; i < modes.GetCount(); ++i)
    {
      const plString sName = Name("justify", i);
      plUiButton* pButton = Button(pSection, {}, sName);
      pButton->ClearChildren();
      Place(pButton, fFieldX + i * (ControlHeight + 2.0f), y, ControlHeight, ControlHeight);
      pButton->m_Color = plColorGammaUB(0, 0, 0, 0);
      pButton->m_vCornerRadius = plVec4(4);

      plUiImage* pIcon = ButtonIcon(pButton, plModelingPanelContent::GetLineIconPath(modes[i].m_szIcon));
      pIcon->m_Color = Muted();
      SetToolTip(board, sName, modes[i].m_szTooltip);
    }

    y += ControlHeight + fRowGap;
  }

  // Copy: Pick and Apply, and Reset projection beside them while it fits
  {
    label("Copy");

    struct CopyButton
    {
      const char* m_szLabel;
      const char* m_szName;
      const char* m_szTooltip;
    };

    const CopyButton buttons[] = {
      {"Pick", "Mo.tex.pick", "Pick up the first selected face's material and texture settings, to apply them to other faces."},
      {"Apply", "Mo.tex.apply", "Give the selected faces the material and texture settings picked up with Pick."},
      {"Reset projection", "Mo.tex.reset", "Back to world alignment, 1 m tiles, no offset or rotation. The material stays."},
    };

    float x = fFieldX;
    for (const CopyButton& button : buttons)
    {
      const float fButton = TextButtonWidth(button.m_szLabel);
      if (x > fFieldX && x + fButton > fFieldX + fFieldWidth)
      {
        x = fFieldX;
        y += ControlHeight + fRowGap;
      }

      AddTextButton(pSection, button.m_szLabel, button.m_szName, x, y, fButton, ControlHeight, false, true);
      SetToolTip(board, button.m_szName, button.m_szTooltip);
      x += fButton + 4.0f;
    }

    y += ControlHeight + fRowGap;
  }

  y += BuildCheckBox(pSection, "Mo.tex.hard", "Hard edges (never smoothed)", settings.m_bHardEdges, fFieldX, y + 3.0f, fFieldWidth) + 3.0f;
  y += 8.0f;

  y += BuildHint(pSection, m_pPanel->GetTextureInfo(), s_fSide, y, m_fContentWidth - 2 * s_fSide);
  return y + 8.0f;
}

float plEngineModelingView::BuildTextureEmpty(plUiElement* pSection, float y)
{
  using namespace plEditorUi;

  // empty state: an icon and one line, centred
  y += 12.0f;

  plUiImage* pIcon = SvgImage(pSection, plModelingPanelContent::GetLineIconPath("ModeFace"));
  Place(pIcon, plMath::Round((m_fContentWidth - 22.0f) * 0.5f), y, 22.0f, 22.0f);
  pIcon->m_Color = Dim();
  y += 22.0f + 8.0f;

  plHybridArray<plString, 4> lines;
  WrapLines(m_pPanel->GetTextureInfo(), FontSize, m_fContentWidth - 40.0f, lines);

  for (const plString& sLine : lines)
  {
    plUiText* pText = AddText(pSection, sLine, 20.0f, y, m_fContentWidth - 40.0f, 18.0f, FontSize, Muted());
    pText->m_HAlign = plUiTextHAlign::Center;
    y += 18.0f;
  }

  return y + 12.0f;
}

float plEngineModelingView::BuildOutput(plUiElement* pSection, float y)
{
  using namespace plEditorUi;

  const auto outputs = plModelingPanelContent::GetOutputs();
  const float fGap = 4.0f;
  const float fWidth = (m_fContentWidth - 2 * s_fSide - fGap) * 0.5f;

  y += 4.0f;

  for (plUInt32 i = 0; i < outputs.GetCount(); ++i)
  {
    const plString sName = Name("out", i);
    AddTextButton(pSection, outputs[i].m_szLabel, sName, s_fSide + (i % 2) * (fWidth + fGap), y + (i / 2) * (ControlHeight + fGap), fWidth, ControlHeight, false, false);
    SetToolTip(GetSurface()->GetBlackboard(), sName, outputs[i].m_szTooltip);
  }

  const plUInt32 uiRows = (outputs.GetCount() + 1) / 2;
  y += uiRows * ControlHeight + (uiRows - 1) * fGap + 8.0f;

  y += BuildHint(pSection, plModelingPanelContent::GetOutputHint(), s_fSide, y, m_fContentWidth - 2 * s_fSide);
  return y + 8.0f;
}

float plEngineModelingView::BuildHealth(plUiElement* pSection, float y)
{
  using namespace plEditorUi;

  auto& board = GetSurface()->GetBlackboard();
  const plDynamicArray<plModelingHealthIssue>& issues = m_pPanel->GetHealthIssues();

  if (issues.IsEmpty())
  {
    y += 2.0f;
    y += BuildHint(pSection, "No problems found in this scene's brushes.", s_fSide, y, m_fContentWidth - 2 * s_fSide);
    return y + 8.0f;
  }

  constexpr float fRowX = 6.0f;
  const float fRowWidth = m_fContentWidth - 2 * fRowX;

  y += 2.0f;

  for (plUInt32 i = 0; i < issues.GetCount(); ++i)
  {
    const plModelingHealthIssue& issue = issues[i];

    const float fFix = issue.m_sFix.IsEmpty() ? 0.0f : TextButtonWidth(issue.m_sFix);
    const float fTextX = fRowX + 6.0f + 14.0f + 8.0f;
    const float fTextWidth = fRowX + fRowWidth - 6.0f - (fFix > 0.0f ? fFix + 8.0f : 0.0f) - fTextX;

    plHybridArray<plString, 4> lines;
    WrapLines(issue.m_sText, s_uiLabelSize, fTextWidth, lines);
    const float fHeight = plMath::Max(24.0f, lines.GetCount() * s_fHintLine + 8.0f);

    // a log row in the warning colour; a click selects the brush
    const plString sRow = Name("issue", i);
    plUiButton* pRow = AddGhostButton(pSection, sRow, fRowX, y, fRowWidth, fHeight);
    pRow->m_HoverColor = Hover();
    pRow->m_vCornerRadius = plVec4(3);
    SetToolTip(board, sRow, "Select this brush");

    plUiImage* pIcon = SvgImage(pRow, IconPath(Icon::Warning));
    Place(pIcon, 6.0f, 5.0f, 14.0f, 14.0f);
    pIcon->m_Color = Warning();

    for (plUInt32 l = 0; l < lines.GetCount(); ++l)
      AddText(pRow, lines[l], fTextX - fRowX, 4.0f + l * s_fHintLine, fTextWidth, s_fHintLine, s_uiLabelSize, Warning());

    if (fFix > 0.0f)
      AddTextButton(pSection, issue.m_sFix, Name("fix", i), fRowX + fRowWidth - 6.0f - fFix, y + (fHeight - 20.0f) * 0.5f, fFix, 20.0f, false, true);

    y += fHeight + 2.0f;
  }

  return y + 6.0f;
}

// Input.

void plEngineModelingView::OnUiEvent(const plMsgUiEvent& event)
{
  const plStringView sId = event.m_sIdentifier.GetView();
  plUInt32 uiIndex = 0;

  if (ParseIndex(sId, "Mo.section.", uiIndex) && uiIndex < SectionCount)
  {
    SetSectionOpen(static_cast<Section>(uiIndex), !m_SectionOpen[uiIndex]);
    return;
  }

  if (ParseIndex(sId, "Mo.mode.", uiIndex) && uiIndex < 4)
  {
    m_pPanel->PickMode(static_cast<plInt32>(uiIndex));
    return;
  }

  if (ParseIndex(sId, "Mo.shape.", uiIndex) && uiIndex < plModelingPanelContent::GetShapes().GetCount())
  {
    m_pPanel->PickShape(plModelingPanelContent::GetShapes()[uiIndex].m_iShape);
    return;
  }

  if (ParseIndex(sId, "Mo.cmd.", uiIndex) && uiIndex < plModelingCommand::Count)
  {
    const plModelingCommand::Enum cmd = static_cast<plModelingCommand::Enum>(uiIndex);
    if (m_pPanel->IsCommandEnabled(cmd))
      m_pPanel->RunCommand(cmd);
    return;
  }

  if (ParseIndex(sId, "Mo.faces.", uiIndex) && uiIndex < 2)
  {
    m_pPanel->SetFaceGranularity(static_cast<plInt32>(uiIndex));
    return;
  }

  if (sId == "Mo.grid")
  {
    m_pPanel->SetSnapToGrid(!plBrushToolSettings::GetSnapToGrid());
    return;
  }

  if (sId == "Mo.through")
  {
    m_pPanel->SetSelectThrough(!m_pPanel->GetSelectThrough());
    return;
  }

  if (sId == "Mo.hidden")
  {
    m_pPanel->SetShowHiddenHandles(!m_pPanel->GetShowHiddenHandles());
    return;
  }

  if (sId == "Mo.tex.align")
  {
    OpenAlignmentMenu();
    return;
  }

  if (ParseIndex(sId, "Mo.texsubmit.", uiIndex) && uiIndex < PL_ARRAY_SIZE(s_TextureFields))
  {
    CommitTextureField(uiIndex);
    return;
  }

  if (ParseIndex(sId, "Mo.justify.", uiIndex) && uiIndex < plModelingPanelContent::GetJustifyModes().GetCount())
  {
    m_pPanel->Justify(plModelingPanelContent::GetJustifyModes()[uiIndex].m_cMode);
    return;
  }

  if (sId == "Mo.tex.pick")
  {
    m_pPanel->SampleFace();
    return;
  }

  if (sId == "Mo.tex.apply")
  {
    m_pPanel->ApplySample();
    return;
  }

  if (sId == "Mo.tex.reset")
  {
    m_pPanel->ResetTexture();
    return;
  }

  if (sId == "Mo.tex.hard")
  {
    ToggleHardEdges();
    return;
  }

  if (ParseIndex(sId, "Mo.out.", uiIndex))
  {
    m_pPanel->RunOutput(uiIndex);
    return;
  }

  if (ParseIndex(sId, "Mo.issue.", uiIndex))
  {
    m_pPanel->SelectHealthIssue(uiIndex);
    return;
  }

  if (ParseIndex(sId, "Mo.fix.", uiIndex))
  {
    m_pPanel->FixHealthIssue(uiIndex);
    return;
  }
}

void plEngineModelingView::OpenAlignmentMenu()
{
  plEditorUiMenu menu;
  const bool bWorld = m_pPanel->GetTextureSettings().m_bWorldAligned;

  plEditorUiMenuItem& world = menu.AddAction("World (seamless)", 0);
  world.m_bCheckable = true;
  world.m_bChecked = bWorld;

  plEditorUiMenuItem& face = menu.AddAction("Face", 1);
  face.m_bCheckable = true;
  face.m_bChecked = !bWorld;

  // under the field
  plDynamicArray<plUiFocusable> focusables;
  GetSurface()->GetCanvas().CollectFocusables(GetSurface()->GetLogicalSize(), focusables);

  plVec2 vPosition = plVec2::MakeZero();
  for (const plUiFocusable& focusable : focusables)
  {
    if (focusable.m_pElement->m_sName == "Mo.tex.align")
      vPosition = plVec2(focusable.m_Rect.x, focusable.m_Rect.y + focusable.m_Rect.height);
  }

  plEditorUiMenuView::Show(*GetSurface(), vPosition, menu, [this](plUInt32 uiChoice)
    {
      plModelingTextureSettings settings = m_pPanel->GetTextureSettings();
      const bool bPickedWorld = uiChoice == 0;
      if (!m_pPanel->HasTextureSelection() || settings.m_bWorldAligned == bPickedWorld)
        return;

      settings.m_bWorldAligned = bPickedWorld;
      m_pPanel->ApplyTextureSettings(settings);
    });
}

void plEngineModelingView::ToggleHardEdges()
{
  if (!m_pPanel->HasTextureSelection())
    return;

  plModelingTextureSettings settings = m_pPanel->GetTextureSettings();
  settings.m_bHardEdges = !settings.m_bHardEdges;
  m_pPanel->ApplyTextureSettings(settings);
}

void plEngineModelingView::CommitTextureField(plUInt32 uiField)
{
  if (!m_pPanel->HasTextureSelection())
    return;

  auto& board = GetSurface()->GetBlackboard();
  const plString sBinding = FieldBinding(uiField);
  const plString sText = board.GetEntryValue(plTempHashedString(sBinding), plString()).ConvertTo<plString>();

  plModelingTextureSettings settings = m_pPanel->GetTextureSettings();
  double& fValue = FieldValue(settings, uiField);

  // unchanged text commits nothing, as an untouched spin box
  if (sText == FormatField(uiField, fValue))
    return;

  double fParsed = 0.0;
  if (plConversionUtils::StringToFloat(sText, fParsed).Failed())
  {
    board.SetEntryValue(sBinding, FormatField(uiField, fValue));
    return;
  }

  fValue = plMath::Clamp(fParsed, s_TextureFields[uiField].m_fMin, s_TextureFields[uiField].m_fMax);
  m_pPanel->ApplyTextureSettings(settings);

  // the field shows the value as stored
  plModelingTextureSettings stored = m_pPanel->GetTextureSettings();
  board.SetEntryValue(sBinding, FormatField(uiField, FieldValue(stored, uiField)));
}

void plEngineModelingView::CommitFocusedFieldOnFocusChange()
{
  plInt32 iFocused = -1;
  const plStringView sFocused = GetSurface()->GetCanvas().GetFocusedElementName();

  for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_TextureFields) && !sFocused.IsEmpty(); ++i)
  {
    if (sFocused == Name("texfield", i))
      iFocused = static_cast<plInt32>(i);
  }

  if (iFocused == m_iFocusedField)
    return;

  // a field commits when focus leaves it, as when Enter is pressed
  const plInt32 iLeft = m_iFocusedField;
  m_iFocusedField = iFocused;

  if (iLeft >= 0)
    CommitTextureField(static_cast<plUInt32>(iLeft));
}


PL_STATICLINK_FILE(EditorPluginModeling, EditorPluginModeling_Brush_EngineModelingPanel);
