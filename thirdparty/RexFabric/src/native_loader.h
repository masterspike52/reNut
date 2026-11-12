// Loads native mods (.asi, .dll, .so) and runs their RexFabricModInit.

#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include <rexfabric/api.h>

#include "library.h"
#include "mod.h"
#include "mods.h"

namespace rexfabric {

class NativeLoader {
 public:
  void Load(const std::vector<ModInfo>& mods);
  // Runs each plugin's shutdown, last loaded first, and removes everything it attached.
  void Unload();

 private:
  struct Plugin {
    std::unique_ptr<RexFabricMod> mod;
    std::unique_ptr<Library> library;
    RexFabricModShutdownFn shutdown;
  };

  void LoadPlugin(const ModInfo& owner, const std::filesystem::path& file);

  std::vector<Plugin> plugins_;
};

// Removes the hooks, listeners and midasm claims a mod made.
void Detach(const RexFabricMod* mod);

}  // namespace rexfabric
