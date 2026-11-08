#pragma once

#include <EditorPluginModeling/EditorPluginModelingDLL.h>
#include <EditorUi/Content.h>
#include <Foundation/Types/UniquePtr.h>

class plModelingPanel;
class plUiElement;
class plUiButton;
struct plMsgUiEvent;

/// \brief The Modeling panel's content, drawn by the engine UI: collapsible sections with five-column icon grids
/// (Draw, Select, Edit, CSG, Pivot), the mode buttons, the texture settings of the selected faces, the output buttons
/// and the health issues.
///
/// It shows the state its plModelingPanel holds and calls the panel for everything it does, so buttons enable by mode
/// and selection exactly as the panel decides. A number field commits on Enter or when focus leaves it, one undo step
/// per committed edit.
///
/// The view is a panel of the scene's frame in the engine shell; it owns the panel.
class PL_EDITORPLUGINMODELING_DLL plEngineModelingView : public plEditorUiContent
{
public:
  explicit plEngineModelingView(plUniquePtr<plModelingPanel>&& pPanel);
  ~plEngineModelingView() override;

  /// The panel's state changed: the view is rebuilt on its next update.
  void Invalidate() { m_bRebuild = true; }

  /// The sections, in panel order.
  enum Section : plUInt8
  {
    Mode,
    Draw,
    Select,
    Edit,
    Csg,
    Pivot,
    Texture,
    Output,
    Health,
    SectionCount
  };

  bool IsSectionOpen(Section section) const { return m_SectionOpen[section]; }
  void SetSectionOpen(Section section, bool bOpen);

protected:
  void OnUiUpdate() override;
  void OnUiEvent(const plMsgUiEvent& event) override;

private:
  void BuildDocument();
  float BuildSection(plUiElement* pScroll, Section section, const char* szTitle);
  float BuildHeader(plUiElement* pSection, Section section, const char* szTitle);
  float BuildModeButtons(plUiElement* pSection, float y);
  float BuildShapeGrid(plUiElement* pSection, float y);
  float BuildCommandGrid(plUiElement* pSection, plUInt32 uiGroup, float y);
  float BuildSelectToggles(plUiElement* pSection, float y);
  float BuildTexture(plUiElement* pSection, float y);
  float BuildTextureEmpty(plUiElement* pSection, float y);
  float BuildOutput(plUiElement* pSection, float y);
  float BuildHealth(plUiElement* pSection, float y);

  /// Wrapped dim lines; returns their height.
  float BuildHint(plUiElement* pParent, plStringView sText, float x, float y, float fWidth);

  /// A check box and its label as one button; returns its height.
  float BuildCheckBox(plUiElement* pParent, plStringView sName, plStringView sLabel, bool bChecked, float x, float y, float fWidth);

  /// A 32 x 28 icon button of a grid, dimmed and inert while disabled, the accent icon on a plate while checked.
  plUiButton* BuildGridButton(plUiElement* pParent, plStringView sName, plStringView sIcon, plStringView sTooltip, float x, float y, bool bEnabled, bool bChecked);

  void OpenAlignmentMenu();
  void CommitTextureField(plUInt32 uiField);
  void CommitFocusedFieldOnFocusChange();
  void ToggleHardEdges();

  plUniquePtr<plModelingPanel> m_pPanel;
  bool m_SectionOpen[SectionCount];
  plInt32 m_iFocusedField = -1; ///< the texture field being edited; it commits when focus leaves it
  float m_fBuiltWidth = -1.0f;
  float m_fContentWidth = 0.0f;
  bool m_bRebuild = true;
};
