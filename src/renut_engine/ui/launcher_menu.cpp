#include "renut_engine/ui/launcher_menu.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <SDL3/SDL.h>

#include "imgui.h"
#include "miniaudio.h"
#include "renut_engine/app/app_paths.h"
#include "renut_engine/app/game_setup.h"
#include "renut_engine/app/renut_logging.h"
#include "renut_engine/app/updater.h"
#include "renut_engine/app/workshop.h"
#include "renut_engine/gpu/backend/native_window.h"
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

// icon/launcher_background.png and icon/workshop_background.png (the same
// without the photos, where the workshop pins its own), which CMakeLists.txt
// turns into these includes.
const unsigned char kBackgroundPng[] = {
#include "launcher_background.png.inc"
};
const unsigned char kWorkshopBackgroundPng[] = {
#include "workshop_background.png.inc"
};

// Mumbo's Mountain, which the build makes from the game's wave bank
// (tools/make_launcher_music.py; never in the repository). The bank's own
// loop region: after the intro, these frames go round.
const char kMusicOgg[] =
#include "launcher_music.ogg.inc"
    ;
constexpr ma_uint64 kMusicLoopStart = 583040;
constexpr ma_uint64 kMusicLoopFrames = 11874944;
constexpr float kMusicVolume = 0.6f;

// The picture's layout, in its own pixels: the menu sits in the column
// between its dashed guides (vertical at x 552 and 1130), below the logo; a
// workshop vehicle's preview is pinned over the photos on the right.
constexpr float kImageWidth = 1672.0f;
constexpr float kImageHeight = 941.0f;
constexpr float kColumnLeft = 560.0f;
// Rows end short of Banjo's photo, whose corner reaches x 1106 at y 608.
constexpr float kRowRight = 1100.0f;
// Text under the rows, from the labels' left to the values' right.
constexpr float kTextWidth = kRowRight - kColumnLeft - 40.0f;
constexpr float kRowHeight = 38.0f;
// The rows' tops, between the horizontal guides (y 380, 442, 683 and 749),
// none on a line: one row, six, one, then two.
constexpr float kSlotTop[] = {392.0f, 449.0f, 487.0f, 525.0f, 563.0f, 601.0f, 639.0f, 697.0f, 759.0f, 797.0f};
constexpr int kSlots = int(sizeof(kSlotTop) / sizeof(kSlotTop[0]));
// Under the last slot: a job's progress, a message.
constexpr float kInfoTop = 845.0f;
constexpr float kTextSize = 24.0f;
constexpr float kSmallTextSize = 19.0f;
constexpr float kPhotoLeft = 1160.0f;
constexpr float kPhotoTop = 430.0f;
constexpr float kPhotoWidth = 460.0f;  // 16:9 inside a white border
constexpr float kPhotoBorder = 12.0f;
// The workshop's rows: BACK and SHARE A VEHICLE, then its vehicles (slots
// 2-7, scrolled); the selected one's details go in the slots below.
constexpr int kWorkshopRows = 7;
constexpr int kWorkshopFixed = 2;
constexpr int kWorkshopEntries = kWorkshopRows - kWorkshopFixed + 1;

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
constexpr const char *kOptionPages[] = {"GRAPHICS", "INPUT"};
constexpr int kOptionPageCount = int(sizeof(kOptionPages) / sizeof(kOptionPages[0]));
constexpr int kSettingRows = kSlots - 1;

// GRAPHICS: what the in-game GRAPHICS menu has (game/graphics_menu.cpp),
// after the window and renderer.
struct Setting {
    const char *cvar;
    const char *label;
};
constexpr Setting kGraphicsSettings[] = {
    {"fullscreen", "Fullscreen"},
#if defined(_WIN32)
    {"renut_gpu_backend", "Renderer"},  // Vulkan only elsewhere
#endif
    {"renut_resolution", "Resolution"},
    {"renut_field_of_view", "Field of View"},
    {"renut_shadow_resolution", "Shadow Resolution"},
    {"renut_shadow_softness", "Shadow Softness"},
    {"renut_anisotropic_filtering", "Anisotropic Filtering"},
    {"renut_ssao", "Ambient Occlusion"},
    {"renut_reflection_resolution", "Reflections"},
    {"renut_lod_distance", "Detail Distance"},
    {"renut_grass_distance", "Grass Distance"},
    {"renut_depth_of_field", "Depth of Field"},
    {"renut_bloom", "Bloom"},
    {"renut_msaa", "MSAA"},
    {"renut_alpha_to_coverage", "Smooth Cut-Outs"},
    {"renut_anti_aliasing", "Anti-Aliasing"},
    {"renut_vsync", "V-Sync"},
    {"frame_cap", "Frame Cap"},
};
// INPUT: the keyboard and mouse switches, then every key bind
// (Nuts&Bolts/Keyboard's renut_key_*).
constexpr Setting kInputSettings[] = {
    {"renut_keyboard", "Keyboard Controls"},
    {"renut_mouse_camera", "Mouse Camera"},
    {"renut_mouse_sensitivity", "Mouse Sensitivity"},
    {"renut_mouse_invert_y", "Mouse Invert Y"},
#if defined(_WIN32)
    {"input_backend", "Controller Input"},  // SDL only elsewhere
#endif
    {"renut_disable_extra_controllers", "Player 1 Only"},
};
constexpr const char kKeyboardCategory[] = "Nuts&Bolts/Keyboard";
constexpr const char kKeyPrefix[] = "renut_key_";

constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kDim = IM_COL32(255, 255, 255, 110);
constexpr ImU32 kNeeded = IM_COL32(255, 222, 120, 255);
constexpr ImU32 kErrorColour = IM_COL32(255, 150, 140, 255);
constexpr ImU32 kHighlight = IM_COL32(255, 255, 255, 38);
constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 90);

std::string Utf8(const fs::path &path) {
    return rex::path_to_utf8(path);
}

std::string Gigabytes(uint64_t bytes) {
    return fmt::format("{:.1f} GB", double(bytes) / double(1ull << 30));
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

bool IsNumber(const rex::cvar::FlagEntry &e) {
    using rex::cvar::FlagType;
    return e.type == FlagType::Int32 || e.type == FlagType::Int64 || e.type == FlagType::Uint32 ||
           e.type == FlagType::Uint64 || e.type == FlagType::Double;
}

// What a setting's row says on its right.
std::string SettingValue(const rex::cvar::FlagEntry &e, bool key) {
    const std::string value = rex::cvar::GetFlagByName(e.name);
    if (key)
        return value.empty() ? "NONE" : Upper(value);
    if (e.type == rex::cvar::FlagType::Boolean)
        return IsOn(value) ? "ON" : "OFF";
    if (e.type == rex::cvar::FlagType::Double)
        return fmt::format("{:.2f}", std::atof(value.c_str()));
    return Upper(value);
}

// The setting's next (dir 1) or previous (dir -1) value, applied and saved
// to renut.toml: a switch flips, a list of values goes round, a ranged number
// steps (whole numbers by a twentieth of the range, at least 1; others by
// 0.05). False if it has none.
bool StepSetting(const rex::cvar::FlagEntry &e, int dir, const fs::path &config_path) {
    const std::string current = rex::cvar::GetFlagByName(e.name);
    std::string next;
    if (e.type == rex::cvar::FlagType::Boolean) {
        next = IsOn(current) ? "false" : "true";
    } else if (e.constraints.allowed_values.size() >= 2) {
        const auto &values = e.constraints.allowed_values;
        const int count = int(values.size());
        const auto it = std::find(values.begin(), values.end(), current);
        const int index = it == values.end() ? 0 : int(it - values.begin());
        next = values[size_t(((index + dir) % count + count) % count)];
    } else if (IsNumber(e) && e.constraints.min && e.constraints.max && *e.constraints.max > *e.constraints.min) {
        const double lo = *e.constraints.min, hi = *e.constraints.max;
        const bool whole = e.type != rex::cvar::FlagType::Double;
        const double step = whole ? std::max(1.0, std::round((hi - lo) / 20.0)) : 0.05;
        double value = std::clamp(std::atof(current.c_str()) + dir * step, lo, hi);
        next = whole ? std::to_string(int64_t(std::llround(value)))
                     : fmt::format("{:g}", std::round(value * 100.0) / 100.0);
    } else {
        return false;
    }
    if (next == current || !rex::cvar::SetFlagByName(e.name, next))
        return false;
    if (!config_path.empty())
        rex::cvar::SaveConfig(config_path);
    return true;
}

// A key bind's label, from its description: "Controller left bumper." is
// LEFT BUMPER, "Move forward (left stick up)." MOVE FORWARD.
std::string KeyLabel(const rex::cvar::FlagEntry &e) {
    if (e.name == "renut_key_walk")
        return "WALK";
    std::string label = e.description.substr(0, e.description.find_first_of(".:("));
    if (label.rfind("Controller ", 0) == 0)
        label = label.substr(11);
    while (!label.empty() && label.back() == ' ')
        label.pop_back();
    return Upper(label.empty() ? e.name.substr(sizeof(kKeyPrefix) - 1) : label);
}

// The next key or mouse button pressed in the game window, while armed: the
// same keys and names the binds use (rex::ui::ParseVirtualKey), on Windows
// and Linux alike. On the UI thread, as the window's events are.
class KeyCapture : public rex::ui::WindowInputListener {
public:
    void Attach(rex::ui::Window *window) {
        window_ = window;
        if (window_)
            window_->AddInputListener(this, kZOrder);
    }
    void Detach() {
        if (window_)
            window_->RemoveInputListener(this);
        window_ = nullptr;
    }
    void Arm() {
        pressed_ = rex::ui::VirtualKey::kNone;
        armed_ = true;
    }
    void Disarm() { armed_ = false; }
    // The key pressed since Arm, once; kNone until one is.
    rex::ui::VirtualKey Take() {
        const rex::ui::VirtualKey key = pressed_;
        pressed_ = rex::ui::VirtualKey::kNone;
        return key;
    }

    void OnKeyDown(rex::ui::KeyEvent &e) override { Press(e.virtual_key()); }
    void OnMouseDown(rex::ui::MouseEvent &e) override {
        switch (e.button()) {
        case rex::ui::MouseEvent::Button::kLeft: Press(rex::ui::VirtualKey::kLButton); break;
        case rex::ui::MouseEvent::Button::kRight: Press(rex::ui::VirtualKey::kRButton); break;
        case rex::ui::MouseEvent::Button::kMiddle: Press(rex::ui::VirtualKey::kMButton); break;
        case rex::ui::MouseEvent::Button::kX1: Press(rex::ui::VirtualKey::kXButton1); break;
        case rex::ui::MouseEvent::Button::kX2: Press(rex::ui::VirtualKey::kXButton2); break;
        default: break;
        }
    }

private:
    // Above the keyboard's (2), mouse camera's (1) and the app's (0) listeners.
    static constexpr size_t kZOrder = 3;

    void Press(rex::ui::VirtualKey key) {
        if (armed_ && pressed_ == rex::ui::VirtualKey::kNone)
            pressed_ = key;
    }

    rex::ui::Window *window_ = nullptr;
    bool armed_ = false;
    rex::ui::VirtualKey pressed_ = rex::ui::VirtualKey::kNone;
};

// --- Controllers --------------------------------------------------------------
//
// Before the game runs there is no input system: the controllers are read as
// the Guide reads them, from SDL's gamepad subsystem, polled and without
// events (the game's own input takes them over after PLAY).

enum PadButton : uint32_t { kPadUp = 1, kPadDown = 2, kPadA = 4, kPadB = 8, kPadLeft = 16, kPadRight = 32 };

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
        }
        return held;
    }

    // At PLAY: the controllers go back, and SDL as the game's input found it.
    // On Linux reNut shares the SDK's SDL, whose input system starts after
    // this and would otherwise inherit no gamepad events and HIDAPI off.
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
//
// The menu's music, on a miniaudio engine of its own (the game's audio isn't
// running yet), until PLAY.

