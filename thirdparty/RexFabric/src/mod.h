// What a RexFabricMod handle points at: anything that owns hooks and writes log lines, be it a native
// plugin, a script or the host itself.

#pragma once

#include <string>

struct RexFabricMod {
  std::string name;
};
