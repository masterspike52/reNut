#include "native_loader.h"

#include <ranges>

#include <rexfabric/midasm.h>
#include <rexfabric/rexfabric.h>

#include "events.h"
#include "hooks.h"
#include "log.h"

namespace rexfabric {

void Detach(const RexFabricMod* mod) {
  Hooks::Get().RemoveAll(mod);
  events::UnlistenAll(mod);
  ReleaseAllMidasm(mod);
}

void NativeLoader::Load(const std::vector<ModInfo>& mods) {
  for (const ModInfo& mod : mods) {
    for (const std::filesystem::path& file : NativePlugins(mod.dir)) LoadPlugin(mod, file);
  }
}

void NativeLoader::LoadPlugin(const ModInfo& owner, const std::filesystem::path& file) {
  auto mod = std::make_unique<RexFabricMod>(RexFabricMod{owner.folder + "/" + file.filename().string()});
  std::string error;
  std::unique_ptr<Library> library = Library::Open(file, error);
  if (!library) {
    RF_ERROR("[{}] failed to load: {}", mod->name, error);
    return;
  }
  auto init = library->Function<RexFabricModInitFn>(REXFABRIC_MOD_INIT_NAME);
  auto shutdown = library->Function<RexFabricModShutdownFn>(REXFABRIC_MOD_SHUTDOWN_NAME);
  if (!init) {
    RF_INFO("[{}] loaded (no {}, so it sets itself up)", mod->name, REXFABRIC_MOD_INIT_NAME);
  } else if (!init(&Api(), mod.get())) {
    RF_WARN("[{}] declined to load", mod->name);
    Detach(mod.get());
    return;
  } else {
    RF_INFO("[{}] loaded", mod->name);
  }
  plugins_.push_back({std::move(mod), std::move(library), shutdown});
}

void NativeLoader::Unload() {
  for (Plugin& plugin : plugins_ | std::views::reverse) {
    Detach(plugin.mod.get());
    if (plugin.shutdown) plugin.shutdown();
    // Left mapped on purpose: a guest thread may still be returning through one of its hooks, and this only
    // runs as the process exits.
    (void)plugin.library.release();
    (void)plugin.mod.release();
  }
  plugins_.clear();
}

}  // namespace rexfabric
