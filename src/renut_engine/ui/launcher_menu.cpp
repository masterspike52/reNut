#include "renut_engine/ui/launcher_menu.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/app/app.h>
#include <rex/hid/input_system.h>
#include <rex/input_keys.h>
#include <rex/module.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/string/text.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/ui_system.h>
#include <rex/utility.h>
#include <SDL3/SDL.h>

#include "imgui.h"
#include "miniaudio.h"
#include "renut_engine/app/app_paths.h"
#include "renut_engine/app/game_setup.h"
#include "renut_engine/app/renut_settings.h"
#include "renut_engine/app/updater.h"
#include "renut_engine/app/workshop.h"
#include "renut_engine/game/graphics_menu.h"
#include "renut_engine/gpu/backend/native_window.h"
#include "renut_engine/mods/mod_loader.h"
// The Guide's HTTP client (WinHTTP, or the system's libcurl), built into reNut
// with the Guide.
#include "thirdparty/rexglue_guide/src/http.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <shobjidl.h>
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

namespace fs = std::filesystem;
namespace setup = renut::setup;
namespace workshop = renut::workshop;
namespace http = rexglue_guide::http;

// icon/launcher_background.png and icon/workshop_background.png, which
// CMakeLists.txt turns into these includes.
const unsigned char kBackgroundPng[] = {
#include "launcher_background.png.inc"
};
const unsigned char kWorkshopBackgroundPng[] = {
#include "workshop_background.png.inc"
};

// The logo and turning gears drawn over the 1920 x 1080 background, each pinned to a
// screen corner; the bottom-left pair mesh 12 teeth to 8, so the 8 turns 1.5x the other way.
const unsigned char kLogoPng[] = {
#include "launcher_logo.png.inc"
};
const unsigned char kGearTopPng[] = {
#include "launcher_gear_top.png.inc"
};
const unsigned char kGearMiddlePng[] = {
#include "launcher_gear_middle.png.inc"
};
const unsigned char kGearCornerPng[] = {
#include "launcher_gear_corner.png.inc"
};
constexpr float kArtWidth = 1920.0f;
constexpr float kArtHeight = 1080.0f;
constexpr float kLogoLeft = 7.0f;  // the logo's top left in the picture
constexpr float kLogoTop = 0.0f;
struct Gear {
    const unsigned char *png;
    size_t size;
    float x, y, radius;  // in the picture's pixels: its centre, and half the sprite's side
    bool right, bottom;  // the screen corner it keeps its place from
    float start, speed;  // degrees, and degrees a second
};
constexpr float kCornerGearSpeed = 6.0f;
const Gear kGears[] = {
    {kGearCornerPng, sizeof(kGearCornerPng), 41.4f, 1036.4f, 290.0f, false, true, 12.8f, kCornerGearSpeed},
    {kGearMiddlePng, sizeof(kGearMiddlePng), 377.2f, 784.2f, 176.0f, false, true, 29.34f,
     -kCornerGearSpeed * 12.0f / 8.0f},
    {kGearTopPng, sizeof(kGearTopPng), 1723.4f, 197.1f, 200.0f, true, false, 0.0f, 4.0f},
};

// Mumbo's Mountain, built from the game's wave bank (tools/make_launcher_music.py),
// looping the bank's own loop region after the intro.
const char kMusicOgg[] =
#include "launcher_music.ogg.inc"
    ;
constexpr ma_uint64 kMusicLoopStart = 583040;
constexpr ma_uint64 kMusicLoopFrames = 11874944;
constexpr float kMusicVolume = 0.6f;
constexpr ma_uint64 kMuteFadeMs = 250;

// The menu's layout in a 1672 x 941 frame the picture fills: a centre column of rows,
// and a workshop vehicle's preview on the right.
constexpr float kImageWidth = 1672.0f;
constexpr float kImageHeight = 941.0f;
constexpr float kColumnLeft = 560.0f;
// Rows end short of the workshop's details on the right.
constexpr float kRowRight = 1100.0f;
// Text under the rows, from the labels' left to the values' right.
constexpr float kTextWidth = kRowRight - kColumnLeft - 40.0f;
constexpr float kRowHeight = 38.0f;
// The rows' tops, one under another.
constexpr float kSlotTop[] = {368.0f, 406.0f, 444.0f, 482.0f, 520.0f, 558.0f, 596.0f, 634.0f, 672.0f, 710.0f};
constexpr int kSlots = int(sizeof(kSlotTop) / sizeof(kSlotTop[0]));
// Under the last slot: a job's progress, a message.
constexpr float kInfoTop = 800.0f;
constexpr float kTextSize = 27.0f;
constexpr float kSmallTextSize = 21.0f;
constexpr float kPhotoLeft = 1160.0f;
constexpr float kPhotoTop = 430.0f;
constexpr float kPhotoWidth = 460.0f;  // 16:9 inside a white border
constexpr float kPhotoBorder = 12.0f;
// The workshop: BACK and SHARE A VEHICLE (slots 0 and 1), then a grid of vehicle cards
// scrolled a row at a time, the selected one's details top right.
constexpr int kWorkshopFixed = 2;
constexpr int kGridColumns = 6;
constexpr int kGridRows = 2;
constexpr float kCardWidth = 258.0f;
constexpr float kGridGap = 12.0f;
constexpr float kGridRight = 1640.0f;
constexpr float kGridLeft = kGridRight - kCardWidth * kGridColumns - kGridGap * (kGridColumns - 1);
constexpr float kGridTop = 500.0f;
constexpr float kGridRowGap = 12.0f;
constexpr float kCardBorder = 6.0f;
constexpr float kCardPictureHeight = (kCardWidth - 2.0f * kCardBorder) * 9.0f / 16.0f;
constexpr float kCardHeight = kCardBorder + kCardPictureHeight + 30.0f;  // the name under the picture
constexpr float kDetailsLeft = 1150.0f;
constexpr float kDetailsTop = 352.0f;
constexpr float kDetailsWidth = kGridRight - kDetailsLeft;
// The music's speaker, in the picture's bottom right corner on every page.
constexpr float kMuteLeft = 1588.0f;
constexpr float kMuteTop = 874.0f;
constexpr float kMuteSize = 46.0f;
// The volume, as bars rising to the speaker's left; left of the first, a strip
// a click or drag silences it on.
constexpr int kVolumeBars = 10;
constexpr float kVolumeRight = kMuteLeft - 6.0f;
constexpr float kVolumeLeft = kVolumeRight - 100.0f;
constexpr float kVolumeSilent = 10.0f;
constexpr float kVolumeBarGap = 3.0f;
constexpr float kVolumeBottom = kMuteTop + 35.0f;
constexpr ImU32 kVolumeUnlit = IM_COL32(255, 255, 255, 45);

// In slot order. kScreenshot is no row of the menu: a file picker's answer
// for the share page.
enum class Row {
    kPlay, kDisc, kTitleUpdate, kDlc, kGameFolder, kUpdateFolder, kSaveFolder, kWorkshop, kOptions, kQuit, kCount,
    kScreenshot
};

constexpr const char *kRowLabels[] = {
    "PLAY", "EXTRACT GAME DISC", "EXTRACT TITLE UPDATE", "ADD DLC", "GAME FOLDER",
    "UPDATE FOLDER", "SAVE FOLDER", "WORKSHOP", "OPTIONS", "QUIT",
};
static_assert(int(Row::kCount) == kSlots);

// OPTIONS: its pages, then each page's settings under BACK (slots 1-9, the
// selected one's description below).
enum class Page { kMain, kWorkshop, kShare, kOptions, kSettings };
// WORKSHOP > SHARE A VEHICLE, in slot order.
enum class ShareRow { kBack, kVehicle, kName, kDescription, kScreenshot, kSubmit, kCount, kNone };
constexpr const char *kOptionPages[] = {"LANGUAGE", "GRAPHICS", "INPUT"};
constexpr int kOptionPageCount = int(sizeof(kOptionPages) / sizeof(kOptionPages[0]));
constexpr int kSettingRows = kSlots - 1;

struct Setting {
    const char *cvar;  // the setting's full name
    const char *label;
};
// LANGUAGE: the game's languages, a row each, set through the SDK's kernel.user_language
// (XLanguage id, kLanguages' order from 1), which the game reads at startup (sub_826F3F30).
constexpr const char kLanguageSetting[] = "kernel.user_language";
constexpr const char *kLanguages[] = {
    "English", "Japanese", "German", "French", "Spanish", "Italian", "Korean",
    "Traditional Chinese", "Portuguese", "Simplified Chinese", "Polish", "Russian",
};
constexpr const char kLanguageAbout[] =
    "The game's language: its menus, text and subtitles. Applies when PLAY starts the game. A language "
    "the game folder has no text for shows English.";
// GRAPHICS: what the in-game GRAPHICS menu has (game/graphics_menu.cpp),
// after the window and renderer.
constexpr Setting kGraphicsSettings[] = {
    {"ui.fullscreen", "Fullscreen"},
#if defined(_WIN32)
    {"renut.renut_gpu_backend", "Renderer"},  // Vulkan only elsewhere
#endif
    {"renut.renut_resolution", "Resolution"},
    {"renut.renut_field_of_view", "Field of View"},
    {"renut.renut_time_of_day", "Time of Day"},
    {"renut.renut_shadow_resolution", "Shadow Resolution"},
    {"renut.renut_shadow_softness", "Shadow Softness"},
    {"renut.renut_anisotropic_filtering", "Anisotropic Filtering"},
    {"renut.renut_ssao", "Ambient Occlusion"},
    {"renut.renut_reflection_resolution", "Reflections"},
    {"renut.renut_lod_distance", "Detail Distance"},
    {"renut.renut_grass_distance", "Grass Distance"},
    {"renut.renut_depth_of_field", "Depth of Field"},
    {"renut.renut_bloom", "Bloom"},
    {"renut.renut_msaa", "MSAA"},
    {"renut.renut_alpha_to_coverage", "Smooth Cut-Outs"},
    {"renut.renut_anti_aliasing", "Anti-Aliasing"},
    {"renut.renut_vsync", "V-Sync"},
    {"renut.frame_cap", "Frame Cap"},
};
// INPUT: the keyboard and mouse switches, then every key bind
// (Nuts&Bolts/Input's renut_key_*).
constexpr Setting kInputSettings[] = {
    {"renut.renut_keyboard", "Keyboard Controls"},
    {"renut.renut_mouse_camera", "Mouse Camera"},
    {"renut.renut_mouse_sensitivity", "Mouse Sensitivity"},
    {"renut.renut_mouse_invert_y", "Mouse Invert Y"},
#if defined(_WIN32)
    {"hid.drivers", "Controller Input"},  // SDL only elsewhere
#endif
    {"renut.renut_disable_extra_controllers", "Player 1 Only"},
};
constexpr const char kInputCategory[] = "Nuts&Bolts/Input";
constexpr const char kKeyPrefix[] = "renut_key_";
// Controller Input's choices: hid.drivers is a list of drivers, without its own.
constexpr const char kDriversSetting[] = "hid.drivers";
const std::vector<std::string> kDriverChoices = {"sdl3", "xinput"};

constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kDim = IM_COL32(255, 255, 255, 110);
constexpr ImU32 kNeeded = IM_COL32(255, 222, 120, 255);
constexpr ImU32 kErrorColour = IM_COL32(255, 150, 140, 255);
constexpr ImU32 kHighlight = IM_COL32(255, 255, 255, 38);
constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 90);

std::string Utf8(const fs::path &path) {
    return rex::string::PathToUTF8(path);
}

bool SameFolder(const fs::path &a, const fs::path &b) {
    std::error_code ec;
    return fs::weakly_canonical(fs::absolute(a, ec), ec) == fs::weakly_canonical(fs::absolute(b, ec), ec);
}

