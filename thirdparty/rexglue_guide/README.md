# Rexliveless Guide, Guide..

An Xbox 360 style Guide for rexglue recompilations. Drop it into your
project and your recomp should get a working Xbox LIVE replacement using our
[RexLiveless](https://rexliveless.xyz) server:

- Sign-in, profiles, gamerpics and uploaded profile photos
- Friends, friend requests, messages and notifications
- Game invites and online sessions, System Link, and crates/blueprints for titles that use them
- Achievements (with unlock pop-ups) and leaderboards
- Themes: Light/Dark mode, curated themes from a gallery, and an in-Guide theme designer
- The 360's look and sounds: blades, the Ring of Light, toasts
- Parties and Voice is not implemented but if this gets used enough or a game needs it we will consider adding it.

The Guide hooks the title's XAM and network calls itself, so the game sees a signed in LIVE
profile and real friends per controller without any changes to rexglue.

## 1. Add the sources

Put the repository in your project, e.g. as a submodule:

```sh
git submodule add https://github.com/masterspike52/RexLiveless.git thirdparty/rexglue_guide
```

## 2. CMake

Include the Guide's CMake file and attach it to your executable **after** `rexglue_setup_target`:

```cmake
add_executable(mygame ${MYGAME_SOURCES})
rexglue_setup_target(mygame)

# Rexglue Guide
include(thirdparty/rexglue_guide/CMakeLists.txt)
rexglue_guide_attach(mygame)
```

`rexglue_guide_attach` adds the Guide's sources to your target (not as a static library), its
include directories (`include/` and `third_party/`), and the system libraries it needs.

The sources have to compile into the executable itself. The Guide defines `__imp__Xam*` and
`__imp__NetDll_*` import functions.

## 3. Hook it into your app

```cpp
#include <rexglue_guide/guide.h>

class MyGameApp : public rex::ReXApp {
  // The Guide's fonts (Selawik) go into the SDK's font atlas.
  void OnConfigureFonts(ImFontAtlas* atlas) override {
    rex::ReXApp::OnConfigureFonts(atlas);
    rexglue_guide::ConfigureFonts(atlas);
  }

  // Create your own dialogs first, then the Guide, so it draws on top of them.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    rex::ReXApp::OnCreateDialogs(drawer);

    rexglue_guide::AddCredit("Your Name", "Programming"); //This can be repeated to add many credits

    rexglue_guide::Install(this, drawer, {
        .title_name = "My Game",
        .on_quit = [this] { if (auto* w = window()) w->RequestClose(); },
        .immediate_drawer = immediate_drawer(),  // required: icons, gamerpics, pictures
    });
  }

  void OnShutdown() override {
    rexglue_guide::Shutdown();
    rex::ReXApp::OnShutdown();
  }

  // While the Guide (or its sign-in screen) is up, the game shouldn't act on input.
  bool WantsUIInputMode() const override { return rexglue_guide::IsOpen(); }
};
```

`immediate_drawer` must be set, or the Guide has no way to upload its icons and pictures and they
come out blank.

If your app already uses `WantsUIInputMode` for its own UI, combine them
(`return my_ui_open || rexglue_guide::IsOpen();`), or hold game input yourself while
`rexglue_guide::IsOpen()` is true.

That's all that's required. On first launch the Guide shows its sign-in screen; players create a
RexLiveless profile or sign in to an existing one, and are remembered after that.

## Opening the Guide

- **F10**: the `bind_guide` key bind (rebindable with the SDK's key binds, I was going to use Enter but figured that would probably conflict more then F10).
- **The controller's Guide button**: turn on the runtime's `guide_button` cvar so the button
  reaches the game's input reads; the Guide sees it there. The Guide opens for the player whose
  controller pressed it.
- **The title:** calls functions like `XShowGamerCardUI` that open the Guide on that player's gamer card.

The Guide's **System Settings > Config** page lists the game's settings (rexglue cvars) by category, like the F4 settings overlay: toggles, choices, sliders for ranged numbers, and the on-screen keyboard for the rest. Pass `config_file` so changes are saved.

## Config reference

All fields of `rexglue_guide::Config` are optional except `immediate_drawer`.

| Field | What it does |
|---|---|
| `title_name` | Your game's name, shown in the Guide (achievements, presence). Default "This Game". |
| `data_dir` | Where accounts and preferences are kept. Default `<exe dir>/guide`. |
| `on_quit` | Adds **Quit Game** to the Settings blade; called when chosen. |
| `ready_for_sign_in` | The sign-in screen waits until this returns true (e.g. while a launcher menu is open). |
| `immediate_drawer` | Your app's `immediate_drawer()`. **Required.** |
| `show_keyboard` | Use your app's own on-screen keyboard for the Guide's text entry instead of the Guide's. Call `done` exactly once, from any thread, with the text or `std::nullopt` if cancelled. |
| `player_for_controller` | Maps a controller (the user index the title reads it with) to a player 0-3, or -1 for none. Default: the same index. |
| `game_frame` | Your renderer's last presented game frame as an `ImmediateTexture` of `immediate_drawer` (and its on-screen UV rect). When set, the Guide blurs the live game behind it, as on the console; otherwise it only darkens it. |
| `config_file` | Where **System Settings > Config** saves the game's settings (cvars), the same file your F4 settings overlay saves to. Unset, changes made there last until the game closes. |
| `show_setting` | Which settings Config lists, by cvar name (for example to leave out ones your app hides). Unset: every setting except console commands. |

### Optional extras

```cpp
// Your own on-screen keyboard for gamertags, passwords and messages:
.show_keyboard = [](const rexglue_guide::TextInput& in, rexglue_guide::TextInputDone done) {
  MyKeyboard::Ask(in.title, in.description, in.text, in.max_length, in.masked, std::move(done));
},

// The live game blurred behind the Guide. `uv` is the part of the texture on screen;
// leave it at {0, 0, 1, 1} if the texture covers the whole window.
.game_frame = [](float uv[4]) -> rex::ui::ImmediateTexture* { return MyRenderer::LastFrameTexture(); },
```

Other functions in `guide.h`:

| Function | Use |
|---|---|
| `AddCredit(who, what)` | Adds a line to Settings > Credits, in order, before the Guide's own credits. |
| `IsOpen()` | True while the Guide or sign-in screen is up. |
| `SignedIn(user)` | Whether player 0-3 has a Guide account signed in. Any thread. |
| `SetKeyboardPad(player, buttons, lx, ly)` / `ClearKeyboardPad()` | If your app maps keyboard keys to a controller, feed that controller's state here so the keyboard opens and drives the Guide like a pad (button `0x0400` is the Guide button). The Guide's own Q/E/X/Y letter shortcuts are off while it's set. |

## Your own menus

`AddMenu` puts a menu of your app's on the Guide, drawn in its style (rows, switches, sliders,
choice lists and the player's theme), so your tools needn't be plain ImGui windows. A menu is
either a **blade** at the far left or right of the Guide's own, or a **detached** window over the
game.

```cpp
rexglue_guide::AddMenu({
    .id = "camera",
    .title = "Camera",                                  // the blade's label, or the window's title
    .place = rexglue_guide::MenuPlace::kRightBlade,     // kLeftBlade, kRightBlade or kDetached
    .icon = camera_png,                                 // optional: white on transparent PNG/JPEG
    .build = [](rexglue_guide::Menu& m) {               // called each frame it's shown
      m.Choice("Source", &source, {"Webcam", "Image", "Off"});
      m.Toggle("Mirror", &mirror);
      m.Slider("Brightness", &brightness, 0.0f, 2.0f, 0.1f);
      m.Page("Advanced", [](rexglue_guide::Menu& page) { page.Number("Exposure", &exposure, 0, 100); });
      if (m.Button("Take Picture")) TakePicture();
      m.Image(preview_texture, 4.0f / 3.0f);
    }});
```

As in ImGui, `build` runs every frame the menu is shown and each call returns what the player did
with that row (chosen, changed). The rows: `Button`, `Toggle`, `Choice` (left/right step it, choosing
it lists the options, with a search when there are many), `Number`, `Slider`, `TextField` (the
Guide's keyboard), `Page` (a row opening a page of its own), and `Header`, `Text` and `Image`, which
aren't picked. A label's text after `##` isn't shown, and only tells rows with the same words apart.

Detached menus take a `MenuWindow`: `width`/`height` and an `anchor` (a corner, an edge or the
centre) with an `x`/`y` offset, all in pixels at 1080p; `strip` puts the title down a blade-like
strip on the left instead of across the top; `movable` lets its title drag it; and `hold_input`
makes the pad and keys work it while it's open (holding the game's input, so `IsOpen()` is true),
where otherwise only the mouse does and the game plays on.

| Function | Use |
|---|---|
| `ShowMenu(id, show)` / `ToggleMenu(id)` | Opens or closes a detached menu's window; for a blade, opens the Guide on it. Any thread. |
| `MenuShown(id)` | Its window is open, or the Guide is on its blade. Any thread. |

## What the Guide takes over

Linked into your executable, the Guide replaces these imports with its own:

- **Sign-in and profiles**: `XamUserGetSigninState`, `XamUserGetSigninInfo`, `XamUserGetXUID`,
  `XamUserGetName`, `XamUserCheckPrivilege`, `XamUserAreUsersFriends`, `XamContentGetCreator`
- **UI**: `XamShowGamerCardUIForXUID`
- **Sessions, stats and enumerators**: `XMsgInProcessCall`, `XMsgStartIORequest`,
  `XamCreateEnumeratorHandle`, `XamUserCreateStatsEnumerator`
- **Input** (read only, to see the Guide button): `XamInputGetState`
- **Networking**: the `NetDll_*` socket and `XNet*` functions, so online and System Link traffic
  goes peer to peer or through the RexLiveless relay

`Install` also turns off the SDK's own web LIVE client (`xlive_web_enabled`), so the two don't run
side by side.

## Runtime settings

Environment variables, mostly for testing:

| Variable | Effect |
|---|---|
| `REXLIVELESS_SERVER` | The server to use, `http(s)://host:port`. Default `https://rexliveless.xyz`. |
| `REXLIVELESS_MOCK=1` | No server: a built-in offline mock with sample friends and messages. |
| `REXGLUE_GUIDE_DATA` | Overrides `data_dir`, e.g. to run a second copy on one PC with another account. |
| `REXLIVELESS_FORCE_RELAY=1` | Sends peer traffic only through the relay (for testing it on one PC). |

The data folder holds `account.json` (the remembered sign-in, per server), `preferences.json`
(sounds, pop-ups, mode, theme), and caches of downloaded gamerpics and theme pictures. Players 2-4
keep their accounts in `player2` to `player4` inside it.

No sounds ship with the Guide. To give it some, put WAVs (48 kHz, 16-bit, stereo) in
`assets/sounds/` before building: `page_left`, `page_right`, `select`, `open`, `notify`, `move`,
`back` and `close`, each `.wav`. Any that are missing play nothing.

## Source layout

For contributors. The Guide UI is one dialog class (`src/guide_dialog.h`) whose parts live in a
file per area, so changes to different parts land in different files:

| File | Area |
|---|---|
| `guide.cpp` | Public API, the per-frame update, opening/closing, page stacks |
| `guide_pages.cpp` | Page dispatch and the blades' own pages |
| `guide_friends.cpp`, `guide_messages.cpp` | Friends, requests, notifications, gamer cards, messages |
| `guide_games.cpp` | Achievements, leaderboards, presence reporting |
| `guide_account.cpp` | Sign-in, profile creation, gamerpics and photos |
| `guide_preferences.cpp`, `guide_themes.cpp` | Preferences, the theme gallery and designer |
| `guide_input.cpp` | Navigation and text entry |
| `guide_menus.cpp` | Apps' menus (`AddMenu`): their rows, blades and windows |
| `guide_draw.cpp`, `guide_draw_list.cpp`, `guide_popups.cpp` | Drawing |
| `server_backend.cpp`, `mock_backend.cpp` | Talking to RexLiveless (or the offline mock) |
| `xam_live.cpp`, `net.cpp`, `live_sessions.cpp`, `live_stats.cpp` | The XAM and network imports the title calls |

# Special Thanks
High-quality default gamerpics: recreations of the 360's by **u/EKAJ13**
(https://www.reddit.com/r/xboxone/comments/6fovrc/ive_been_recreating_some_of_the_old_xbox_360/).

Gamerpic Database: **xboxgamer.pics** (https://xboxgamer.pics), an archive of the 360's gamerpics
by game. Profiles pick from it under Change Gamerpic > Gamerpic Database; the pictures load from its CDN.

The Guide always lists both credits in Settings > Credits.
