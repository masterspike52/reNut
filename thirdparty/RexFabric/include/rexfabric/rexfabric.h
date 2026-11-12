// RexFabric for the host project.
//
//   rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override { return rexfabric::Configure(settings); }
//   rex::Result<void> OnInit() override {
//     rexfabric::Setup({.thunderstore = "my-community", .menu_key = "F5", .frame_function = 0x8266FBB0});
//     return rexfabric::CreateMenus(GetUI(), {MakeTexture, FreeTexture});
//   }
//   rex::Result<void> OnTitleLaunch(rex::kernel::TitleRuntime&) override { DeclareModApi(); rexfabric::Start(); return rex::Ok(); }
//   void OnShutdown() override { rexfabric::Stop(); }
//
// Stop runs once, so it can also go where the window's close event is seen, before the title unloads.
// The game's own modding API (rexfabric/game_api.h) is declared before Start, which is when mods load.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include <rex/result.h>
#include <rexfabric/api.h>

namespace rex {
class SettingsRegistry;
}  // namespace rex

namespace rex::ui {
class IUISystem;
}  // namespace rex::ui

namespace rexfabric {

// Adds RexFabric's log category and the old names of its hotkeys. Call from OnConfigure, before the config loads.
rex::Result<void> Configure(rex::SettingsRegistry& settings);

struct Config {
  // The game's Thunderstore community, as in thunderstore.io/c/<community>. Empty: no Thunderstore, so the
  // menu has no browser.
  std::string thunderstore;
  // One folder per mod. Empty: "mods" beside the executable.
  std::filesystem::path mods_dir;
  // The key that opens the mods menu, by name ("F5"); it becomes the rebindable ui.bind.mods. Empty: no key,
  // and the game opens the menu with ShowMenu.
  std::string menu_key;
  // The key that opens the node editor, by name; it becomes the rebindable ui.bind.node_editor. Only with
  // WITH_NODE_EDITOR. Empty: no key; the mods menu still opens it.
  std::string node_editor_key;
  // A guest function the game runs once a frame, such as its present. RexFabric hooks it and fires the
  // frame event from there, on the guest thread. 0: the game calls Tick itself.
  uint32_t frame_function = 0;
  bool native_mods = true;
  bool lua_mods = true;
};

// Call once, after Configure and before anything else here.
void Setup(Config config);
const Config& GetConfig();

// Loads the enabled mods. Call after guest memory exists and before the guest runs: TitleApp::OnTitleLaunch.
void Start();
// Shuts the mods down and removes every hook. Later calls do nothing.
void Stop();
// Fires the frame event, with the seconds since the last one. Only needed without a frame_function.
void Tick();

// Pictures for the UI's ImGui, for the mods' icons: `create` uploads RGBA8 and returns its texture, or
// ImTextureID_Invalid; `destroy` frees one. Either may be called on any thread.
struct Images {
  std::function<ImTextureID(uint32_t width, uint32_t height, const uint8_t* rgba)> create;
  std::function<void(ImTextureID texture)> destroy;
};

// Makes the mods menu: a Guide blade and window when built WITH_GUIDE, plain ImGui windows otherwise, and the
// hotkeys. Call once from OnInit on the UI thread, after Setup and after the Guide is installed.
rex::Result<void> CreateMenus(rex::ui::IUISystem& ui, Images images);
void ShowMenu(bool show = true);
void ToggleMenu();
// A RexFabric window is open: the mods menu, or the node editor. For a game deciding whether mods get input.
bool MenuShown();

// The node editor, which makes Lua mods from node graphs in game. Only with WITH_NODE_EDITOR; without it these do
// nothing. Closing it asks first when a graph has unsaved changes.
void ShowNodeEditor(bool show = true);
bool NodeEditorShown();

// A mod in the mods folder, for the game to show or report.
struct InstalledMod {
  std::string folder;
  std::filesystem::path dir;
  std::string name;
  std::string version;
  std::string author;
  bool enabled = false;
};
// The mods folder as it is now: the enabled mods in load order, then the rest. Reads the folder each call, so
// it's for occasional use, such as a crash report. Any thread.
std::vector<InstalledMod> InstalledMods();

// A setting a mod added (rex.cvar.boolean and the rest), for the game's settings menu. It stays for the process.
struct ModSetting {
  std::string name;      // the full name, "mods.<name>", for rex::SettingsRegistry::Find
  std::string category;  // as the mod gave it, "Scripts" when it gave none
  std::string mod;       // the folder of the mod that added it
};
// Every setting mods added so far, in the order they were added. Any thread.
std::vector<ModSetting> ModSettings();

// Content: for a game that loads files from mods (models, textures, data). Each reads the mods folder as it is
// now, so a change applies at once, where code mods wait for the next start. Any thread.
std::filesystem::path ModsDir();
// The enabled mods' folders, in load order.
std::vector<std::filesystem::path> EnabledModDirs();
// Goes up whenever a mod is turned on or off, moved or renamed in this process (the menu, the calls below, an
// install), so a cache of mod content knows to reload.
uint64_t ModsRevision();
// Turns a mod on or off, as the menu does. False when there's no such mod.
bool SetModEnabled(std::string_view folder, bool enabled);
// Renames a mod's folder, keeping its place in the load order.
bool RenameMod(std::string_view from, std::string_view to, std::string* error = nullptr);

// For a game moving from a mod system of its own: when the mods folder has no load order yet, these folders
// start enabled in this order and every other one starts off. Call before Start. False when there was a load
// order already, which is then left alone.
bool SeedLoadOrder(const std::vector<std::string>& enabled);

// The interface native mods get. The game may use it too, as Host().
const RexFabricApi& Api();
RexFabricMod* Host();

}  // namespace rexfabric