std::string Gigabytes(uint64_t bytes) {
    return std::format("{:.1f} GB", double(bytes) / double(1ull << 30));
}

std::string Upper(std::string s) {
    for (char &c : s)
        if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
    return s;
}

// 'text' shortened from its end with "..." until it fits 'room'.
std::string FitEnd(ImFont *font, float size, std::string text, float room) {
    if (font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x <= room)
        return text;
    while (!text.empty() && font->CalcTextSizeA(size, FLT_MAX, 0.0f, (text + "...").c_str()).x > room)
        text.pop_back();
    return text + "...";
}

// 'text' shortened from its start: a path keeps its end.
std::string FitStart(ImFont *font, float size, std::string text, float room) {
    while (text.size() > 4 && font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x > room)
        text = "..." + text.substr(4);
    return text;
}

// --- Settings -------------------------------------------------------------------

bool IsOn(const std::string &value) {
    return value == "true" || value == "1";
}

// A setting by its full name, or null when it isn't registered.
rex::SettingBase *FindSetting(const std::string &name) {
    auto found = REX_SETTINGS().Find(name);
    return found ? *found : nullptr;
}

bool IsNumber(const rex::SettingBase &s) {
    using rex::SettingType;
    const SettingType type = s.GetType();
    return type == SettingType::kI32 || type == SettingType::kI64 || type == SettingType::kU32 ||
           type == SettingType::kU64 || type == SettingType::kF64;
}

// The values a setting steps through.
std::vector<std::string> Choices(const rex::SettingBase &s) {
    if (s.GetName() == kDriversSetting)
        return kDriverChoices;
    return s.GetConstraints().allowed;
}

// What a setting's row says on its right.
std::string SettingValue(const rex::SettingBase &s, bool key) {
    const std::string value = s.GetText();
    if (key)
        return value.empty() ? "NONE" : Upper(value);
    if (s.GetType() == rex::SettingType::kBool)
        return IsOn(value) ? "ON" : "OFF";
    if (s.GetType() == rex::SettingType::kF64)
        return std::format("{:.2f}", std::atof(value.c_str()));
    return Upper(value);
}

std::string Trim(const std::string &s) {
    const size_t l = s.find_first_not_of(" \t\r");
    const size_t r = s.find_last_not_of(" \t\r");
    return l == std::string::npos ? std::string() : s.substr(l, r - l + 1);
}

// Writes a startup-only setting ("hid.drivers") into the config file for the next start,
// as it can't change while reNut runs: its key in its scope's table, added if missing.
bool WriteStartupSetting(const fs::path &config_path, const std::string &name, const std::string &value) {
    const size_t dot = name.rfind('.');
    if (config_path.empty() || dot == std::string::npos)
        return false;
    const std::string table = "[" + name.substr(0, dot) + "]";
    const std::string key = name.substr(dot + 1);
    const std::string line = key + " = \"" + value + "\"";
    std::vector<std::string> lines;
    {
        std::ifstream in(config_path);
        for (std::string l; std::getline(in, l);)
            lines.push_back(l);
    }
    bool in_table = false, done = false;
    for (size_t i = 0; i < lines.size() && !done; ++i) {
        const std::string t = Trim(lines[i]);
        if (!t.empty() && t.front() == '[') {
            if (in_table) {
                lines.insert(lines.begin() + std::ptrdiff_t(i), line);
                done = true;
            }
            in_table = t == table;
            continue;
        }
        if (in_table && t.rfind(key, 0) == 0 && Trim(t.substr(key.size())).rfind('=', 0) == 0) {
            lines[i] = line;
            done = true;
        }
    }
    if (!done) {
        if (!in_table) {
            lines.push_back("");
            lines.push_back(table);
        }
        lines.push_back(line);
    }
    std::ofstream out(config_path, std::ios::trunc);
    for (const std::string &l : lines)
        out << l << "\n";
    out.close();
    return bool(out);
}

// Steps the setting to its next (dir 1) or previous (dir -1) value and saves it to
// renut.toml; false if it has none. A startup-only one is saved and reNut restarts.
bool StepSetting(const rex::SettingBase &s, int dir, const fs::path &config_path) {
    const std::string current = s.GetText();
    const rex::SettingConstraints &c = s.GetConstraints();
    const std::vector<std::string> values = Choices(s);
    std::string next;
    if (s.GetType() == rex::SettingType::kBool) {
        next = IsOn(current) ? "false" : "true";
    } else if (values.size() >= 2) {
        const int count = int(values.size());
        const auto it = std::find(values.begin(), values.end(), current);
        const int index = it == values.end() ? 0 : int(it - values.begin());
        next = values[size_t(((index + dir) % count + count) % count)];
    } else if (IsNumber(s) && c.min && c.max && *c.max > *c.min) {
        const double lo = *c.min, hi = *c.max;
        const bool whole = s.GetType() != rex::SettingType::kF64;
        const double step = whole ? std::max(1.0, std::round((hi - lo) / 20.0)) : 0.05;
        double value = std::clamp(std::atof(current.c_str()) + dir * step, lo, hi);
        next = whole ? std::to_string(int64_t(std::llround(value)))
                     : std::format("{:g}", std::round(value * 100.0) / 100.0);
    } else {
        return false;
    }
    if (next == current)
        return false;
    if (s.IsFrozen() && c.life == rex::SettingLife::kStartupOnly) {
        (void)REX_SETTINGS().Save(config_path);
        if (!WriteStartupSetting(config_path, s.GetName(), next))
            return false;
        rex::app::RequestRestart();
        return true;
    }
    if (!REX_SETTINGS().SetText(s.GetName(), next))
        return false;
    if (!config_path.empty())
        (void)REX_SETTINGS().Save(config_path);
    return true;
}

// A key bind's label, from its description: "Controller left bumper." is
// LEFT BUMPER, "Move forward (left stick up)." MOVE FORWARD.
std::string KeyLabel(const rex::SettingBase &s, const std::string &name) {
    if (name == "renut_key_walk")
        return "WALK";
    const std::string &help = s.GetHelp();
    std::string label = help.substr(0, help.find_first_of(".:("));
    if (label.rfind("Controller ", 0) == 0)
        label = label.substr(11);
    while (!label.empty() && label.back() == ' ')
        label.pop_back();
    return Upper(label.empty() ? name.substr(sizeof(kKeyPrefix) - 1) : label);
}

// Captures the next key or mouse button pressed in the game window while armed,
// as the virtual keys the binds use (on the UI thread).
class KeyCapture : public rex::ui::IUIEventListener {
public:
    // Added before the keyboard's and mouse camera's listeners (RenutApp::OnInit), so it hears a key first.
    void Attach(rex::ui::IUISystem &ui) { guard_ = ui.AddEventListener(*this); }
    void Detach() { guard_.Reset(); }
    void Arm() {
        pressed_ = rex::VirtualKey::kNone;
        armed_ = true;
    }
    void Disarm() { armed_ = false; }
    // The key pressed since Arm, once; kNone until one is.
    rex::VirtualKey Take() {
        const rex::VirtualKey key = pressed_;
        pressed_ = rex::VirtualKey::kNone;
        return key;
    }

    void OnInputEvent(rex::InputEvent &e) override {
        using Type = rex::InputEvent::Type;
        if (e.type == Type::kKeyDown) {
            Press(e.key);
            return;
        }
        if (e.type != Type::kButtonDown)
            return;
        switch (e.button) {
        case rex::MouseButton::kLeft: Press(rex::VirtualKey::kLButton); break;
        case rex::MouseButton::kRight: Press(rex::VirtualKey::kRButton); break;
        case rex::MouseButton::kMiddle: Press(rex::VirtualKey::kMButton); break;
        case rex::MouseButton::kX1: Press(rex::VirtualKey::kXButton1); break;
        case rex::MouseButton::kX2: Press(rex::VirtualKey::kXButton2); break;
        default: break;
        }
    }
    void OnUIEvent(const rex::ui::UIEvent &) override {}

private:
    void Press(rex::VirtualKey key) {
        if (armed_ && pressed_ == rex::VirtualKey::kNone)
            pressed_ = key;
    }

    rex::ScopeGuard guard_;
    bool armed_ = false;
    rex::VirtualKey pressed_ = rex::VirtualKey::kNone;
};

// --- Controllers --------------------------------------------------------------
// Before the game's input system runs, controllers are polled from SDL's gamepads.

enum PadButton : uint32_t {
    kPadUp = 1, kPadDown = 2, kPadA = 4, kPadB = 8, kPadLeft = 16, kPadRight = 32, kPadY = 64,
    kPadLeftBumper = 128, kPadRightBumper = 256
};

class Pads {
public:
    ~Pads() { Close(); }

    uint32_t Held() {
        if (!Init())
            return 0;
        SDL_UpdateGamepads();
        Refresh();
        uint32_t held = 0;
        for (SDL_Gamepad *g : open_) {
            const int16_t x = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
            const int16_t y = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
            if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -16000)
                held |= kPadUp;
            if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > 16000)
                held |= kPadDown;
            if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -16000)
                held |= kPadLeft;
            if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > 16000)
                held |= kPadRight;
            if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH))
                held |= kPadA;
            if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_EAST))
                held |= kPadB;
            if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_NORTH))
                held |= kPadY;
            if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))
                held |= kPadLeftBumper;
            if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER))
                held |= kPadRightBumper;
        }
        return held;
    }

    // At PLAY: closes the controllers and restores SDL's gamepad events and HIDAPI
    // hint for the game's input system, which shares this SDL on Linux.
    void Close() {
        for (SDL_Gamepad *g : open_)
            SDL_CloseGamepad(g);
        open_.clear();
        if (std::exchange(started_, false)) {
            SDL_SetJoystickEventsEnabled(true);
            SDL_SetGamepadEventsEnabled(true);
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
            SDL_ResetHint(SDL_HINT_JOYSTICK_HIDAPI);
        }
    }

private:
    bool Init() {
        if (started_ || SDL_WasInit(SDL_INIT_GAMEPAD))
            return true;
        if (failed_)
            return false;
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            SDL_ResetHint(SDL_HINT_JOYSTICK_HIDAPI);
            failed_ = true;
            return false;
        }
        SDL_SetJoystickEventsEnabled(false);
        SDL_SetGamepadEventsEnabled(false);
        started_ = true;
        return true;
    }

    void Refresh() {
        for (auto it = open_.begin(); it != open_.end();) {
            if (SDL_GamepadConnected(*it)) {
                ++it;
            } else {
                SDL_CloseGamepad(*it);
                it = open_.erase(it);
            }
        }
        int count = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&count);
        for (int i = 0; ids && i < count; ++i) {
            const bool have = std::any_of(open_.begin(), open_.end(),
                                          [&](SDL_Gamepad *g) { return SDL_GetGamepadID(g) == ids[i]; });
            if (!have)
                if (SDL_Gamepad *g = SDL_OpenGamepad(ids[i]))
                    open_.push_back(g);
        }
        SDL_free(ids);
    }

    std::vector<SDL_Gamepad *> open_;
    bool started_ = false;  // the subsystem is the launcher's
    bool failed_ = false;
};

// --- Music ----------------------------------------------------------------------
// The menu's music on its own miniaudio engine until PLAY; muting fades it out silently.

// Whether the music plays, and how loud: the launcher's own, in launcher.cfg in
// reNut's writable folder ("music=on" or "off", "volume=" 0 to 1).
constexpr const char kMusicFile[] = "launcher.cfg";

struct MusicChoice {
    bool on = true;
    double volume = 1.0;
};

