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
#include <string_view>
#include <vector>

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
  // System Settings > Config saves the game's settings (cvars) here, as the
  // F4 settings overlay does; unset, changes there last until the game closes.
  std::filesystem::path config_file;
  // Optional: whether Config lists a setting, by cvar name (e.g. to leave out
  // ones the app hides). Unset: all of them but console commands.
  std::function<bool(std::string_view name)> show_setting;
  // Optional: why leaderboard scores and replays (XSessionWriteStats, game
  // clips) can't be posted now, e.g. "cheats are on"; empty when they can.
  // They fail for the title then, and the player gets a notification saying
  // why. Asked on the game's threads. Unset: always posted.
  std::function<std::string()> leaderboard_block;
  // Optional: this build's name (e.g. its version), sent with leaderboard
  // scores and replays so the server can refuse ones from old builds.
  std::string build;
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

// Xbox LIVE storage (XStorage*), for a title with its XStorage calls compiled
// in: that XDK code hands XAM its requests in LIVE's own wire format, so the
// host hooks each call at its entry and calls these with the guest arguments.
// One returns false when it isn't the Guide's to answer (no RexLiveless
// account signed in, or a server path the Guide didn't build): the title's own
// code runs then. Otherwise *result is what the call returns (ERROR_IO_PENDING
// with an overlapped, which completes once RexLiveless answers). Game thread.
bool StorageUploadFromMemory(uint32_t user, uint32_t server_path, uint32_t size, uint32_t buffer,
                             uint32_t overlapped, uint32_t* result);
bool StorageDownloadToMemory(uint32_t user, uint32_t server_path, uint32_t size, uint32_t buffer,
                             uint32_t results_size, uint32_t results, uint32_t overlapped, uint32_t* result);
bool StorageEnumerate(uint32_t user, uint32_t server_path, uint32_t start, uint32_t max, uint32_t results_size,
                      uint32_t results, uint32_t overlapped, uint32_t* result);

// App menus -------------------------------------------------------------------
//
// A menu of the app's own, drawn the way the Guide is (its rows, switches,
// sliders and theme). Attached, it's a blade of the Guide's, at the far left or
// far right of its own; detached, a window over the game.
//
//   rexglue_guide::AddMenu({.id = "camera", .title = "Camera", .build = [](rexglue_guide::Menu& m) {
//     m.Toggle("Mirror", &mirror);
//     if (m.Button("Take Picture")) TakePicture();
//   }});

enum class MenuPlace { kLeftBlade, kRightBlade, kDetached };

// Where a detached menu sits on the screen.
enum class MenuAnchor { kTopLeft, kTop, kTopRight, kLeft, kCenter, kRight, kBottomLeft, kBottom, kBottomRight };

// A detached menu's window. Sizes and offsets are pixels at 1080p, scaled with
// the screen as the Guide is.
struct MenuWindow {
  float width = 620.0f, height = 720.0f;
  MenuAnchor anchor = MenuAnchor::kCenter;
  float x = 0.0f, y = 0.0f;  // from the anchor: right and down
  // The title down a strip on the left, under the icon, as on a blade; false:
  // across the top.
  bool strip = false;
  // Its title drags it about (until the game closes).
  bool movable = true;
  // While it's open, the game's input is held (IsOpen() is true) and the pad
  // and keys work it; otherwise only the mouse does, and the game plays on.
  bool hold_input = false;
};

class Menu;

struct MenuDef {
  std::string id;     // unique; what ShowMenu calls it
  std::string title;  // the blade's label, or the window's title
  MenuPlace place = MenuPlace::kRightBlade;
  MenuWindow window;  // kDetached
  // Optional: a PNG or JPEG for the blade's strip (and a strip window's), drawn
  // in the theme's text colour: white on transparent. Without one, the title's
  // first letter.
  std::string icon;
  // What's on it, called each frame it's shown, on the Guide's drawing thread.
  // As in ImGui, each call returns what the player did with it last.
  std::function<void(Menu&)> build;
};

// Adds a menu. Call before or after Install; blades are placed in the order
// added, each the next one out on its side.
void AddMenu(MenuDef menu);
// Detached: opens or closes its window. Attached: opens the Guide on its blade,
// or closes the Guide if it's there. Any thread.
void ShowMenu(std::string_view id, bool show = true);
void ToggleMenu(std::string_view id);
// Its window is open, or the Guide is on its blade. Any thread.
bool MenuShown(std::string_view id);

// What a menu's build function puts on it, top to bottom. Labels are what the
// rows say; anything after "##" in one isn't shown, and only tells rows with
// the same words apart (as in ImGui).
class Menu {
 public:
  enum class Tone { kNormal, kDim, kError };

  // A row that does something: true the frame it's chosen.
  bool Button(std::string_view label, std::string_view detail = {});
  // On or off, shown as a switch. True when it changed.
  bool Toggle(std::string_view label, bool* value);
  // One of `options`: left and right step through them, choosing it lists them
  // (with a search when there are many). True when it changed.
  bool Choice(std::string_view label, int* index, const std::vector<std::string>& options);
  // A whole number in [min, max]: left and right step it, choosing it types one
  // in. True when it changed.
  bool Number(std::string_view label, int* value, int min, int max, int step = 1);
  // A value in [min, max] on a slider; `format` (printf) shows it.
  bool Slider(std::string_view label, float* value, float min, float max, float step = 0.0f,
              const char* format = "%.2f");
  // Text, typed in with the keyboard; shown under the label.
  bool TextField(std::string_view label, std::string* text, size_t max_length = 32);
  // A row that opens a page of its own: `build` says what's on it, and is
  // called only while it's shown.
  void Page(std::string_view label, const std::function<void(Menu&)>& build, std::string_view detail = {});
  // Rows that aren't picked: a heading, wrapped text, and a picture as wide as
  // the menu (a texture of Config::immediate_drawer's; null shows its place),
  // width over height, no taller than max_height (pixels at 1080p).
  void Header(std::string_view text);
  void Text(std::string_view text, Tone tone = Tone::kNormal);
  void Image(rex::ui::ImmediateTexture* texture, float aspect, float max_height = 360.0f);
  // Goes back a page once it's built; at the first, closes the window (or the
  // Guide).
  void Back();
  bool detached() const;

  struct State;
  explicit Menu(State* state) : state_(state) {}

 private:
  State* state_;
};

}  // namespace rexglue_guide