class Music {
public:
    ~Music() { Stop(); }

    void Start() {
        if (engine_ready_)
            return;
        if (ma_engine_init(nullptr, &engine_) != MA_SUCCESS) {
            RNUT_WARN("Launcher: no audio device for the music");
            return;
        }
        engine_ready_ = true;
        const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
        if (ma_decoder_init_memory(kMusicOgg, sizeof(kMusicOgg) - 1, &config, &decoder_) != MA_SUCCESS) {
            RNUT_WARN("Launcher: the music could not be decoded");
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
        ma_sound_set_volume(&sound_, kMusicVolume);
        ma_sound_start(&sound_);
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
//
// The answer comes later: Windows' dialog runs on a thread of its own, so the
// menu keeps drawing under it; SDL's (Linux: the desktop's portal, or zenity)
// answers through a callback, maybe from another thread.

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
                state.paths.push_back(rex::to_path(*f));
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
    bool restart_tried = false;   // an installed update's restart (RestartForUpdate)
    bool restart_failed = false;

    setup::Paths paths;
    setup::Status status;
    Row selected = Row::kPlay;
    std::string message;
    bool message_is_error = false;
    bool job_was_running = false;

    uint64_t background = 0;           // overlay image ids
    uint64_t workshop_background = 0;  // the workshop's pages: no photos
    ImU32 backdrop = IM_COL32(35, 80, 176, 255);

    Pads pads;
    uint32_t pad_held = 0;
    Repeat up, down, left, right;

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
    int ws_top = 0;           // the first entry shown

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
                Say("That folder has another region's Nuts & Bolts: reNut runs the US (NTSC) or EU (PAL) version.",
                    true);
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
                                          "(NTSC) or EU (PAL) version. Extract one of those discs, or choose a "
                                          "folder that has one."
                    : !status.game      ? "Extract the game disc first (or choose a game folder that has it)."
                                        : "Extract Title Update #3 first: reNut runs on it.",
                    true);
            return;
        }
        switch (row) {
        case Row::kPlay: {
            setup::SavePaths(app_name, paths);
            rex::PathConfig config;
            config.game_data_root = paths.game;
            config.update_data_root = paths.update;
            config.user_data_root = paths.user;
            visible = false;
            pads.Close();
            capture.Detach();
            music.Stop();
            ReleaseImages();
            if (on_play)
                on_play(std::move(config));
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
                         index_error = r.status ? fmt::format("The workshop answered {}; try again later.", r.status)
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
                         Say(r.status ? fmt::format("The workshop answered {}; try again later.", r.status)
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
                if (rex::cvar::GetFlagInfo(list[i].cvar))
                    settings.push_back({list[i].cvar, Upper(list[i].label), false});
        };
        if (which == 0) {
            add(kGraphicsSettings, std::size(kGraphicsSettings));
        } else {
            add(kInputSettings, std::size(kInputSettings));
            for (const auto &e : rex::cvar::GetRegistry())
                if (e.category == kKeyboardCategory && e.name.rfind(kKeyPrefix, 0) == 0)
                    settings.push_back({e.name, KeyLabel(e), true});
        }
        settings_page = which;
        settings_selected = settings.empty() ? 0 : 1;
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
        if (step && settings_selected > 0) {
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
            if (const SettingRow *row = SelectedSetting(); row && !row->key)
                if (const auto *entry = rex::cvar::GetFlagInfo(row->cvar))
                    StepSetting(*entry, side, config_path);

        DrawRow(f, 0, settings_selected == 0, true, "BACK", kOptionPages[settings_page], kDim, false);
        for (int i = 0; i < shown; ++i) {
            const SettingRow &row = settings[size_t(settings_top + i)];
            const auto *entry = rex::cvar::GetFlagInfo(row.cvar);
            const bool waiting = row.cvar == binding;
            const std::string value = waiting ? "PRESS A KEY" : entry ? SettingValue(*entry, row.key) : "";
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
            const auto *entry = rex::cvar::GetFlagInfo(row->cvar);
            about = entry ? entry->description : "";
            if (about.size() > 240)
                about = about.substr(0, 237) + "...";
            if (entry && entry->lifecycle == rex::cvar::Lifecycle::kRequiresRestart &&
                about.find("restart") == std::string::npos)
                about += " Takes effect after a restart.";
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
        if (const auto *entry = rex::cvar::GetFlagInfo(row->cvar))
            StepSetting(*entry, 1, config_path);
    }

    // While a key bind waits for its key: takes it (or cancels). True while it
    // has the input: from the press that chose the row until a moment after
    // the key, which the overlay may see a frame after the window does (the
    // Escape or Enter that was bound would leave the page or bind again).
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
        const rex::ui::VirtualKey key = capture.Take();
        std::string name;
        bool done = (pressed & kPadB) || key == rex::ui::VirtualKey::kEscape;
        if (!done && key == rex::ui::VirtualKey::kDelete) {
            done = true;
            rex::cvar::SetFlagByName(binding, "");
        } else if (!done && key != rex::ui::VirtualKey::kNone) {
            name = rex::ui::VirtualKeyToString(key);
            if (!name.empty()) {
                done = true;
                rex::cvar::SetFlagByName(binding, name);
            }
        }
        if (done) {
            if (!config_path.empty())
                rex::cvar::SaveConfig(config_path);
            capture.Disarm();
            binding.clear();
            bind_wait = 0.25f;
        }
        return true;
    }

    // --- Drawing ----------------------------------------------------------------

    // One menu row: the highlight and arrow when selected, the label, and a
    // value on its right.
    void DrawRow(const Frame &f, int index, bool is_selected, bool enabled, const std::string &label,
                 std::string value, ImU32 value_colour, bool value_is_path) const {
        const ImVec2 a = f.At(kColumnLeft, kSlotTop[index]);
        const ImVec2 b = f.At(kRowRight, kSlotTop[index] + kRowHeight);
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
    void Mouse(const Frame &f, int index, int &selection, bool &activate) const {
        const ImGuiIO &io = ImGui::GetIO();
        const ImVec2 a = f.At(kColumnLeft, kSlotTop[index]);
        const ImVec2 b = f.At(kRowRight, kSlotTop[index] + kRowHeight);
        if (picking || !binding.empty() || bind_wait > 0.0f || !ImGui::IsMouseHoveringRect(a, b, false))
            return;
        if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)
            selection = index;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            selection = index;
            activate = true;
        }
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
        const std::string line = fmt::format("{}  {}%  ({} of {})", what, int(done * 100.0f),
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

    void DrawScrollArrows(const Frame &f, int first_slot, int last_slot, bool above, bool below) const {
        const float arrow_x = kRowRight + 14.0f;
        if (above) {
            const ImVec2 p = f.At(arrow_x, kSlotTop[first_slot] + 4.0f);
            f.draw->AddTriangleFilled(ImVec2(p.x - 6 * f.scale, p.y + 6 * f.scale),
                                      ImVec2(p.x + 6 * f.scale, p.y + 6 * f.scale), p, kDim);
        }
        if (below) {
            const ImVec2 p = f.At(arrow_x, kSlotTop[last_slot] + kRowHeight - 4.0f);
            f.draw->AddTriangleFilled(ImVec2(p.x - 6 * f.scale, p.y - 6 * f.scale),
                                      ImVec2(p.x + 6 * f.scale, p.y - 6 * f.scale), p, kDim);
        }
    }

    void DrawWorkshop(const Frame &f, int step, bool &activate) {
        // Rows: BACK, SHARE A VEHICLE, then the entries from ws_top.
        const int count = int(entries.size());
        const int total = count + kWorkshopFixed;
        if (step)
            ws_selected = (ws_selected + step + total) % total;
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
            ws_top -= int(wheel);
        // The selected entry stays in view; the wheel scrolls past it.
        if (step && ws_selected >= kWorkshopFixed) {
            ws_top = std::min(ws_top, ws_selected - kWorkshopFixed);
            ws_top = std::max(ws_top, ws_selected - kWorkshopFixed - (kWorkshopEntries - 1));
        }
        ws_top = std::clamp(ws_top, 0, std::max(0, count - kWorkshopEntries));

        int selection = ws_selected < kWorkshopFixed ? ws_selected : ws_selected - ws_top;
        const int shown = std::min(kWorkshopEntries, count - ws_top);
        for (int i = 0; i < kWorkshopFixed + shown; ++i)
            Mouse(f, i, selection, activate);
        ws_selected = selection < kWorkshopFixed ? selection : ws_top + selection;

        DrawRow(f, 0, ws_selected == 0, true, "BACK", "WORKSHOP", kDim, false);
        DrawRow(f, 1, ws_selected == 1, true, "SHARE A VEHICLE", "", kWhite, false);
        const float info_y = kSlotTop[kWorkshopRows + 1] + 6.0f;
        const float wrap = kTextWidth * f.scale;
        if (!index_loaded) {
            const std::string what = index_loading ? "LOADING THE WORKSHOP..." : index_error;
            f.draw->AddText(f.font, f.small_text, f.At(kColumnLeft + 30.0f, kSlotTop[kWorkshopFixed] + 10.0f),
                            index_loading ? kWhite : kErrorColour, what.c_str(), nullptr, wrap);
        }
        for (int i = 0; i < shown; ++i) {
            const workshop::Entry &e = entries[size_t(ws_top + i)];
            const std::string value = downloading == e.id   ? "..."
                                      : installed.count(e.id) ? "ADDED"
                                                              : "";
            DrawRow(f, i + kWorkshopFixed, ws_selected == ws_top + i + kWorkshopFixed, true, Upper(e.name), value,
                    kWhite, false);
        }
        DrawScrollArrows(f, kWorkshopFixed, kWorkshopRows, ws_top > 0, ws_top + shown < count);

        // Under the list: who made the selected one and what it is, or what
        // the last download said.
        const workshop::Entry *e = SelectedEntry();
        if (!message.empty()) {
            DrawMessage(f, info_y);
        } else if (ws_selected == 1) {
            f.draw->AddText(f.font, f.small_text, f.At(kColumnLeft + 30.0f, info_y), kDim,
                            "Share one of your vehicles: choose it, name it and add a screenshot. reNut copies "
                            "the submission and opens the workshop's form on GitHub, where you paste it.",
                            nullptr, wrap);
        } else if (e) {
            std::string line = "BY " + Upper(e->author) + fmt::format("  -  {} PARTS", e->parts);
            if (!e->needs.empty())
                line += "  -  NEEDS PART MODS";
            f.draw->AddText(f.font, f.small_text, f.At(kColumnLeft + 30.0f, info_y), kWhite, line.c_str());
            std::string about = e->description;
            if (about.size() > 220)
                about = about.substr(0, 217) + "...";
            f.draw->AddText(f.font, f.small_text, f.At(kColumnLeft + 30.0f, info_y + 24.0f), kDim, about.c_str(),
                            nullptr, wrap);
        }
        const char *hint = ws_selected == 0         ? "ENTER / A  BACK"
                           : !e                     ? "ENTER / A  OPEN      B  BACK"
                           : installed.count(e->id) ? "ENTER / A  REMOVE      B  BACK"
                                                    : "ENTER / A  DOWNLOAD      B  BACK";
        f.draw->AddText(f.font, f.small_text, f.At(kColumnLeft + 30.0f, kImageHeight - 34.0f), kDim, hint);

        // The selected vehicle's picture.
        if (!e)
            return;
        FetchPreview(*e);
        const auto it = previews.find(e->id);
        DrawPhoto(f, it != previews.end() ? it->second : 0, Upper(e->name));
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
        fs::path mods = rex::to_path(rex::cvar::GetFlagByName("renut_mods_folder"));
        if (mods.is_relative())
            mods = renut::paths::WritableFolder() / mods;
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
        // A key bind waiting for its key has every key and button.
        // So has a text field being typed in (its Enter and Escape are its own).
        if (UpdateBinding(dt, pressed) || editing != ShareRow::kNone || io.WantTextInput) {
            step = side = 0;
            activate = back = stop = false;
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
        f.font = renut::gpu::overlay::MenuFont();
        // The picture, whole and centred; the rest of the screen its colour.
        f.scale = std::min(io.DisplaySize.x / kImageWidth, io.DisplaySize.y / kImageHeight);
        f.origin = ImVec2((io.DisplaySize.x - kImageWidth * f.scale) * 0.5f,
                          (io.DisplaySize.y - kImageHeight * f.scale) * 0.5f);
        f.text = kTextSize * f.scale;
        f.small_text = kSmallTextSize * f.scale;
        f.draw->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, backdrop);
        // The workshop's pages have the picture without its photos, where the
        // vehicle's own is pinned.
        const bool workshop_page = page == Page::kWorkshop || page == Page::kShare;
        const uint64_t picture = workshop_page && workshop_background ? workshop_background : background;
        if (picture)
            f.draw->AddImage(ImTextureRef(ImTextureID(picture)), f.At(0.0f, 0.0f), f.At(kImageWidth, kImageHeight));

        switch (page) {
        case Page::kMain: DrawMain(f, step, activate, progress); break;
        case Page::kWorkshop: DrawWorkshop(f, step, activate); break;
        case Page::kOptions: DrawOptions(f, step, activate); break;
        case Page::kSettings: DrawSettings(f, step, side, activate); break;
        case Page::kShare: DrawShare(f, step, side, activate); break;
        }
        ImGui::End();
        RestartForUpdate(busy);

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

void LauncherMenu::Init(const std::string &app_name, const rex::PathConfig &defaults,
                        const std::filesystem::path &config_path, rex::ui::Window *window, PlayFn on_play,
                        QuitFn on_quit) {
    Impl &m = *impl_;
    m.app_name = app_name;
    m.config_path = config_path;
    m.capture.Attach(window);
    m.on_play = std::move(on_play);
    m.on_quit = std::move(on_quit);
    m.paths = setup::LoadPaths(app_name, defaults);
    m.Refresh();
    m.selected = m.Ready() ? Row::kPlay : !m.status.game ? Row::kDisc : Row::kTitleUpdate;
    m.LoadBackground();
    m.music.Start();
    renut::update::Start();
    m.visible = true;
}

void LauncherMenu::Draw() {
    if (impl_->visible)
        impl_->Draw();
}