MusicChoice LoadMusicChoice() {
    MusicChoice choice;
    std::ifstream in(renut::paths::WritableFolder() / kMusicFile);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "music")
            choice.on = value != "off";
        else if (key == "volume")
            choice.volume = std::clamp(std::atof(value.c_str()), 0.0, 1.0);
    }
    return choice;
}

void SaveMusicChoice(const MusicChoice &choice) {
    std::ofstream out(renut::paths::WritableFolder() / kMusicFile, std::ios::trunc);
    out << "music=" << (choice.on ? "on" : "off") << "\nvolume=" << std::format("{:g}", choice.volume) << "\n";
}

class Music {
public:
    ~Music() { Stop(); }

    void Start(bool muted, float volume) {
        // No audio device, or music that can't be decoded: no music.
        if (engine_ready_ || ma_engine_init(nullptr, &engine_) != MA_SUCCESS)
            return;
        engine_ready_ = true;
        const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
        if (ma_decoder_init_memory(kMusicOgg, sizeof(kMusicOgg) - 1, &config, &decoder_) != MA_SUCCESS) {
            Stop();
            return;
        }
        decoder_ready_ = true;
        ma_data_source_set_loop_point_in_pcm_frames(&decoder_, kMusicLoopStart, kMusicLoopStart + kMusicLoopFrames);
        if (ma_sound_init_from_data_source(&engine_, &decoder_, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &sound_) !=
            MA_SUCCESS) {
            Stop();
            return;
        }
        sound_ready_ = true;
        ma_sound_set_looping(&sound_, MA_TRUE);
        SetVolume(volume);
        const float fade = muted ? 0.0f : 1.0f;
        ma_sound_set_fade_in_milliseconds(&sound_, fade, fade, 0);
        ma_sound_start(&sound_);
    }

    void SetMuted(bool muted) {
        if (sound_ready_)
            ma_sound_set_fade_in_milliseconds(&sound_, -1.0f, muted ? 0.0f : 1.0f, kMuteFadeMs);
    }

    // 0 to 1, of the music's own level; muted or not.
    void SetVolume(float volume) {
        if (sound_ready_)
            ma_sound_set_volume(&sound_, kMusicVolume * volume);
    }

    void Stop() {
        if (std::exchange(sound_ready_, false))
            ma_sound_uninit(&sound_);
        if (std::exchange(decoder_ready_, false))
            ma_decoder_uninit(&decoder_);
        if (std::exchange(engine_ready_, false))
            ma_engine_uninit(&engine_);
    }

private:
    ma_engine engine_{};
    ma_decoder decoder_{};
    ma_sound sound_{};
    bool engine_ready_ = false;
    bool decoder_ready_ = false;
    bool sound_ready_ = false;
};

// A held direction: once when pressed, then again and again while held.
struct Repeat {
    bool held = false;
    float wait = 0.0f;

    bool Step(bool down, float dt) {
        if (!down) {
            held = false;
            return false;
        }
        if (!held) {
            held = true;
            wait = 0.4f;
            return true;
        }
        wait -= dt;
        if (wait > 0.0f)
            return false;
        wait = 0.12f;
        return true;
    }
};

// --- File and folder pickers ---------------------------------------------------
// Answered asynchronously: Windows' dialog runs on its own thread, SDL's via a callback.

struct PickState {
    std::mutex mutex;
    bool answered = false;
    Row row = Row::kCount;
    std::vector<fs::path> paths;  // empty when cancelled
    std::string error;            // no picker
};

enum class PickKind { kFolder, kDiscImage, kPackage, kDlc, kImage };

#if defined(_WIN32)

fs::path ItemPath(IShellItem *item) {
    fs::path path;
    PWSTR wide = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wide))) {
        path = fs::path(wide);
        CoTaskMemFree(wide);
    }
    return path;
}

std::vector<fs::path> RunPicker(PickKind kind, const fs::path &start, HWND owner) {
    std::vector<fs::path> paths;
    IFileOpenDialog *dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return paths;
    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM;
    if (kind == PickKind::kFolder)
        options |= FOS_PICKFOLDERS;
    dialog->SetOptions(options);
    if (kind == PickKind::kDiscImage) {
        const COMDLG_FILTERSPEC filters[] = {{L"Disc images (*.iso)", L"*.iso"}, {L"All files", L"*.*"}};
        dialog->SetFileTypes(2, filters);
    }
    if (kind == PickKind::kImage) {
        const COMDLG_FILTERSPEC filters[] = {{L"Pictures (*.png, *.jpg)", L"*.png;*.jpg;*.jpeg"}};
        dialog->SetFileTypes(1, filters);
    }
    dialog->SetTitle(kind == PickKind::kFolder      ? L"Choose a folder"
                     : kind == PickKind::kDiscImage ? L"Choose the game disc's image"
                     : kind == PickKind::kPackage   ? L"Choose Title Update #3 (the TU_... file)"
                     : kind == PickKind::kImage     ? L"Choose a screenshot of the vehicle"
                                                    : L"Choose the DLC package");
    std::error_code ec;
    if (!start.empty() && fs::is_directory(start, ec)) {
        IShellItem *folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.wstring().c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItemArray *items = nullptr;
        if (SUCCEEDED(dialog->GetResults(&items))) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                IShellItem *item = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &item))) {
                    if (fs::path path = ItemPath(item); !path.empty())
                        paths.push_back(std::move(path));
                    item->Release();
                }
            }
            items->Release();
        }
    }
    dialog->Release();
    return paths;
}

#else

void SDLCALL OnPicked(void *userdata, const char *const *filelist, int) {
    auto *holder = static_cast<std::shared_ptr<PickState> *>(userdata);
    {
        PickState &state = **holder;
        std::lock_guard lock(state.mutex);
        state.answered = true;
        state.paths.clear();
        state.error.clear();
        if (!filelist)
            state.error = SDL_GetError();
        else
            for (const char *const *f = filelist; *f; ++f)
                state.paths.push_back(rex::string::to_path(*f));
    }
    delete holder;
}

#endif

} // namespace

// --- The menu -------------------------------------------------------------------

struct LauncherMenu::Impl {
    // One drawn frame: where the picture is and how big, in screen pixels.
    // First, as the drawing functions take it.
    struct Frame {
        ImDrawList *draw;
        ImFont *font;
        float scale;
        ImVec2 origin;
        float text;
        float small_text;  // not "small": rpcndr.h defines it
        ImVec2 At(float x, float y) const { return ImVec2(origin.x + x * scale, origin.y + y * scale); }
    };

    std::string app_name;
    fs::path config_path;
    PlayFn on_play;
    QuitFn on_quit;
    bool visible = false;
    Page page = Page::kMain;

    // OPTIONS: the page chosen there (0 BACK), and its settings.
    struct SettingRow {
        std::string cvar;
        std::string label;
        bool key = false;  // a key bind: A waits for a key
        // One of the setting's values, listed as a row of its own (LANGUAGE): A
        // picks it, and the one set is marked.
        std::string choice;
    };
    int options_selected = 1;
    int settings_page = 0;
    std::vector<SettingRow> settings;
    int settings_selected = 0;  // 0 BACK, then the settings
    int settings_top = 0;       // the first setting shown
    std::string binding;        // the key bind waiting for a key
    float bind_wait = 0.0f;     // until the press that chose it is let go
    KeyCapture capture;
    Music music;
    MusicChoice music_choice;
    bool volume_drag = false;     // the mouse is setting the volume on its bars
    bool restart_tried = false;   // an installed update's restart (RestartForUpdate)
    bool restart_failed = false;

    setup::Paths paths;
    fs::path active_user;  // the save folder in use, fixed when reNut starts
    setup::Status status;
    Row selected = Row::kPlay;
    std::string message;
    bool message_is_error = false;
    bool job_was_running = false;

    uint64_t background = 0;           // overlay image ids
    uint64_t workshop_background = 0;  // the workshop's pages
    uint64_t logo = 0;
    ImVec2 logo_size{};
    uint64_t gears[std::size(kGears)] = {};
    ImU32 backdrop = IM_COL32(35, 80, 176, 255);

    Pads pads;
    uint32_t pad_held = 0;
    Repeat up, down, left, right, quieter, louder;

    std::shared_ptr<PickState> pick = std::make_shared<PickState>();
    bool picking = false;
#if defined(_WIN32)
    std::thread picker;
#endif

    // The workshop: its list, fetched once; the previews fetched as they are
    // looked at; the save folder downloads go into and what is in it already.
    std::vector<workshop::Entry> entries;
    bool index_loading = false;
    bool index_loaded = false;
    std::string index_error;
    fs::path save;
    std::map<std::string, std::string> installed;
    std::map<std::string, uint64_t> previews;  // by entry id: overlay image id
    std::set<std::string> previews_asked;
    std::string downloading;  // the entry id
    int ws_selected = 0;      // 0 BACK, 1 SHARE A VEHICLE, then the entries
    int ws_top = 0;           // the first grid row shown

    // SHARE A VEHICLE: the save's vehicles and the one chosen, the name and
    // description typed, the screenshot; the text field while one is edited.
    std::vector<workshop::LocalVehicle> vehicles;
    int vehicle_index = 0;
    std::string share_name;
    std::string share_description;
    fs::path screenshot;
    uint64_t screenshot_image = 0;  // overlay image id
    int share_selected = 1;
    ShareRow editing = ShareRow::kNone;
    std::string edit_buffer;
    bool focus_edit = false;
    http::Endpoint endpoint{workshop::kHost, 443, true};
    // Last, so it stops (and drops its pending answers, which point here)
    // before anything else goes.
    http::Worker web;

    ~Impl() {
        capture.Detach();
        setup::Cancel();
#if defined(_WIN32)
        if (picker.joinable())
            picker.join();
#endif
        ReleaseImages();
    }

    void ReleaseImages() {
        renut::gpu::overlay::RemoveImage(background);
        renut::gpu::overlay::RemoveImage(workshop_background);
        background = workshop_background = 0;
        renut::gpu::overlay::RemoveImage(std::exchange(logo, 0));
        for (uint64_t &gear : gears)
            renut::gpu::overlay::RemoveImage(std::exchange(gear, 0));
        for (const auto &[id, image] : previews)
            renut::gpu::overlay::RemoveImage(image);
        previews.clear();
        previews_asked.clear();
        renut::gpu::overlay::RemoveImage(screenshot_image);
        screenshot_image = 0;
    }

    void LoadBackground() {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba = workshop::DecodeImage(kBackgroundPng, sizeof(kBackgroundPng), &w, &h);
        if (rgba.empty())
            return;
        // The bars beside the picture, on a screen of another shape: its
        // corner's colour, averaged over the grid lines there.
        uint64_t sum[3] = {};
        const int n = std::min({w, h, 48});
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                for (int c = 0; c < 3; ++c)
                    sum[c] += rgba[(size_t(y) * w + x) * 4 + c];
        if (n > 0)
            backdrop = IM_COL32(int(sum[0] / (n * n)), int(sum[1] / (n * n)), int(sum[2] / (n * n)), 255);
        background = renut::gpu::overlay::AddImage(uint32_t(w), uint32_t(h), std::move(rgba));
        // The workshop's: drawn over the same place (it is the same picture).
        rgba = workshop::DecodeImage(kWorkshopBackgroundPng, sizeof(kWorkshopBackgroundPng), &w, &h);
        if (!rgba.empty())
            workshop_background = renut::gpu::overlay::AddImage(uint32_t(w), uint32_t(h), std::move(rgba));
        rgba = workshop::DecodeImage(kLogoPng, sizeof(kLogoPng), &w, &h);
        if (!rgba.empty()) {
            logo_size = ImVec2(float(w), float(h));
            logo = renut::gpu::overlay::AddImage(uint32_t(w), uint32_t(h), std::move(rgba));
        }
        for (size_t i = 0; i < std::size(kGears); ++i) {
            rgba = workshop::DecodeImage(kGears[i].png, kGears[i].size, &w, &h);
            if (!rgba.empty())
                gears[i] = renut::gpu::overlay::AddImage(uint32_t(w), uint32_t(h), std::move(rgba));
        }
    }

