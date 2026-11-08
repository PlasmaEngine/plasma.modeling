#pragma once

#include <EditorPluginModeling/Brush/BrushActions.h>
#include <EditorPluginModeling/Brush/BrushDocumentFixup.h>
#include <EditorPluginModeling/EditorPluginModelingDLL.h>
#include <EditorUi/Runtime.h>
#include <Foundation/Types/UniquePtr.h>
#include <ModelingPlugin/Modeling/BrushFaceAttributes.h>
#include <ToolsFoundation/Document/DocumentManager.h>

#include <functional>

class plDocument;
class plDocumentObject;
class plEditorUiDocumentFrame;
class plEngineModelingView;
class plBrushToolContext;

struct plGameObjectEvent;
struct plBrushModeEvent;
struct plBrushSelectionEvent;
struct plDocumentObjectStructureEvent;
struct plDocumentObjectPropertyEvent;
struct plSelectionManagerEvent;

/// \brief The texture settings of the selected faces, as the Texture section shows and edits them.
struct plModelingTextureSettings
{
  bool m_bWorldAligned = true;
  double m_fTileU = 1.0;
  double m_fTileV = 1.0;
  double m_fOffsetU = 0.0;
  double m_fOffsetV = 0.0;
  double m_fRotation = 0.0; ///< degrees
  bool m_bHardEdges = false;
};

/// \brief A problem the Health section lists for one of the scene's brushes, with its fix when there is one.
struct plModelingHealthIssue
{
  plString m_sText;
  plUuid m_BrushObject; ///< the brush's game object, which a click on the issue selects
  plUuid m_Brush;       ///< the brush component the fix works on
  plString m_sFix;      ///< the fix button's label; empty when there is no fix
  std::function<plStatus(const plDocumentObject*)> m_Fix;
};

/// \brief The Modeling dock panel. Sections: Mode, Draw (shape palette), Select, Edit (polygon operations), CSG,
/// Pivot, Texture (per-face UV settings), Output (export and conversion) and Health (problems in the scene's brushes,
/// each with a fix). Every button runs the same modeling command as its key.
///
/// The panel holds what the sections show (modes, enabled commands, texture settings, health issues) and runs what
/// they do. The engine UI draws them (plEngineModelingView, which owns the panel).
///
/// A tab beside Properties in the scene's frame of the engine shell: the panel manager adds the view to every scene's
/// frame when the frame is made.
class PL_EDITORPLUGINMODELING_DLL plModelingPanel
{
  PL_DISALLOW_COPY_AND_ASSIGN(plModelingPanel);

public:
  plModelingPanel(plDocument* pDocument);
  ~plModelingPanel();

  /// \brief Unhooks from the document. Called when the document starts closing, which can be before the frame destroys
  /// the view and with it the panel.
  void DetachFromDocument();

  /// \brief The view that shows the panel's state; nullptr while none does.
  void SetView(plEngineModelingView* pView) { m_pView = pView; }

  // What the sections show

  /// The scene, or nullptr once the panel is detached.
  plDocument* GetSceneDocument() const { return m_pDocument; }
  plBrushToolContext* GetBrushContext() const;

  /// The sub-mode the Mode buttons mark (plBrushSubMode), -1 while the Brush tool is inactive.
  plInt32 GetCheckedMode() const { return m_iCheckedMode; }

  /// The shape the Draw palette has armed (plBrushShape), -1 for none.
  plInt32 GetArmedShape() const { return m_iArmedShape; }

  bool IsCommandEnabled(plModelingCommand::Enum cmd) const { return m_CommandEnabled[cmd]; }
  bool IsPivotEditActive() const { return m_bPivotEditActive; }
  bool GetSelectThrough() const { return m_bSelectThrough; }
  bool GetShowHiddenHandles() const { return m_bShowHiddenHandles; }
  plInt32 GetFaceGranularity() const { return m_iFaceGranularity; }

  /// Face mode with faces selected: the Texture section shows and edits their settings.
  bool HasTextureSelection() const { return m_bTextureAvailable; }
  const plModelingTextureSettings& GetTextureSettings() const { return m_TextureSettings; }

