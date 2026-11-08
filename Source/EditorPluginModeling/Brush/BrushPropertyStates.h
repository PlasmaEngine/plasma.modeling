#pragma once

#include <EditorPluginModeling/EditorPluginModelingDLL.h>

/// \brief Hides brush properties that do nothing in the current configuration: generator parameters
/// the shape does not use, all generator parameters once the brush is Editable, and every output and
/// surface setting on cutters (they never render in a game or collide).
class PL_EDITORPLUGINMODELING_DLL plBrushPropertyStates
{
public:
  static void Startup();
  static void Shutdown();
};