    // The background over the whole screen: the picture in the middle at the menu's
    // scale, tiled outwards with mirrored copies.
    void DrawBackground(const Frame &f, uint64_t picture) const {
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        const float tw = kImageWidth * f.scale, th = kImageHeight * f.scale;
        if (!picture || tw < 1.0f || th < 1.0f)
            return;
        const int i0 = int(std::floor(-f.origin.x / tw)), i1 = int(std::ceil((screen.x - f.origin.x) / tw));
        const int j0 = int(std::floor(-f.origin.y / th)), j1 = int(std::ceil((screen.y - f.origin.y) / th));
        for (int j = j0; j < j1; ++j) {
            for (int i = i0; i < i1; ++i) {
                const ImVec2 a(f.origin.x + float(i) * tw, f.origin.y + float(j) * th);
                const bool flip_x = (i & 1) != 0, flip_y = (j & 1) != 0;
                f.draw->AddImage(ImTextureRef(ImTextureID(picture)), a, ImVec2(a.x + tw, a.y + th),
                                 ImVec2(flip_x ? 1.0f : 0.0f, flip_y ? 1.0f : 0.0f),
                                 ImVec2(flip_x ? 0.0f : 1.0f, flip_y ? 0.0f : 1.0f));
            }
        }
    }

    // A place in the picture's art (1920 x 1080) on the screen, kept from the
    // screen's left or right and top or bottom, at the menu's scale.
    ImVec2 FromCorner(const Frame &f, float x, float y, bool right, bool bottom) const {
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        const float unit = f.scale * kImageWidth / kArtWidth;
        return ImVec2(right ? screen.x - (kArtWidth - x) * unit : x * unit,
                      bottom ? screen.y - (kArtHeight - y) * unit : y * unit);
    }

    // The logo in the top left corner, and the gears in theirs, each turned
    // about its centre by the time the menu has shown.
    void DrawCorners(const Frame &f) const {
        const float unit = f.scale * kImageWidth / kArtWidth;
        if (logo) {
            const ImVec2 a = FromCorner(f, kLogoLeft, kLogoTop, false, false);
            f.draw->AddImage(ImTextureRef(ImTextureID(logo)), a,
                             ImVec2(a.x + logo_size.x * unit, a.y + logo_size.y * unit));
        }
        const double seconds = ImGui::GetTime();
        for (size_t i = 0; i < std::size(kGears); ++i) {
            if (!gears[i])
                continue;
            const Gear &g = kGears[i];
            const ImVec2 c = FromCorner(f, g.x, g.y, g.right, g.bottom);
            const float r = g.radius * unit;
            const double turn = std::fmod(g.start + seconds * g.speed, 360.0) * (3.14159265358979323846 / 180.0);
            const float cs = float(std::cos(turn)) * r, sn = float(std::sin(turn)) * r;
            // The sprite's corners (-1,-1), (1,-1), (1,1), (-1,1) turned (y is down: clockwise).
            const auto corner = [&](float u, float v) { return ImVec2(c.x + u * cs - v * sn, c.y + u * sn + v * cs); };
            f.draw->AddImageQuad(ImTextureRef(ImTextureID(gears[i])), corner(-1, -1), corner(1, -1), corner(1, 1),
                                 corner(-1, 1));
        }
    }

    bool Ready() const { return status.game && status.update; }

    void Refresh() {
        status = setup::Check(paths);
    }

    void Say(std::string text, bool error = false) {
        message = std::move(text);
        message_is_error = error;
    }

    bool RowEnabled(Row row, bool busy) const {
        if (row == Row::kQuit)
            return true;
        if (picking)
            return false;
        if (row == Row::kWorkshop || row == Row::kOptions)
            return true;
        if (busy)
            return false;
        // There is one DLC: once it's in, there is nothing more to add.
        if (row == Row::kDlc)
            return !status.dlc;
        return row != Row::kPlay || Ready();
    }

    void OpenPicker(Row row, PickKind kind, const fs::path &start) {
        picking = true;
        {
            std::lock_guard lock(pick->mutex);
            pick->answered = false;
            pick->row = row;
        }
#if defined(_WIN32)
        if (picker.joinable())
            picker.join();
        plume::RenderWindow window{};
        HWND owner = renut::gpu::GetNativeRenderWindow(window) ? static_cast<HWND>(window) : nullptr;
        picker = std::thread([state = pick, kind, start, owner] {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            std::vector<fs::path> paths = RunPicker(kind, start, owner);
            if (SUCCEEDED(com))
                CoUninitialize();
            std::lock_guard lock(state->mutex);
            state->paths = std::move(paths);
            state->error.clear();
            state->answered = true;
        });
#else
        std::error_code ec;
        const std::string where = !start.empty() && fs::is_directory(start, ec) ? Utf8(start) : std::string();
        auto *holder = new std::shared_ptr<PickState>(pick);
        SDL_Window *window = renut::gpu::GetGameSdlWindow();
        if (kind == PickKind::kFolder) {
            SDL_ShowOpenFolderDialog(OnPicked, holder, window, where.empty() ? nullptr : where.c_str(), false);
        } else if (kind == PickKind::kDiscImage) {
            static const SDL_DialogFileFilter filters[] = {{"Disc images (*.iso)", "iso"}, {"All files", "*"}};
            SDL_ShowOpenFileDialog(OnPicked, holder, window, filters, 2, where.empty() ? nullptr : where.c_str(),
                                   false);
        } else if (kind == PickKind::kImage) {
            static const SDL_DialogFileFilter filters[] = {{"Pictures (*.png, *.jpg)", "png;jpg;jpeg"}};
            SDL_ShowOpenFileDialog(OnPicked, holder, window, filters, 1, where.empty() ? nullptr : where.c_str(),
                                   false);
        } else {
            SDL_ShowOpenFileDialog(OnPicked, holder, window, nullptr, 0, where.empty() ? nullptr : where.c_str(),
                                   false);
        }
#endif
    }

    void TakePick() {
        Row row;
        std::vector<fs::path> picked;
        std::string error;
        {
            std::lock_guard lock(pick->mutex);
            if (!pick->answered)
                return;
            pick->answered = false;
            row = pick->row;
            picked = std::move(pick->paths);
            error = std::move(pick->error);
        }
        picking = false;
        if (!error.empty()) {
            Say("No file picker (" + error + ").", true);
            return;
        }
        if (picked.empty())
            return;
        switch (row) {
        case Row::kDisc:
            if (setup::StartExtractDisc(picked[0], paths.game))
                Say("");
            break;
        case Row::kTitleUpdate:
            if (setup::StartExtractTitleUpdate(picked[0], paths.game, paths.update))
                Say("");
            break;
        case Row::kDlc:
            if (setup::StartAddDlc(std::move(picked)))
                Say("");
            break;
        case Row::kGameFolder: {
            // An update folder that went with the game folder moves with it.
            if (paths.update == paths.game / "update")
                paths.update = picked[0] / "update";
            paths.game = picked[0];
            Changed();
            if (status.other_region)
                Say("That folder has another region's Nuts & Bolts: reNut runs the US (NTSC) version.", true);
            else
                Say(status.game ? "The game's files are there." : "Extract the game disc into it.", false);
            break;
        }
        case Row::kUpdateFolder:
            paths.update = picked[0];
            Changed();
            break;
        case Row::kSaveFolder:
            paths.user = picked[0];
            Changed();
            if (!SameFolder(paths.user, active_user))
                Say("reNut restarts with the new save folder when PLAY is pressed.");
            break;
        case Row::kScreenshot:
            SetScreenshot(picked[0]);
            break;
        default:
            break;
        }
    }

    void Changed() {
        setup::SavePaths(app_name, paths);
        Refresh();
    }

    void Activate(Row row, bool busy) {
        if (!RowEnabled(row, busy)) {
            if (row == Row::kPlay && !busy)
                Say(status.other_region ? "The game folder has another region's Nuts & Bolts: reNut runs the US "
                                          "(NTSC) version. Extract the US disc, or choose a folder that has it."
                    : !status.game      ? "Extract the game disc first (or choose a game folder that has it)."
                                        : "Extract Title Update #3 first: reNut runs on it.",
                    true);
            return;
        }
        switch (row) {
        case Row::kPlay: {
            setup::SavePaths(app_name, paths);
            visible = false;
            pads.Close();
            capture.Detach();
            music.Stop();
            ReleaseImages();
            if (on_play)
                on_play(paths);
            break;
        }
        case Row::kDisc:
            OpenPicker(row, PickKind::kDiscImage, {});
            break;
        case Row::kTitleUpdate:
            OpenPicker(row, PickKind::kPackage, {});
            break;
        case Row::kDlc:
            OpenPicker(row, PickKind::kDlc, {});
            break;
        case Row::kWorkshop:
            OpenWorkshop();
            break;
        case Row::kGameFolder:
            OpenPicker(row, PickKind::kFolder, paths.game);
            break;
        case Row::kUpdateFolder:
            OpenPicker(row, PickKind::kFolder, paths.update);
            break;
        case Row::kSaveFolder:
            OpenPicker(row, PickKind::kFolder, paths.user);
            break;
        case Row::kOptions:
            page = Page::kOptions;
            options_selected = 1;
            Say("");
            break;
        case Row::kQuit:
            if (on_quit)
                on_quit();
            break;
        default:
            break;
        }
    }

    // What the row says on its right.
    std::string Value(Row row, ImU32 &colour) const {
        colour = kWhite;
        switch (row) {
        case Row::kDisc:
            if (status.game)
                return "DONE";
            if (status.other_region) {
                colour = kErrorColour;
                return "WRONG REGION";
            }
            colour = kNeeded;
            return "NEEDED";
        case Row::kTitleUpdate:
            if (status.update)
                return "DONE";
            colour = kNeeded;
            return "NEEDED";
        case Row::kDlc:
            return status.dlc ? "ADDED" : "OPTIONAL";
        case Row::kGameFolder:
            return Utf8(paths.game);
        case Row::kUpdateFolder:
            return Utf8(paths.update);
        case Row::kSaveFolder:
            return Utf8(paths.user);
        default:
            return {};
        }
    }

    // --- The workshop -----------------------------------------------------------

    void OpenWorkshop() {
        page = Page::kWorkshop;
        ws_selected = 0;
        ws_top = 0;
        save = workshop::SaveFolder(paths.user);
        installed = workshop::Installed(save);
        Say("");
        if (!index_loaded && !index_loading)
            FetchIndex();
    }

    void FetchIndex() {
        index_loading = true;
        index_error.clear();
        // raw.githubusercontent.com caches a file for minutes; a query string
        // gets the list as it is now.
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        web.Post(endpoint, "GET", std::string(workshop::kRoot) + "index.json?t=" + std::to_string(now), "", "",
                 [this](const http::Response &r) {
                     index_loading = false;
                     if (r.status != 200) {
                         index_error = r.status ? std::format("The workshop answered {}; try again later.", r.status)
                                                : "Can't reach the workshop (no connection?).";
                         return;
                     }
                     entries = workshop::ParseIndex(r.body, &index_error);
                     index_loaded = index_error.empty();
                 });
    }

    const workshop::Entry *SelectedEntry() const {
        const int i = ws_selected - kWorkshopFixed;
        return i >= 0 && i < int(entries.size()) ? &entries[size_t(i)] : nullptr;
    }