  /// The line under the texture settings, or the text shown instead of them.
  const plString& GetTextureInfo() const { return m_sTextureInfo; }

  const plDynamicArray<plModelingHealthIssue>& GetHealthIssues() const { return m_HealthIssues; }

  // What the sections do

  void PickMode(plInt32 iMode);
  void PickShape(plInt32 iShape);
  void RunCommand(plModelingCommand::Enum cmd);
  void SetFaceGranularity(plInt32 iGranularity);
  void SetSelectThrough(bool bEnable);
  void SetShowHiddenHandles(bool bEnable);
  void SetSnapToGrid(bool bEnable);

  /// \brief Gives the selected faces these settings, as one "Edit Face Texturing" undo step.
  void ApplyTextureSettings(const plModelingTextureSettings& settings);
  void Justify(char cMode);
  void SampleFace();
  void ApplySample();
  void ResetTexture();

  /// \brief Runs an Output button (plModelingPanelContent::GetOutputs) and shows its result in the status bar for 8 s.
  void RunOutput(plUInt32 uiIndex);

  /// \brief Selects the game object of the brush an issue is about.
  void SelectHealthIssue(plUInt32 uiIndex);
  void FixHealthIssue(plUInt32 uiIndex);

private:
  void OnRefreshHealth();

  // state
  void DocumentManagerEventHandler(const plDocumentManager::Event& e);
  void GameObjectEventHandler(const plGameObjectEvent& e);
  void BrushModeEventHandler(const plBrushModeEvent& e);
  void BrushSelectionEventHandler(const plBrushSelectionEvent& e);
  void SnapToGridEventHandler(bool bSnap);
  void StructureEventHandler(const plDocumentObjectStructureEvent& e);
  void PropertyEventHandler(const plDocumentObjectPropertyEvent& e);
  void SelectionEventHandler(const plSelectionManagerEvent& e);
  void UncheckAllButtons();
  void SetCheckedMode(int iMode); ///< -1 = none (tool inactive)
  void SyncToggles(const plBrushToolContext* pContext);
  void UpdateEnabledState();
  void UpdateTextureFields();
  void ScheduleHealthRefresh();
  void CollectHealthIssues();

  /// The engine front end redraws from the panel's state.
  void InvalidateView();

  // texture helpers
  bool GetFaceSelection(const plDocumentObject*& out_pBrush, plHashSet<plUInt32>& out_faceIds) const;

  plDocument* m_pDocument = nullptr;
  plUniquePtr<plBrushDocumentFixup> m_pFixup;
  plEngineModelingView* m_pView = nullptr; ///< the view that shows the panel; it owns the panel

  plInt32 m_iCheckedMode = -1;
  plInt32 m_iArmedShape = -1;
  bool m_CommandEnabled[plModelingCommand::Count] = {};
  bool m_bPivotEditActive = false;
  bool m_bSelectThrough = false;
  bool m_bShowHiddenHandles = false;
  plInt32 m_iFaceGranularity = 0; ///< plBrushFaceGranularity::Surface, the tool's default

  bool m_bTextureAvailable = false;
  plModelingTextureSettings m_TextureSettings;
  plString m_sTextureInfo;

  bool m_bHasSample = false;
  plBrushFaceAttributes m_Sample;
  plString m_sSampleMaterial;

  plDynamicArray<plModelingHealthIssue> m_HealthIssues;
  plEditorUiTimerGroup m_Timers; ///< the health refresh, 400 ms after the last change
  plUInt32 m_uiHealthTimer = 0;
  plEventSubscriptionID m_DocumentManagerSubscription = 0;
};

/// \brief Shows a Modeling panel in the frame of every scene in the engine shell: a tab beside Properties.
class PL_EDITORPLUGINMODELING_DLL plModelingPanelManager
{
public:
  static void Startup();
  static void Shutdown();

  /// The panels that exist; they unhook from their documents at Shutdown.
  static void RegisterPanel(plModelingPanel* pPanel);
  static void UnregisterPanel(plModelingPanel* pPanel);

private:
  static void AddShellPanel(plEditorUiDocumentFrame& ref_frame);
};
