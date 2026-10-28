#pragma once
/**
 * Rexglue Guide: an Xbox 360 style Guide (accounts, friends, notifications,
 * achievements) for rexglue projects.
 *
 * Setup in a rex::ReXApp subclass:
 *   void OnConfigureFonts(ImFontAtlas* atlas) override {
 *     rexglue_guide::ConfigureFonts(atlas);
 *   }
 *   void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
 *     rexglue_guide::AddCredit("Your Name", "Programming");
 *     rexglue_guide::Install(this, drawer, {.title_name = "My Game",
 *                                           .immediate_drawer = immediate_drawer()});
 *   }
 *   void OnShutdown() override { rexglue_guide::Shutdown(); }
 *   bool WantsUIInputMode() const override { return rexglue_guide::IsOpen(); }
 *
 * CMake: rexglue_guide_attach(<target>) after add_executable().
 */

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

struct ImFontAtlas;

namespace rex {
class ReXApp;
}
namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
class ImmediateTexture;
}

namespace rexglue_guide {

// A request for text: what the Guide asks for (gamertags, passwords, messages).
struct TextInput {
  std::string title;
  std::string description;
  std::string text;  // starting text
  bool masked = false;
  size_t max_length = 32;  // characters
};
// Called exactly once, on any thread: the text, or nullopt if cancelled.
using TextInputDone = std::function<void(std::optional<std::string>)>;

struct Config {
  // Shown on the Games & Apps blade.
  std::string title_name;
  // Where the signed-in account is remembered. Empty = <exe dir>/guide.
  std::filesystem::path data_dir;
  // Adds "Quit Game" to the Settings blade when set.
  std::function<void()> on_quit;
  // The sign-in screen waits for this at startup, e.g. until a launcher closes.
  std::function<bool()> ready_for_sign_in;
  // What the Guide's icons, gamerpics and achievement icons are uploaded
  // through: the app's ReXApp::immediate_drawer().
  rex::ui::ImmediateDrawer* immediate_drawer = nullptr;
  // The app's own on-screen keyboard, used for the Guide's text entry when set;
  // otherwise the Guide draws its own.
  std::function<void(const TextInput&, TextInputDone)> show_keyboard;
  // The player (user index 0-3) a controller is, by the user index the title
  // reads it with (XInputGetState), or -1 for none. The Guide opens for the
  // player whose controller pressed the Guide button, with that player's
  // account. Unset: the same index.
  std::function<int(int controller)> player_for_controller;
  // Optional, called each frame the Guide is up: the game's frame as last
  // shown, as a texture of immediate_drawer's, and the part of it on screen
  // (uv: u0, v0, u1, v1); nullptr when there's none. Set, the Guide blurs the
  // live game behind it; unset, it just darkens it.
  std::function<rex::ui::ImmediateTexture*(float uv[4])> game_frame;
};

// Adds a line to Settings > Credits. Call before or after Install; credits show
// in the order added, followed by the Guide's own.
void AddCredit(std::string who, std::string what);

// Adds the Guide's fonts. Call from ReXApp::OnConfigureFonts.
void ConfigureFonts(ImFontAtlas* atlas);

// Creates the Guide. Call once from ReXApp::OnCreateDialogs, after the app's
// own dialogs so the Guide draws on top of them.
void Install(rex::ReXApp* app, rex::ui::ImGuiDrawer* drawer, Config config = {});

void Shutdown();

// True while the Guide or the sign-in screen is up; game input should be held.
bool IsOpen();

// True when a player (user index 0-3) has a Guide account signed in. Any
// thread.
bool SignedIn(int user);

// A keyboard playing a controller (the app's own key binds): its
// XINPUT_GAMEPAD buttons (0x0400 the Guide button) and left stick (-1..1, y
// up), held until the next call. It opens and drives the Guide like a pad, for
// 'player' (user index 0-3). While it's set, the Guide's own letter shortcuts
// (Q/E/X/Y) are off, as the binds may use those keys. Any thread.
void SetKeyboardPad(int player, uint16_t buttons, float lx, float ly);
// The keyboard no longer plays a controller.
void ClearKeyboardPad();

}  // namespace rexglue_guide