    void FetchPreview(const workshop::Entry &e) {
        if (e.preview.empty() || !previews_asked.insert(e.id).second)
            return;
        web.Post(endpoint, "GET", std::string(workshop::kRoot) + e.preview, "", "",
                 [this, id = e.id](const http::Response &r) {
                     if (r.status != 200 || !visible)
                         return;
                     int w = 0, h = 0;
                     std::vector<uint8_t> rgba = workshop::DecodeImage(r.body.data(), r.body.size(), &w, &h);
                     if (!rgba.empty())
                         previews[id] = renut::gpu::overlay::AddImage(uint32_t(w), uint32_t(h), std::move(rgba));
                 });
    }

    void ActivateEntry() {
        if (ws_selected == 1) {
            OpenShare();
            return;
        }
        const workshop::Entry *e = SelectedEntry();
        if (!e) {
            page = Page::kMain;
            Say("");
            return;
        }
        if (installed.count(e->id)) {
            workshop::Remove(save, e->id);
            installed = workshop::Installed(save);
            Say("Took " + e->name + " back out of your vehicles.");
            return;
        }
        if (!downloading.empty())
            return;
        downloading = e->id;
        Say("");
        web.Post(endpoint, "GET", std::string(workshop::kRoot) + e->blueprint, "", "",
                 [this, entry = *e](const http::Response &r) {
                     downloading.clear();
                     if (r.status != 200) {
                         Say(r.status ? std::format("The workshop answered {}; try again later.", r.status)
                                      : "Can't reach the workshop (no connection?).",
                             true);
                         return;
                     }
                     std::string error;
                     if (!workshop::Install(save, entry, r.body, &error)) {
                         Say(error, true);
                         return;
                     }
                     installed = workshop::Installed(save);
                     Say(entry.name + " is in your vehicles: build it from the garage's blueprints.");
                 });
    }

    // --- OPTIONS -----------------------------------------------------------------

    void OpenSettings(int which) {
        settings.clear();
        const auto add = [&](const Setting *list, size_t count) {
            for (size_t i = 0; i < count; ++i)
                if (FindSetting(list[i].cvar))
                    settings.push_back({list[i].cvar, Upper(list[i].label), false});
        };
        if (which == 0) {
            if (FindSetting(kLanguageSetting))
                for (size_t i = 0; i < std::size(kLanguages); ++i)
                    settings.push_back({kLanguageSetting, Upper(kLanguages[i]), false, std::to_string(i + 1)});
        } else if (which == 1) {
            add(kGraphicsSettings, std::size(kGraphicsSettings));
        } else {
            add(kInputSettings, std::size(kInputSettings));
            for (const auto &e : renut::graphics_menu::CategorizedSettings()) {
                const std::string name = e.name;
                if (name.rfind(kKeyPrefix, 0) != 0 || std::string(e.category) != kInputCategory)
                    continue;
                const std::string full = std::string(renut::settings::kScope) + "." + name;
                if (const rex::SettingBase *setting = FindSetting(full))
                    settings.push_back({full, KeyLabel(*setting, name), true});
            }
        }
        settings_page = which;
        settings_selected = settings.empty() ? 0 : 1;
        // A list of choices opens on the one set.
        for (size_t i = 0; i < settings.size(); ++i)
            if (!settings[i].choice.empty() && ValueOf(settings[i].cvar) == settings[i].choice)
                settings_selected = int(i) + 1;
        settings_top = 0;
        page = Page::kSettings;
        Say("");
    }

    const SettingRow *SelectedSetting() const {
        const int i = settings_selected - 1;
        return i >= 0 && i < int(settings.size()) ? &settings[size_t(i)] : nullptr;
    }

    void DrawOptions(const Frame &f, int step, bool &activate) {
        const int total = kOptionPageCount + 1;  // BACK and the pages
        if (step)
            options_selected = (options_selected + step + total) % total;
        for (int i = 0; i < total; ++i)
            Mouse(f, i, options_selected, activate);
        DrawRow(f, 0, options_selected == 0, true, "BACK", "OPTIONS", kDim, false);
        for (int i = 0; i < kOptionPageCount; ++i)
            DrawRow(f, i + 1, options_selected == i + 1, true, kOptionPages[i], "", kWhite, false);
    }

    void ActivateOptions() {
        if (options_selected == 0) {
            page = Page::kMain;
            return;
        }
        OpenSettings(options_selected - 1);
    }

    void DrawSettings(const Frame &f, int step, int side, bool &activate) {
        const int count = int(settings.size());
        const int total = count + 1;  // BACK and the settings
        if (step)
            settings_selected = (settings_selected + step + total) % total;
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
            settings_top -= int(wheel);
        if ((step || settings_selected > settings_top + kSettingRows) && settings_selected > 0) {
            settings_top = std::min(settings_top, settings_selected - 1);
            settings_top = std::max(settings_top, settings_selected - kSettingRows);
        }
        settings_top = std::clamp(settings_top, 0, std::max(0, count - kSettingRows));

        int selection = settings_selected == 0 ? 0 : settings_selected - settings_top;
        const int shown = std::min(kSettingRows, count - settings_top);
        for (int i = 0; i <= shown; ++i)
            Mouse(f, i, selection, activate);
        settings_selected = selection == 0 ? 0 : settings_top + selection;

        // Left and right change the selected setting either way.
        if (side && binding.empty())
            if (const SettingRow *row = SelectedSetting(); row && !row->key && row->choice.empty())
                if (const rex::SettingBase *entry = FindSetting(row->cvar))
                    StepSetting(*entry, side, config_path);

        DrawRow(f, 0, settings_selected == 0, true, "BACK", kOptionPages[settings_page], kDim, false);
        for (int i = 0; i < shown; ++i) {
            const SettingRow &row = settings[size_t(settings_top + i)];
            const rex::SettingBase *entry = FindSetting(row.cvar);
            const bool waiting = row.cvar == binding;
            std::string value = waiting ? "PRESS A KEY" : entry ? SettingValue(*entry, row.key) : "";
            if (!row.choice.empty())
                value = ValueOf(row.cvar) == row.choice ? "SELECTED" : "";
            DrawRow(f, i + 1, settings_selected == settings_top + i + 1, true, row.label, value,
                    waiting ? kNeeded : kWhite, false);
        }
        const float arrow_x = kRowRight + 14.0f;
        if (settings_top > 0) {
            const ImVec2 p = f.At(arrow_x, kSlotTop[1] + 4.0f);
            f.draw->AddTriangleFilled(ImVec2(p.x - 6 * f.scale, p.y + 6 * f.scale),
                                      ImVec2(p.x + 6 * f.scale, p.y + 6 * f.scale), p, kDim);
        }
        if (settings_top + shown < count) {
            const ImVec2 p = f.At(arrow_x, kSlotTop[kSettingRows] + kRowHeight - 4.0f);
            f.draw->AddTriangleFilled(ImVec2(p.x - 6 * f.scale, p.y - 6 * f.scale),
                                      ImVec2(p.x + 6 * f.scale, p.y - 6 * f.scale), p, kDim);
        }

        // Under the list: what the selected setting does, or how to bind a key.
        const SettingRow *row = SelectedSetting();
        std::string about;
        if (!binding.empty()) {
            about = "Press a key or mouse button for it. Escape (or B) cancels; Delete unbinds it.";
        } else if (row) {
            const rex::SettingBase *entry = FindSetting(row->cvar);
            about = row->cvar == kLanguageSetting ? kLanguageAbout : entry ? entry->GetHelp() : "";
            if (about.size() > 240)
                about = about.substr(0, 237) + "...";
            const rex::SettingLife life = entry ? entry->GetConstraints().life : rex::SettingLife::kLive;
            if (entry && row->cvar != kLanguageSetting && life == rex::SettingLife::kRestart &&
                about.find("restart") == std::string::npos)
                about += " Takes effect after a restart.";
            if (entry && row->cvar != kLanguageSetting && life == rex::SettingLife::kStartupOnly)
                about += " Changing it restarts reNut.";
        }
        f.draw->AddText(f.font, f.small_text, f.At(kColumnLeft + 30.0f, kInfoTop), binding.empty() ? kDim : kNeeded,
                        about.c_str(), nullptr, kTextWidth * f.scale);
    }

    void ActivateSetting() {
        const SettingRow *row = SelectedSetting();
        if (!row) {
            page = Page::kOptions;
            return;
        }
        if (row->key) {
            // Waits for the key or click that chose the row to be let go.
            binding = row->cvar;
            bind_wait = 0.3f;
            return;
        }
        if (!row->choice.empty()) {
            if (REX_SETTINGS().SetText(row->cvar, row->choice) && !config_path.empty())
                (void)REX_SETTINGS().Save(config_path);
            return;
        }
        if (const rex::SettingBase *entry = FindSetting(row->cvar))
            StepSetting(*entry, 1, config_path);
    }

    // A setting's value as text, empty when it isn't registered.
    static std::string ValueOf(const std::string &name) {
        const rex::SettingBase *setting = FindSetting(name);
        return setting ? setting->GetText() : std::string();
    }

    // While a key bind waits: takes the key (or cancels). True while it holds the input,
    // from the press that chose the row until a moment after the bound key.
    bool UpdateBinding(float dt, uint32_t pressed) {
        if (binding.empty()) {
            if (bind_wait <= 0.0f)
                return false;
            bind_wait -= dt;
            return true;
        }
        if (bind_wait > 0.0f) {
            bind_wait -= dt;
            if (bind_wait <= 0.0f)
                capture.Arm();
            return true;
        }
        const rex::VirtualKey key = capture.Take();
        std::string name;
        bool done = (pressed & kPadB) || key == rex::VirtualKey::kEscape;
        if (!done && key == rex::VirtualKey::kDelete) {
            done = true;
            (void)REX_SETTINGS().SetText(binding, "");
        } else if (!done && key != rex::VirtualKey::kNone) {
            const rex::hid::IInputSystem *input = REX_INTERFACE(rex::hid::IInputSystem);
            name = input ? input->VirtualKeyToString(key) : std::string();
            if (!name.empty()) {
                done = true;
                (void)REX_SETTINGS().SetText(binding, name);
            }
        }
        if (done) {
            if (!config_path.empty())
                (void)REX_SETTINGS().Save(config_path);
            capture.Disarm();
            binding.clear();
            bind_wait = 0.25f;
        }
        return true;
    }

    // --- Drawing ----------------------------------------------------------------

    // One menu row: the highlight and arrow when selected, the label, and a
    // value on its right. In the column, or as wide from 'left'.
    void DrawRow(const Frame &f, int index, bool is_selected, bool enabled, const std::string &label,
                 std::string value, ImU32 value_colour, bool value_is_path, float left = kColumnLeft) const {
        const ImVec2 a = f.At(left, kSlotTop[index]);
        const ImVec2 b = f.At(left + kRowRight - kColumnLeft, kSlotTop[index] + kRowHeight);
        if (is_selected) {
            f.draw->AddRectFilled(a, b, kHighlight, 6.0f * f.scale);
            const float mid = (a.y + b.y) * 0.5f;
            const float x = a.x + 10.0f * f.scale;
            f.draw->AddTriangleFilled(ImVec2(x, mid - 7.0f * f.scale), ImVec2(x, mid + 7.0f * f.scale),
                                      ImVec2(x + 11.0f * f.scale, mid), kWhite);
        }
        const float label_x = a.x + 30.0f * f.scale;
        const float right = b.x - 12.0f * f.scale;
        const float value_size = value_is_path ? f.small_text : f.text;
        float value_width = 0.0f;
        if (!value.empty() && !value_is_path)
            value_width = f.font->CalcTextSizeA(value_size, FLT_MAX, 0.0f, value.c_str()).x;
        const std::string shown =
            FitEnd(f.font, f.text, label, right - label_x - (value_width ? value_width + 24.0f * f.scale : 0.0f));
        f.draw->AddText(f.font, f.text, ImVec2(label_x, (a.y + b.y - f.text) * 0.5f), enabled ? kWhite : kDim,
                        shown.c_str());
        if (value.empty())
            return;
        if (value_is_path) {
            const float label_end = label_x + f.font->CalcTextSizeA(f.text, FLT_MAX, 0.0f, shown.c_str()).x;
            value = FitStart(f.font, value_size, value, right - label_end - 24.0f * f.scale);
            value_width = f.font->CalcTextSizeA(value_size, FLT_MAX, 0.0f, value.c_str()).x;
        }
        f.draw->AddText(f.font, value_size, ImVec2(right - value_width, (a.y + b.y - value_size) * 0.5f),
                        enabled || value_is_path ? value_colour : kDim, value.c_str());
    }

