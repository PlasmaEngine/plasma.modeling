#include <ModelingPlugin/ModelingPluginPCH.h>

#include <Foundation/Configuration/Startup.h>

// clang-format off
PL_BEGIN_SUBSYSTEM_DECLARATION(GameEngine, ModelingPlugin)

  BEGIN_SUBSYSTEM_DEPENDENCIES
    "Foundation",
    "Core"
  END_SUBSYSTEM_DEPENDENCIES

  ON_CORESYSTEMS_STARTUP
  {
  }

  ON_CORESYSTEMS_SHUTDOWN
  {
  }

PL_END_SUBSYSTEM_DECLARATION;
// clang-format on

PL_STATICLINK_FILE(ModelingPlugin, ModelingPlugin_Startup);
