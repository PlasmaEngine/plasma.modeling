#pragma once

#include <EditorPluginModeling/Brush/BrushActions.h>
#include <Foundation/Types/ArrayPtr.h>

/// \brief What the Modeling panel offers, shared by its Qt and engine front ends: the shape palette, the command
/// groups with their hints, the justify modes, the output buttons and the icon of each command.
namespace plModelingPanelContent
{
  /// A shape of the Draw palette.
  struct Shape
  {
    const char* m_szLabel;
    plInt32 m_iShape; ///< plBrushShape::Enum
    const char* m_szIcon;
  };

  /// A section of command buttons: its title, its commands in grid order and the hint under them.
  struct CommandGroup
  {
    const char* m_szTitle;
    plArrayPtr<const plModelingCommand::Enum> m_Commands;
    const char* m_szHint;
  };

  /// A justify button of the Texture section.
  struct JustifyMode
  {
    char m_cMode; ///< 'L', 'R', 'T', 'B', 'C' or 'F'
    const char* m_szIcon;
    const char* m_szTooltip;
  };

  /// A button of the Output section.
  struct Output
  {
    const char* m_szLabel;
    const char* m_szIcon;
    const char* m_szTooltip;
  };

  /// Icon buttons per grid row.
  constexpr plUInt32 IconColumns = 5;

  PL_EDITORPLUGINMODELING_DLL plArrayPtr<const Shape> GetShapes();
  PL_EDITORPLUGINMODELING_DLL const char* GetDrawHint();

  /// Select, Edit, CSG and Pivot, in panel order.
  PL_EDITORPLUGINMODELING_DLL plArrayPtr<const CommandGroup> GetCommandGroups();

  PL_EDITORPLUGINMODELING_DLL plArrayPtr<const JustifyMode> GetJustifyModes();

  PL_EDITORPLUGINMODELING_DLL plArrayPtr<const Output> GetOutputs();
  PL_EDITORPLUGINMODELING_DLL const char* GetOutputHint();

  /// The base name of a command's panel icon, or nullptr. The Qt panel shows ":/ModelingIcons/Tools/<name>.svg", the
  /// engine UI the line icon ":/ModelingIcons/Line/<name>.svg".
  PL_EDITORPLUGINMODELING_DLL const char* GetCommandIcon(plModelingCommand::Enum cmd);

  /// The engine UI's line icon of that base name.
  PL_EDITORPLUGINMODELING_DLL plString GetLineIconPath(plStringView sName);

  /// "Name (Key)" with the command's current key, or just the name when it has none.
  PL_EDITORPLUGINMODELING_DLL plString GetCommandTooltip(plModelingCommand::Enum cmd);
} // namespace plModelingPanelContent