    // The row the mouse is over (moving), or clicked.
    void Mouse(const Frame &f, int index, int &selection, bool &activate, float left = kColumnLeft) const {
        MouseRect(f.At(left, kSlotTop[index]), f.At(left + kRowRight - kColumnLeft, kSlotTop[index] + kRowHeight),
                  index, selection, activate);
    }

    // The same for anything else selectable, between screen points a and b.
    void MouseRect(ImVec2 a, ImVec2 b, int index, int &selection, bool &activate) const {
        const ImGuiIO &io = ImGui::GetIO();
        if (picking || !binding.empty() || bind_wait > 0.0f || !ImGui::IsMouseHoveringRect(a, b, false))
            return;
        if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)
            selection = index;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            selection = index;
            activate = true;
        }
    }

    // The music's corner: volume bars (click or drag to set) and the speaker icon;
    // true when the speaker is clicked.
    bool DrawMusic(const Frame &f, bool enabled) {
        enabled = enabled && !picking && binding.empty() && bind_wait <= 0.0f;
        const ImVec2 bars_a = f.At(kVolumeLeft - kVolumeSilent, kMuteTop);
        const ImVec2 bars_b = f.At(kVolumeRight, kMuteTop + kMuteSize);
        const ImVec2 a = f.At(kMuteLeft, kMuteTop);
        const ImVec2 b = f.At(kMuteLeft + kMuteSize, kMuteTop + kMuteSize);
        const bool over_bars = enabled && ImGui::IsMouseHoveringRect(bars_a, bars_b, false);
        const bool hovered = enabled && ImGui::IsMouseHoveringRect(a, b, false);
        if (over_bars && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            volume_drag = true;
        if (volume_drag) {
            if (enabled && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                // The bar under the mouse and those before it; left of them, none.
                const float t = (ImGui::GetIO().MousePos.x - f.At(kVolumeLeft, 0.0f).x) /
                                ((kVolumeRight - kVolumeLeft) * f.scale);
                const int bars = t <= 0.0f ? 0 : std::min(int(t * kVolumeBars), kVolumeBars - 1) + 1;
                SetMusicVolume(double(bars) / kVolumeBars, false);
            } else {
                // Saved once, when let go.
                volume_drag = false;
                SaveMusicChoice(music_choice);
            }
        }
        if (over_bars || volume_drag)
            f.draw->AddRectFilled(bars_a, bars_b, kHighlight, 6.0f * f.scale);
        if (hovered)
            f.draw->AddRectFilled(a, b, kHighlight, 6.0f * f.scale);

        const bool muted = !music_choice.on;
        const double volume = music_choice.volume;
        const ImU32 colour = muted ? kDim : kWhite;
        const int lit = int(std::lround(volume * kVolumeBars));
        const float bar_width = (kVolumeRight - kVolumeLeft - kVolumeBarGap * (kVolumeBars - 1)) / kVolumeBars;
        for (int i = 0; i < kVolumeBars; ++i) {
            const float x = kVolumeLeft + i * (bar_width + kVolumeBarGap);
            const float height = 6.0f + 20.0f * float(i) / float(kVolumeBars - 1);
            f.draw->AddRectFilled(f.At(x, kVolumeBottom - height), f.At(x + bar_width, kVolumeBottom),
                                  i < lit ? colour : kVolumeUnlit, 1.5f * f.scale);
        }

        const auto at = [&](float x, float y) { return f.At(kMuteLeft + x, kMuteTop + y); };
        const ImVec2 cone[] = {at(9.0f, 17.0f), at(16.0f, 17.0f), at(25.0f, 9.0f), at(25.0f, 37.0f),
                               at(16.0f, 29.0f), at(9.0f, 29.0f)};
        f.draw->AddConcavePolyFilled(cone, int(std::size(cone)), colour);
        const float line = std::max(1.0f, 2.5f * f.scale);
        if (muted) {
            f.draw->AddLine(at(30.0f, 17.0f), at(39.0f, 29.0f), colour, line);
            f.draw->AddLine(at(39.0f, 17.0f), at(30.0f, 29.0f), colour, line);
        } else {
            // Silent: no waves; up to half way: one; louder: two.
            const int waves = lit == 0 ? 0 : volume < 0.5 ? 1 : 2;
            for (int i = 0; i < waves; ++i) {
                f.draw->PathArcTo(at(25.0f, 23.0f), (i ? 13.0f : 7.0f) * f.scale, -0.85f, 0.85f);
                f.draw->PathStroke(colour, 0, line);
            }
        }
        return hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    }

    void ToggleMusic() {
        music_choice.on = !music_choice.on;
        SaveMusicChoice(music_choice);
        music.SetMuted(!music_choice.on);
    }

    // Sets the music to 'volume' (0 to 1, in the bars' steps) and unmutes it; saved to
    // launcher.cfg unless 'save' is false (a drag, saved when let go).
    void SetMusicVolume(double volume, bool save) {
        volume = std::round(std::clamp(volume, 0.0, 1.0) * kVolumeBars) / kVolumeBars;
        bool changed = false;
        if (volume != music_choice.volume) {
            music_choice.volume = volume;
            music.SetVolume(float(volume));
            changed = true;
        }
        if (!music_choice.on) {
            music_choice.on = true;
            music.SetMuted(false);
            changed = true;
        }
        if (changed && save)
            SaveMusicChoice(music_choice);
    }

    void DrawMessage(const Frame &f, float y) const {
        if (!message.empty())
            f.draw->AddText(f.font, f.small_text, f.At(kColumnLeft + 30.0f, y), message_is_error ? kErrorColour : kWhite,
                            message.c_str(), nullptr, kTextWidth * f.scale);
    }

    // The update (app/updater.h), one line at the bottom of the menu.
    void DrawUpdate(const Frame &f) const {
        const renut::update::Status u = renut::update::Get();
        std::string line;
        ImU32 colour = kDim;
        switch (u.state) {
        case renut::update::State::kDownloading: line = "DOWNLOADING " + Upper(u.release) + "..."; break;
        case renut::update::State::kInstalling: line = "INSTALLING " + Upper(u.release) + "..."; break;
        case renut::update::State::kReady:
            line = restart_failed ? Upper(u.release) + " IS INSTALLED: RESTART RENUT TO PLAY IT"
                                  : "UPDATED TO " + Upper(u.release) + ": RESTARTING...";
            colour = kNeeded;
            break;
        case renut::update::State::kFailed:
            line = "COULD NOT UPDATE: " + Upper(u.error);
            colour = kErrorColour;
            break;
        default: return;
        }
        line = FitEnd(f.font, f.small_text, line, kTextWidth * f.scale);
        f.draw->AddText(f.font, f.small_text, f.At(kColumnLeft + 30.0f, kImageHeight - 34.0f), colour, line.c_str());
    }

    // An installed update starts at once, while the menu is idle: not over a
    // job, a picker, a key bind or typing, and not once the game has started.
    void RestartForUpdate(bool busy) {
        if (restart_tried || renut::update::Get().state != renut::update::State::kReady)
            return;
        if (page != Page::kMain || busy || picking || !binding.empty() || editing != ShareRow::kNone)
            return;
        restart_tried = true;
        if (!renut::update::Restart()) {
            restart_failed = true;
            return;
        }
        if (on_quit)
            on_quit();
    }

    void DrawMain(const Frame &f, int step, bool &activate, const setup::Progress &progress) {
        const bool busy = progress.running;
        const int count = int(Row::kCount);
        if (step)
            selected = Row((int(selected) + step + count) % count);
        int selection = int(selected);
        for (int i = 0; i < count; ++i)
            Mouse(f, i, selection, activate);
        selected = Row(selection);
        for (int i = 0; i < count; ++i) {
            ImU32 colour;
            const Row row = Row(i);
            const bool path = row >= Row::kGameFolder && row <= Row::kSaveFolder;
            // Value sets the colour: called before DrawRow reads it.
            std::string value = Value(row, colour);
            DrawRow(f, i, row == selected, RowEnabled(row, busy), kRowLabels[i], std::move(value), colour, path);
        }

        // Under the menu: the running job, or what the last one said; at the
        // bottom, reNut's own update.
        const float info_y = kInfoTop;
        if (!busy) {
            DrawMessage(f, info_y + 4.0f);
            DrawUpdate(f);
            return;
        }
        const ImVec2 a = f.At(kColumnLeft + 30.0f, info_y);
        const ImVec2 b = f.At(kRowRight - 10.0f, info_y + 14.0f);
        const float done = progress.total ? float(double(progress.bytes) / double(progress.total)) : 0.0f;
        f.draw->AddRect(a, b, kWhite, 3.0f * f.scale, 0, std::max(1.0f, 2.0f * f.scale));
        f.draw->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * std::clamp(done, 0.0f, 1.0f), b.y), kWhite, 3.0f * f.scale);
        const char *what = progress.job == setup::Job::kDisc          ? "EXTRACTING THE GAME DISC"
                           : progress.job == setup::Job::kTitleUpdate ? "EXTRACTING THE TITLE UPDATE"
                                                                      : "ADDING DLC";
        const std::string line = std::format("{}  {}%  ({} of {})", what, int(done * 100.0f),
                                             Gigabytes(progress.bytes), Gigabytes(progress.total));
        const float room = kTextWidth * f.scale;
        f.draw->AddText(f.font, f.small_text, f.At(kColumnLeft + 30.0f, info_y + 22.0f), kWhite, line.c_str());
        const std::string file = FitStart(f.font, f.small_text, progress.file, room);
        f.draw->AddText(f.font, f.small_text, f.At(kColumnLeft + 30.0f, info_y + 46.0f), kDim, file.c_str());
        f.draw->AddText(f.font, f.small_text, f.At(kColumnLeft + 30.0f, info_y + 70.0f), kWhite, "ESC / B  STOP");
    }

    // A picture pinned over the photos on the right, with its caption under
    // it; a grey card while there is none yet.
    void DrawPhoto(const Frame &f, uint64_t image, const std::string &caption) const {
        const float photo_height = kPhotoWidth * 9.0f / 16.0f;
        const ImVec2 a = f.At(kPhotoLeft - kPhotoBorder, kPhotoTop - kPhotoBorder);
        const ImVec2 b = f.At(kPhotoLeft + kPhotoWidth + kPhotoBorder, kPhotoTop + photo_height + kPhotoBorder * 3.5f);
        f.draw->AddRectFilled(ImVec2(a.x + 6 * f.scale, a.y + 8 * f.scale), ImVec2(b.x + 6 * f.scale, b.y + 8 * f.scale),
                              kShadow, 4.0f * f.scale);
        f.draw->AddRectFilled(a, b, IM_COL32(248, 246, 240, 255), 4.0f * f.scale);
        const ImVec2 pa = f.At(kPhotoLeft, kPhotoTop);
        const ImVec2 pb = f.At(kPhotoLeft + kPhotoWidth, kPhotoTop + photo_height);
        if (image)
            f.draw->AddImage(ImTextureRef(ImTextureID(image)), pa, pb);
        else
            f.draw->AddRectFilled(pa, pb, IM_COL32(60, 60, 60, 255));
        const std::string text = FitEnd(f.font, f.small_text, caption, kPhotoWidth * f.scale);
        f.draw->AddText(f.font, f.small_text, f.At(kPhotoLeft, kPhotoTop + photo_height + 8.0f),
                        IM_COL32(40, 40, 48, 255), text.c_str());
    }

    // Up and down arrows at x, by the top and bottom of a scrolled list, when
    // there is more of it above or below.
    void DrawScrollArrows(const Frame &f, float x, float top, float bottom, bool above, bool below) const {
        if (above) {
            const ImVec2 p = f.At(x, top + 4.0f);
            f.draw->AddTriangleFilled(ImVec2(p.x - 6 * f.scale, p.y + 6 * f.scale),
                                      ImVec2(p.x + 6 * f.scale, p.y + 6 * f.scale), p, kDim);
        }
        if (below) {
            const ImVec2 p = f.At(x, bottom - 4.0f);
            f.draw->AddTriangleFilled(ImVec2(p.x - 6 * f.scale, p.y - 6 * f.scale),
                                      ImVec2(p.x + 6 * f.scale, p.y - 6 * f.scale), p, kDim);
        }
    }

    // A grid card's top left, in the picture's pixels: the 'slot'th shown.
    static ImVec2 CardAt(int slot) {
        return ImVec2(kGridLeft + float(slot % kGridColumns) * (kCardWidth + kGridGap),
                      kGridTop + float(slot / kGridColumns) * (kCardHeight + kGridRowGap));
    }

    // A vehicle's card: its picture and name in a white border, ringed when selected
    // and tagged when added or downloading.
    void DrawCard(const Frame &f, ImVec2 at, const workshop::Entry &e, bool is_selected) const {
        const ImVec2 a = f.At(at.x, at.y);
        const ImVec2 b = f.At(at.x + kCardWidth, at.y + kCardHeight);
        f.draw->AddRectFilled(ImVec2(a.x + 5 * f.scale, a.y + 6 * f.scale), ImVec2(b.x + 5 * f.scale, b.y + 6 * f.scale),
                              kShadow, 4.0f * f.scale);
        f.draw->AddRectFilled(a, b, IM_COL32(248, 246, 240, 255), 4.0f * f.scale);
        const ImVec2 pa = f.At(at.x + kCardBorder, at.y + kCardBorder);
        const ImVec2 pb = f.At(at.x + kCardWidth - kCardBorder, at.y + kCardBorder + kCardPictureHeight);
        const auto it = previews.find(e.id);
        if (it != previews.end())
            f.draw->AddImage(ImTextureRef(ImTextureID(it->second)), pa, pb);
        else
            f.draw->AddRectFilled(pa, pb, IM_COL32(60, 60, 60, 255));
        const std::string name =
            FitEnd(f.font, f.small_text, Upper(e.name), (kCardWidth - 2.0f * kCardBorder) * f.scale);
        f.draw->AddText(f.font, f.small_text, f.At(at.x + kCardBorder, at.y + kCardBorder + kCardPictureHeight + 5.0f),
                        IM_COL32(40, 40, 48, 255), name.c_str());
        const char *tag = downloading == e.id ? "..." : installed.count(e.id) ? "ADDED" : nullptr;
        if (tag) {
            const float w = f.font->CalcTextSizeA(f.small_text, FLT_MAX, 0.0f, tag).x;
            const ImVec2 ta(pb.x - w - 14.0f * f.scale, pa.y + 6.0f * f.scale);
            const ImVec2 tb(pb.x - 6.0f * f.scale, ta.y + f.small_text + 6.0f * f.scale);
            f.draw->AddRectFilled(ta, tb, IM_COL32(35, 80, 176, 230), 4.0f * f.scale);
            f.draw->AddText(f.font, f.small_text, ImVec2(ta.x + 4.0f * f.scale, ta.y + 3.0f * f.scale), kWhite, tag);
        }
        if (is_selected) {
            const float pad = 4.0f * f.scale;
            f.draw->AddRect(ImVec2(a.x - pad, a.y - pad), ImVec2(b.x + pad, b.y + pad), kNeeded, 6.0f * f.scale, 0,
                            std::max(1.0f, 4.0f * f.scale));
        }
    }

    void DrawWorkshop(const Frame &f, int step, int side, bool &activate) {
        // BACK, SHARE A VEHICLE, then the grid: up and down go a grid row
        // (up from its top row to SHARE A VEHICLE), left and right a vehicle.
        const int count = int(entries.size());
        const int rows = (count + kGridColumns - 1) / kGridColumns;
        ws_selected = std::min(ws_selected, kWorkshopFixed + count - 1);
        if (step) {
            if (ws_selected < kWorkshopFixed) {
                const int next = ws_selected + step;
                ws_selected = next < kWorkshopFixed ? std::max(next, 0)
                              : count             ? kWorkshopFixed + ws_top * kGridColumns
                                                  : ws_selected;
            } else {
                const int i = ws_selected - kWorkshopFixed;
                const int next = i + step * kGridColumns;
                if (next < 0)
                    ws_selected = kWorkshopFixed - 1;
                else if (next < count)
                    ws_selected = kWorkshopFixed + next;
                else if (i / kGridColumns < rows - 1)
                    ws_selected = kWorkshopFixed + count - 1;  // into a shorter last row
            }
        }
        if (side && ws_selected >= kWorkshopFixed)
            ws_selected = kWorkshopFixed + std::clamp(ws_selected - kWorkshopFixed + side, 0, count - 1);
        // The selected vehicle stays in view; the wheel scrolls past it.
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
            ws_top -= int(wheel);
        if ((step || side) && ws_selected >= kWorkshopFixed) {
            const int row = (ws_selected - kWorkshopFixed) / kGridColumns;
            ws_top = std::clamp(ws_top, row - (kGridRows - 1), row);
        }
        ws_top = std::clamp(ws_top, 0, std::max(0, rows - kGridRows));

        const int first = ws_top * kGridColumns;
        const int shown = std::clamp(count - first, 0, kGridRows * kGridColumns);
        int selection = ws_selected;
        for (int i = 0; i < kWorkshopFixed; ++i)
            Mouse(f, i, selection, activate, kGridLeft);
        for (int i = 0; i < shown; ++i) {
            const ImVec2 at = CardAt(i);
            MouseRect(f.At(at.x, at.y), f.At(at.x + kCardWidth, at.y + kCardHeight), kWorkshopFixed + first + i,
                      selection, activate);
        }
        ws_selected = selection;

        DrawRow(f, 0, ws_selected == 0, true, "BACK", "WORKSHOP", kDim, false, kGridLeft);
        DrawRow(f, 1, ws_selected == 1, true, "SHARE A VEHICLE", "", kWhite, false, kGridLeft);
        if (!index_loaded || count == 0) {
            const std::string what = index_loading  ? "LOADING THE WORKSHOP..."
                                     : !index_loaded ? index_error
                                                     : "THE WORKSHOP HAS NO VEHICLES YET.";
            f.draw->AddText(f.font, f.small_text, f.At(kGridLeft + 30.0f, kGridTop + 10.0f),
                            index_loaded || index_loading ? kWhite : kErrorColour, what.c_str(), nullptr,
                            kTextWidth * f.scale);
        }
        for (int i = 0; i < shown; ++i) {
            const workshop::Entry &e = entries[size_t(first + i)];
            FetchPreview(e);
            DrawCard(f, CardAt(i), e, ws_selected == kWorkshopFixed + first + i);
        }
        DrawScrollArrows(f, kGridRight + 16.0f, kGridTop,
                         kGridTop + kGridRows * kCardHeight + (kGridRows - 1) * kGridRowGap, ws_top > 0,
                         ws_top + kGridRows < rows);

        // Right of BACK and SHARE A VEHICLE: what the last download said, or
        // the selected vehicle: its name, who made it, what it is.
        const workshop::Entry *e = SelectedEntry();
        const float wrap = kDetailsWidth * f.scale;
        if (!message.empty()) {
            f.draw->AddText(f.font, f.small_text, f.At(kDetailsLeft, kDetailsTop),
                            message_is_error ? kErrorColour : kWhite, message.c_str(), nullptr, wrap);
        } else if (ws_selected == 1) {
            f.draw->AddText(f.font, f.small_text, f.At(kDetailsLeft, kDetailsTop), kDim,
                            "Share one of your vehicles: choose it, name it and add a screenshot. reNut copies "
                            "the submission and opens the workshop's form on GitHub, where you paste it.",
                            nullptr, wrap);
        } else if (e) {
            const std::string name = FitEnd(f.font, f.text, Upper(e->name), wrap);
            f.draw->AddText(f.font, f.text, f.At(kDetailsLeft, kDetailsTop), kWhite, name.c_str());
            std::string line = "BY " + Upper(e->author) + std::format("  -  {} PARTS", e->parts);
            if (!e->needs.empty())
                line += "  -  NEEDS PART MODS";
            line = FitEnd(f.font, f.small_text, line, wrap);
            f.draw->AddText(f.font, f.small_text, f.At(kDetailsLeft, kDetailsTop + 32.0f), kWhite, line.c_str());
            std::string about = e->description;
            if (about.size() > 150)
                about = about.substr(0, 147) + "...";
            f.draw->AddText(f.font, f.small_text, f.At(kDetailsLeft, kDetailsTop + 58.0f), kDim, about.c_str(),
                            nullptr, wrap);
        }
        const char *hint = ws_selected == 0         ? "ENTER / A  BACK"
                           : !e                     ? "ENTER / A  OPEN      B  BACK"
                           : installed.count(e->id) ? "ENTER / A  REMOVE      B  BACK"
                                                    : "ENTER / A  DOWNLOAD      B  BACK";
        f.draw->AddText(f.font, f.small_text, f.At(kGridLeft + 30.0f, kImageHeight - 34.0f), kDim, hint);
    }

    // --- Sharing a vehicle -----------------------------------------------------

    void OpenShare() {
        vehicles = workshop::LocalVehicles(save);
        vehicle_index = 0;
        share_name.clear();
        share_description.clear();
        share_selected = 1;
        editing = ShareRow::kNone;
        PickVehicle(0);
        page = Page::kShare;
        Say(vehicles.empty() ? "There are no vehicles in your save yet: build one in the game's garage first." : "",
            vehicles.empty());
    }

    // Moves to another vehicle; its name goes in NAME unless one was typed.
    void PickVehicle(int dir) {
        if (vehicles.empty())
            return;
        const std::string old_name = vehicles[size_t(vehicle_index)].name;
        const int count = int(vehicles.size());
        vehicle_index = ((vehicle_index + dir) % count + count) % count;
        if (share_name.empty() || share_name == old_name.substr(0, workshop::kMaxName))
            share_name = vehicles[size_t(vehicle_index)].name.substr(0, workshop::kMaxName);
    }

    void SetScreenshot(const fs::path &file) {
        std::string data;
        {
            std::ifstream in(file, std::ios::binary);
            data.assign(std::istreambuf_iterator<char>(in), {});
        }
        int w = 0, h = 0;
        std::vector<uint8_t> rgba = workshop::DecodeImage(data.data(), data.size(), &w, &h, 960);
        if (rgba.empty()) {
            Say("That picture could not be read: choose a PNG or JPEG.", true);
            return;
        }
        renut::gpu::overlay::RemoveImage(screenshot_image);
        screenshot_image = renut::gpu::overlay::AddImage(uint32_t(w), uint32_t(h), std::move(rgba));
        screenshot = file;
        Say("");
    }

    void Share() {
        if (vehicles.empty())
            return;
        const fs::path mods = renut::mods::ModsFolder();
        std::string error;
        const std::string submission = workshop::BuildSubmission(
            vehicles[size_t(vehicle_index)].file, share_name, share_description, screenshot, mods, &error);
        if (submission.empty()) {
            Say(error, true);
            return;
        }
        ImGui::SetClipboardText(submission.c_str());
        OpenUrl(workshop::kSubmitUrl);
        Say("Copied. On GitHub, paste it (Ctrl+V) into the form, tick the box and submit: the workshop's bot "
            "checks it and publishes it, usually within a minute.");
    }

    static void OpenUrl(const char *url) {
#if defined(_WIN32)
        ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
#else
        SDL_OpenURL(url);
#endif
    }

    void DrawShare(const Frame &f, int step, int side, bool &activate) {
        constexpr int total = int(ShareRow::kCount);
        if (step)
            share_selected = (share_selected + step + total) % total;
        for (int i = 0; i < total; ++i)
            Mouse(f, i, share_selected, activate);
        if (side && ShareRow(share_selected) == ShareRow::kVehicle)
            PickVehicle(side);

        const bool have = !vehicles.empty();
        DrawRow(f, 0, share_selected == 0, true, "BACK", "SHARE A VEHICLE", kDim, false);
        DrawRow(f, 1, share_selected == 1, have, "VEHICLE",
                have ? Upper(vehicles[size_t(vehicle_index)].name) : "NONE", kWhite, false);
        DrawTextRow(f, ShareRow::kName, "NAME", share_name, workshop::kMaxName);
        DrawTextRow(f, ShareRow::kDescription, "DESCRIPTION", share_description, workshop::kMaxDescription);
        DrawRow(f, 4, share_selected == 4, have, "SCREENSHOT",
                screenshot.empty() ? "CHOOSE" : Utf8(screenshot.filename()), screenshot.empty() ? kNeeded : kWhite,
                !screenshot.empty());
        DrawRow(f, 5, share_selected == 5, have, "COPY AND OPEN GITHUB", "", kWhite, false);

        const float wrap = kTextWidth * f.scale;
        if (!message.empty()) {
            DrawMessage(f, kInfoTop);
        } else {
            const char *about =
                share_selected == 1   ? "Left and right choose the vehicle."
                : share_selected == 2 ? "What the workshop lists it as (up to 31 characters). Type it on the keyboard."
                : share_selected == 3 ? "A sentence or two about it: what it does, which buttons do what."
                : share_selected == 4 ? "A PNG or JPEG of the vehicle (a screenshot from the game). reNut makes it "
                                        "small enough for GitHub."
                : share_selected == 5 ? "Copies the submission and opens the workshop's form on GitHub (a GitHub "
                                        "account is needed). At most 3 vehicles per 10 minutes."
                                      : "";
            f.draw->AddText(f.font, f.small_text, f.At(kColumnLeft + 30.0f, kInfoTop), kDim, about, nullptr, wrap);
        }
        DrawPhoto(f, screenshot_image, have ? Upper(share_name) : "");
    }

    // NAME or DESCRIPTION: the text, or a text field while it is edited (A
    // or a click starts it; Enter, Escape or clicking away ends it).
    void DrawTextRow(const Frame &f, ShareRow row, const char *label, std::string &text, size_t max) {
        const int index = int(row);
        const bool active = editing == row;
        DrawRow(f, index, share_selected == index, !vehicles.empty(), label, active ? "" : text, kWhite, false);
        if (!active)
            return;
        const float label_width = f.font->CalcTextSizeA(f.text, FLT_MAX, 0.0f, label).x;
        const ImVec2 a = f.At(kColumnLeft + 30.0f, kSlotTop[index] + 5.0f);
        const ImVec2 b = f.At(kRowRight - 10.0f, kSlotTop[index] + kRowHeight - 5.0f);
        ImGui::SetCursorScreenPos(ImVec2(a.x + label_width + 16.0f * f.scale, a.y));
        ImGui::PushFont(f.font, f.small_text);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 40));
        ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * f.scale, (b.y - a.y - f.small_text) * 0.5f));
        ImGui::SetNextItemWidth(b.x - a.x - label_width - 16.0f * f.scale);
        if (focus_edit) {
            ImGui::SetKeyboardFocusHere();
            focus_edit = false;
        }
        edit_buffer.resize(max + 1);
        const bool entered = ImGui::InputText("##share_text", edit_buffer.data(), max + 1,
                                              ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const bool left_field = ImGui::IsItemDeactivated();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::PopFont();
        if (entered || left_field) {
            text = edit_buffer.c_str();
            editing = ShareRow::kNone;
        }
    }

    void ActivateShare() {
        switch (ShareRow(share_selected)) {
        case ShareRow::kBack:
            page = Page::kWorkshop;
            Say("");
            break;
        case ShareRow::kVehicle:
            PickVehicle(1);
            break;
        case ShareRow::kName:
        case ShareRow::kDescription:
            if (vehicles.empty())
                break;
            editing = ShareRow(share_selected);
            edit_buffer = editing == ShareRow::kName ? share_name : share_description;
            focus_edit = true;
            break;
        case ShareRow::kScreenshot:
            if (!vehicles.empty())
                OpenPicker(Row::kScreenshot, PickKind::kImage, screenshot.parent_path());
            break;
        case ShareRow::kSubmit:
            Share();
            break;
        default:
            break;
        }
    }

    void Draw() {
        ImGuiIO &io = ImGui::GetIO();
        web.Pump();
        TakePick();
        const setup::Progress progress = setup::GetProgress();
        const bool busy = progress.running;
        if (job_was_running && !busy) {
            Refresh();
            if (!progress.error.empty())
                Say(progress.error, true);
            else
                Say(progress.result);
            if (Ready() && page == Page::kMain)
                selected = Row::kPlay;
        }
        job_was_running = busy;

        // Input: arrows and Enter, a controller's d-pad, stick and A, the mouse.
        const uint32_t held = picking ? 0 : pads.Held();
        const uint32_t pressed = held & ~pad_held;
        pad_held = held;
        const float dt = io.DeltaTime;
        const bool key_up = !picking && ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
        const bool key_down = !picking && ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
        const bool key_left = !picking && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true);
        const bool key_right = !picking && ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
        int step =
            (up.Step(held & kPadUp, dt) || key_up ? -1 : 0) + (down.Step(held & kPadDown, dt) || key_down ? 1 : 0);
        int side = (left.Step(held & kPadLeft, dt) || key_left ? -1 : 0) +
                   (right.Step(held & kPadRight, dt) || key_right ? 1 : 0);
        bool activate = !picking && ((pressed & kPadA) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                     ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                                     ImGui::IsKeyPressed(ImGuiKey_Space, false));
        // B goes back a page; Escape doesn't (the BACK rows do, for the
        // keyboard and mouse). On the menu, B or Escape stops a running job.
        bool back = !picking && (pressed & kPadB);
        bool stop = !picking && busy && page == Page::kMain &&
                    ((pressed & kPadB) || ImGui::IsKeyPressed(ImGuiKey_Escape, false));
        // M or Y mutes or unmutes the music, on any page; - and + or the
        // bumpers turn it down and up.
        bool mute = !picking && ((pressed & kPadY) || ImGui::IsKeyPressed(ImGuiKey_M, false));
        const bool key_quieter = !picking && (ImGui::IsKeyPressed(ImGuiKey_Minus, true) ||
                                              ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, true));
        const bool key_louder = !picking && (ImGui::IsKeyPressed(ImGuiKey_Equal, true) ||
                                             ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, true));
        int volume = (quieter.Step(held & kPadLeftBumper, dt) || key_quieter ? -1 : 0) +
                     (louder.Step(held & kPadRightBumper, dt) || key_louder ? 1 : 0);
        // A key bind waiting for its key has every key and button.
        // So has a text field being typed in (its Enter and Escape are its own).
        if (UpdateBinding(dt, pressed) || editing != ShareRow::kNone || io.WantTextInput) {
            step = side = volume = 0;
            activate = back = stop = mute = false;
        }
        // Either way the frame is drawn, on the page it leads to: a frame
        // without the menu blinks.
        if (back && page != Page::kMain) {
            // One page up: settings to OPTIONS, sharing to the workshop, the
            // rest to the menu.
            page = page == Page::kSettings ? Page::kOptions : page == Page::kShare ? Page::kWorkshop : Page::kMain;
            Say("");
            step = side = 0;
            activate = false;
        } else if (stop) {
            setup::Cancel();
        }

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("reNut##launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        Frame f;
        f.draw = ImGui::GetWindowDrawList();
        f.font = renut::gpu::overlay::LauncherFont();
        // The picture, whole and centred; the rest of the screen its colour.
        f.scale = std::min(io.DisplaySize.x / kImageWidth, io.DisplaySize.y / kImageHeight);
        f.origin = ImVec2((io.DisplaySize.x - kImageWidth * f.scale) * 0.5f,
                          (io.DisplaySize.y - kImageHeight * f.scale) * 0.5f);
        f.text = kTextSize * f.scale;
        f.small_text = kSmallTextSize * f.scale;
        f.draw->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, backdrop);
        const bool workshop_page = page == Page::kWorkshop || page == Page::kShare;
        DrawBackground(f, workshop_page && workshop_background ? workshop_background : background);
        DrawCorners(f);

        switch (page) {
        case Page::kMain: DrawMain(f, step, activate, progress); break;
        case Page::kWorkshop: DrawWorkshop(f, step, side, activate); break;
        case Page::kOptions: DrawOptions(f, step, activate); break;
        case Page::kSettings: DrawSettings(f, step, side, activate); break;
        case Page::kShare: DrawShare(f, step, side, activate); break;
        }
        if (DrawMusic(f, editing == ShareRow::kNone))
            mute = true;
        ImGui::End();
        RestartForUpdate(busy);
        if (mute)
            ToggleMusic();
        else if (volume)
            SetMusicVolume(music_choice.volume + double(volume) / kVolumeBars, true);

        if (!activate)
            return;
        switch (page) {
        case Page::kMain: Activate(selected, busy); break;
        case Page::kWorkshop: ActivateEntry(); break;
        case Page::kOptions: ActivateOptions(); break;
        case Page::kSettings: ActivateSetting(); break;
        case Page::kShare: ActivateShare(); break;
        }
    }
};

LauncherMenu::LauncherMenu() : impl_(std::make_unique<Impl>()) {}

LauncherMenu::~LauncherMenu() = default;

void LauncherMenu::Init(const std::string &app_name, const renut::setup::Paths &defaults,
                        const std::filesystem::path &config_path, rex::ui::IUISystem &ui, PlayFn on_play,
                        QuitFn on_quit) {
    Impl &m = *impl_;
    m.app_name = app_name;
    m.config_path = config_path;
    m.capture.Attach(ui);
    m.on_play = std::move(on_play);
    m.on_quit = std::move(on_quit);
    m.paths = setup::LoadPaths(app_name, defaults);
    // The save folder can't change while reNut runs (RenutApp::OnConfigure applies renut.cfg's).
    m.active_user = defaults.user;
    m.paths.user = defaults.user;
    m.Refresh();
    m.selected = m.Ready() ? Row::kPlay : !m.status.game ? Row::kDisc : Row::kTitleUpdate;
    m.LoadBackground();
    m.music_choice = LoadMusicChoice();
    m.music.Start(!m.music_choice.on, float(m.music_choice.volume));
    renut::update::Start();
    m.visible = true;
}

void LauncherMenu::Draw() {
    if (impl_->visible)
        impl_->Draw();
}
