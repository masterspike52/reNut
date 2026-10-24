#include "renut_engine/ui/touch_controls.h"
#include "renut_engine/app/app_paths.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>

#include <jni.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <rex/filesystem.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/ui/virtual_key.h>

#include "imgui.h"

namespace renut::touch {

std::atomic<bool> g_back{false};  // set from Java

namespace {

using rex::ui::VirtualKey;
using rex::X_RESULT;
using rex::X_STATUS;

// Controls that move together in edit mode.
enum Group { G_STICK, G_DPAD, G_FACE, G_LT, G_LB, G_RT, G_RB, G_BACK, G_START, G_LS, G_RS, G_COUNT };

struct Button {
    int group;
    uint16_t mask;  // X_INPUT_GAMEPAD_*; 0 for the triggers
    int trigger;    // 1 left, 2 right
    const char *label;
    ImU32 ink;
    float x = 0, y = 0, r = 0;  // laid out
};

enum Role { R_NONE, R_STICK, R_LOOK, R_DPAD, R_BUTTON, R_DRAG };
struct Finger {
    SDL_FingerID id = 0;
    Role role = R_NONE;
    int index = 0;            // button, or the group being dragged
    float ax = 0, ay = 0;     // stick anchor / last drag point
    float x = 0, y = 0;
};

std::mutex g_mutex;  // everything below
Button g_buttons[] = {
    {G_FACE, rex::input::X_INPUT_GAMEPAD_A, 0, "A", IM_COL32(110, 220, 90, 255)},
    {G_FACE, rex::input::X_INPUT_GAMEPAD_B, 0, "B", IM_COL32(240, 90, 80, 255)},
    {G_FACE, rex::input::X_INPUT_GAMEPAD_X, 0, "X", IM_COL32(90, 150, 255, 255)},
    {G_FACE, rex::input::X_INPUT_GAMEPAD_Y, 0, "Y", IM_COL32(250, 210, 70, 255)},
    {G_LT, 0, 1, "LT", IM_COL32_WHITE},
    {G_LB, rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER, 0, "LB", IM_COL32_WHITE},
    {G_RT, 0, 2, "RT", IM_COL32_WHITE},
    {G_RB, rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER, 0, "RB", IM_COL32_WHITE},
    {G_BACK, rex::input::X_INPUT_GAMEPAD_BACK, 0, "BACK", IM_COL32_WHITE},
    {G_START, rex::input::X_INPUT_GAMEPAD_START, 0, "START", IM_COL32_WHITE},
    {G_LS, rex::input::X_INPUT_GAMEPAD_LEFT_THUMB, 0, "LS", IM_COL32_WHITE},
    {G_RS, rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB, 0, "RS", IM_COL32_WHITE},
};
constexpr int kButtons = int(std::size(g_buttons));
float g_offs[G_COUNT][2];  // edit mode offsets, in screen heights
Finger g_fingers[10];
float g_w = 2340, g_h = 1080;  // overlay pixels, from Draw
float g_stick_x, g_stick_y, g_stick_r, g_dpad_x, g_dpad_y, g_dpad_r, g_unit;
bool g_enabled = true, g_started = false, g_blocked = false, g_editing = false;
int g_gamepads = 0;
ImFont *g_font = nullptr;

// What the pad reports, rebuilt from the fingers.
uint16_t g_pad_buttons = 0;
uint8_t g_lt = 0, g_rt = 0;
float g_lx = 0, g_ly = 0, g_rx = 0, g_ry = 0;
uint32_t g_packet = 0;

bool VisibleLocked() { return g_started && g_enabled && g_gamepads == 0; }

void Layout() {
    const float W = g_w, H = g_h, u = H / 720.0f, b = 92 * u, m = 48 * u;
    g_unit = b;
    auto off = [&](int g, float &x, float &y) { x += g_offs[g][0] * H; y += g_offs[g][1] * H; };
    g_stick_x = m + 1.3f * b; g_stick_y = H - m - 1.3f * b; g_stick_r = 1.0f * b;
    off(G_STICK, g_stick_x, g_stick_y);
    g_dpad_x = m + 1.0f * b; g_dpad_y = H * 0.42f; g_dpad_r = 0.75f * b;
    off(G_DPAD, g_dpad_x, g_dpad_y);
    const float fx = W - m - 1.5f * b, fy = H - m - 1.4f * b, d = 0.8f * b, fr = 0.42f * b;
    struct { float x, y, r; } at[] = {
        {fx, fy + d, fr}, {fx + d, fy, fr}, {fx - d, fy, fr}, {fx, fy - d, fr},   // A B X Y
        {m + 0.5f * b, m + 0.5f * b, 0.45f * b},                                  // LT
        {m + 1.6f * b, m + 0.5f * b, 0.45f * b},                                  // LB
        {W - m - 0.5f * b, m + 0.5f * b, 0.45f * b},                              // RT
        {W - m - 1.6f * b, m + 0.5f * b, 0.45f * b},                              // RB
        {W / 2 - 0.75f * b, m * 0.5f + 0.35f * b, 0.35f * b},                     // BACK
        {W / 2 + 0.75f * b, m * 0.5f + 0.35f * b, 0.35f * b},                     // START
        {m + 2.75f * b, H - m - 2.3f * b, 0.32f * b},                             // LS
        {fx - 1.75f * b, fy - 1.3f * b, 0.32f * b},                               // RS
    };
    for (int i = 0; i < kButtons; ++i) {
        g_buttons[i].x = at[i].x; g_buttons[i].y = at[i].y; g_buttons[i].r = at[i].r;
        off(g_buttons[i].group, g_buttons[i].x, g_buttons[i].y);
    }
}

std::filesystem::path LayoutPath() { return renut::paths::WritableFolder() / "controls.txt"; }

void LoadLayout() {
    FILE *f = std::fopen(LayoutPath().string().c_str(), "r");
    if (!f)
        return;
    char key[32];
    float x, y;
    while (std::fscanf(f, "%31s %f %f", key, &x, &y) == 3) {
        if (std::strcmp(key, "enabled") == 0)
            g_enabled = x != 0;
        else if (int g = std::atoi(key); g >= 0 && g < G_COUNT && std::isdigit(key[0]))
            g_offs[g][0] = x, g_offs[g][1] = y;
    }
    std::fclose(f);
}

void SaveLayoutLocked() {
    FILE *f = std::fopen(LayoutPath().string().c_str(), "w");
    if (!f)
        return;
    std::fprintf(f, "enabled %d 0\n", g_enabled ? 1 : 0);
    for (int g = 0; g < G_COUNT; ++g)
        if (g_offs[g][0] || g_offs[g][1])
            std::fprintf(f, "%d %.4f %.4f\n", g, g_offs[g][0], g_offs[g][1]);
    std::fclose(f);
}

// Done / Reset in edit mode, top centre.
void EditButtons(float &done_x, float &reset_x, float &y, float &w, float &h) {
    w = 2.2f * g_unit; h = 0.7f * g_unit; y = 1.4f * g_unit;
    done_x = g_w / 2 + 0.15f * g_unit; reset_x = g_w / 2 - 0.15f * g_unit - w;
}

void Rebuild() {
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;
    for (const Finger &f : g_fingers) {
        switch (f.role) {
        case R_BUTTON: {
            const Button &b = g_buttons[f.index];
            buttons |= b.mask;
            if (b.trigger == 1) lt = 255;
            if (b.trigger == 2) rt = 255;
            break;
        }
        case R_DPAD: {
            const float dx = f.x - g_dpad_x, dy = f.y - g_dpad_y;
            if (std::hypot(dx, dy) < g_dpad_r * 0.25f)
                break;
            // 8 directions: diagonals between two arrows
            const int sector = int(std::lround(std::atan2(-dy, dx) / (M_PI / 4))) & 7;
            static const uint16_t kDirs[8] = {
                rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT,
                rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT | rex::input::X_INPUT_GAMEPAD_DPAD_UP,
                rex::input::X_INPUT_GAMEPAD_DPAD_UP,
                rex::input::X_INPUT_GAMEPAD_DPAD_UP | rex::input::X_INPUT_GAMEPAD_DPAD_LEFT,
                rex::input::X_INPUT_GAMEPAD_DPAD_LEFT,
                rex::input::X_INPUT_GAMEPAD_DPAD_LEFT | rex::input::X_INPUT_GAMEPAD_DPAD_DOWN,
                rex::input::X_INPUT_GAMEPAD_DPAD_DOWN,
                rex::input::X_INPUT_GAMEPAD_DPAD_DOWN | rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT};
            buttons |= kDirs[sector];
            break;
        }
        case R_STICK:
        case R_LOOK: {
            const float r = f.role == R_STICK ? g_stick_r : 0.8f * g_unit;
            float dx = (f.x - f.ax) / r, dy = (f.y - f.ay) / r;
            const float len = std::hypot(dx, dy);
            if (len > 1) dx /= len, dy /= len;
            (f.role == R_STICK ? lx : rx) = dx;
            (f.role == R_STICK ? ly : ry) = -dy;
            break;
        }
        default:
            break;
        }
    }
    if (buttons != g_pad_buttons || lt != g_lt || rt != g_rt || lx != g_lx || ly != g_ly ||
        rx != g_rx || ry != g_ry)
        ++g_packet;
    g_pad_buttons = buttons; g_lt = lt; g_rt = rt; g_lx = lx; g_ly = ly; g_rx = rx; g_ry = ry;
}

void ReleaseAllLocked() {
    for (Finger &f : g_fingers)
        f = Finger{};
    Rebuild();
}

Finger *FindFinger(SDL_FingerID id) {
    for (Finger &f : g_fingers)
        if (f.role != R_NONE && f.id == id)
            return &f;
    return nullptr;
}

void FingerDown(SDL_FingerID id, float x, float y) {
    Layout();
    Finger *f = nullptr;
    for (Finger &slot : g_fingers)
        if (slot.role == R_NONE) { f = &slot; break; }
    if (!f)
        return;
    *f = Finger{id, R_NONE, 0, x, y, x, y};
    const float u = g_unit;

    if (g_editing) {
        float done_x, reset_x, by, bw, bh;
        EditButtons(done_x, reset_x, by, bw, bh);
        if (y >= by && y < by + bh && x >= done_x && x < done_x + bw) {
            g_editing = false;
            SaveLayoutLocked();
            return;
        }
        if (y >= by && y < by + bh && x >= reset_x && x < reset_x + bw) {
            std::memset(g_offs, 0, sizeof(g_offs));
            SaveLayoutLocked();
            return;
        }
        int group = -1;
        if (std::hypot(x - g_stick_x, y - g_stick_y) < g_stick_r * 1.2f) group = G_STICK;
        if (std::hypot(x - g_dpad_x, y - g_dpad_y) < g_dpad_r * 1.2f) group = G_DPAD;
        for (const Button &b : g_buttons)
            if (std::hypot(x - b.x, y - b.y) < b.r * 1.3f) group = b.group;
        if (group >= 0) { f->role = R_DRAG; f->index = group; }
        return;
    }

    for (int i = 0; i < kButtons; ++i) {
        if (std::hypot(x - g_buttons[i].x, y - g_buttons[i].y) < g_buttons[i].r * 1.15f) {
            f->role = R_BUTTON; f->index = i;
            Rebuild();
            return;
        }
    }
    if (std::hypot(x - g_dpad_x, y - g_dpad_y) < g_dpad_r * 1.1f) {
        f->role = R_DPAD;
    } else if (x < g_w * 0.45f && y > g_h * 0.3f) {
        // The stick centres where the thumb lands, unless it lands on it.
        f->role = R_STICK;
        if (std::hypot(x - g_stick_x, y - g_stick_y) < g_stick_r) f->ax = g_stick_x, f->ay = g_stick_y;
    } else if (x >= g_w * 0.5f && y > 1.3f * u) {
        f->role = R_LOOK;
    }
    Rebuild();
}

void FingerMove(SDL_FingerID id, float x, float y) {
    Finger *f = FindFinger(id);
    if (!f)
        return;
    if (f->role == R_DRAG) {
        g_offs[f->index][0] += (x - f->x) / g_h;
        g_offs[f->index][1] += (y - f->y) / g_h;
    }
    f->x = x; f->y = y;
    Rebuild();
}

void FingerUp(SDL_FingerID id) {
    if (Finger *f = FindFinger(id)) {
        *f = Finger{};
        Rebuild();
    }
}

void CountGamepads() {
    int count = 0;
    if (SDL_JoystickID *pads = SDL_GetGamepads(&count))
        SDL_free(pads);
    g_gamepads = count;
}

bool SDLCALL WatchEvents(void *, SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED: {
        std::lock_guard lock(g_mutex);
        CountGamepads();
        if (!VisibleLocked())
            ReleaseAllLocked();
        break;
    }
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
        std::lock_guard lock(g_mutex);
        if (!VisibleLocked() || g_blocked)
            break;
        const float x = e->tfinger.x * g_w, y = e->tfinger.y * g_h;
        if (e->type == SDL_EVENT_FINGER_DOWN)
            FingerDown(e->tfinger.fingerID, x, y);
        else if (e->type == SDL_EVENT_FINGER_MOTION)
            FingerMove(e->tfinger.fingerID, x, y);
        else
            FingerUp(e->tfinger.fingerID);
        break;
    }
    default:
        break;
    }
    return true;
}

// --- the input driver ---------------------------------------------------------

constexpr auto kTouchDevice = static_cast<rex::input::DeviceId>(0x544F5543);  // "TOUC"

int16_t Axis(float v) { return int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f); }

class TouchDriver : public rex::input::InputDriver {
public:
    TouchDriver() : InputDriver(nullptr, 0) {}

    X_STATUS Setup() override { return X_STATUS_SUCCESS; }

    void EnumerateDevices(std::vector<rex::input::DeviceInfo> &out) override {
        if (!Visible())
            return;
        rex::input::DeviceInfo info;
        info.id = kTouchDevice;
        info.name = "Touch controls";
        info.subtype = rex::input::XINPUT_DEVSUBTYPE_GAMEPAD;
        info.synthetic = true;
        out.push_back(info);
    }

    X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE *out) override {
        std::lock_guard lock(g_mutex);
        if (id != kTouchDevice || !VisibleLocked())
            return X_ERROR_DEVICE_NOT_CONNECTED;
        if (out) {
            std::memset(out, 0, sizeof(*out));
            out->packet_number = g_packet;
            out->gamepad.buttons = g_pad_buttons;
            out->gamepad.left_trigger = g_lt;
            out->gamepad.right_trigger = g_rt;
            out->gamepad.thumb_lx = Axis(g_lx);
            out->gamepad.thumb_ly = Axis(g_ly);
            out->gamepad.thumb_rx = Axis(g_rx);
            out->gamepad.thumb_ry = Axis(g_ry);
        }
        return X_ERROR_SUCCESS;
    }

    X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                   rex::input::X_INPUT_CAPABILITIES *caps) override {
        if (id != kTouchDevice || !Visible())
            return X_ERROR_DEVICE_NOT_CONNECTED;
        if (caps) {
            std::memset(caps, 0, sizeof(*caps));
            caps->type = rex::input::XINPUT_DEVTYPE_GAMEPAD;
            caps->sub_type = rex::input::XINPUT_DEVSUBTYPE_GAMEPAD;
            caps->gamepad.buttons = 0xFFFF;
            caps->gamepad.left_trigger = 0xFF;
            caps->gamepad.right_trigger = 0xFF;
            caps->gamepad.thumb_lx = int16_t(0x7FFF);
            caps->gamepad.thumb_ly = int16_t(0x7FFF);
            caps->gamepad.thumb_rx = int16_t(0x7FFF);
            caps->gamepad.thumb_ry = int16_t(0x7FFF);
        }
        return X_ERROR_SUCCESS;
    }

    X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION *) override {
        return id == kTouchDevice && Visible() ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
    }

    // The XUI menus read keystrokes: one per changed key, ups first.
    X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                                rex::input::X_INPUT_KEYSTROKE *out) override {
        std::lock_guard lock(g_mutex);
        if (id != kTouchDevice || !VisibleLocked())
            return X_ERROR_DEVICE_NOT_CONNECTED;
        if (!out)
            return X_ERROR_BAD_ARGUMENTS;
        static constexpr VirtualKey kKeys[] = {
            VirtualKey::kXInputPadDpadUp, VirtualKey::kXInputPadDpadDown,
            VirtualKey::kXInputPadDpadLeft, VirtualKey::kXInputPadDpadRight,
            VirtualKey::kXInputPadStart, VirtualKey::kXInputPadBack,
            VirtualKey::kXInputPadLThumbPress, VirtualKey::kXInputPadRThumbPress,
            VirtualKey::kXInputPadLShoulder, VirtualKey::kXInputPadRShoulder,
            VirtualKey::kNone, VirtualKey::kNone,
            VirtualKey::kXInputPadA, VirtualKey::kXInputPadB,
            VirtualKey::kXInputPadX, VirtualKey::kXInputPadY,
            VirtualKey::kXInputPadLTrigger, VirtualKey::kXInputPadRTrigger,
            VirtualKey::kXInputPadLThumbUp, VirtualKey::kXInputPadLThumbDown,
            VirtualKey::kXInputPadLThumbRight, VirtualKey::kXInputPadLThumbLeft,
        };
        uint32_t now = g_pad_buttons;
        if (g_lt) now |= 1u << 16;
        if (g_rt) now |= 1u << 17;
        if (g_ly > 0.5f) now |= 1u << 18;
        if (g_ly < -0.5f) now |= 1u << 19;
        if (g_lx > 0.5f) now |= 1u << 20;
        if (g_lx < -0.5f) now |= 1u << 21;
        const uint32_t changed = now ^ reported_;
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 0; i < int(std::size(kKeys)); ++i) {
                const uint32_t bit = 1u << i;
                const bool down = (now & bit) != 0;
                if (!(changed & bit) || down != (pass == 1) || kKeys[i] == VirtualKey::kNone)
                    continue;
                reported_ ^= bit;
                std::memset(out, 0, sizeof(*out));
                out->virtual_key = uint16_t(kKeys[i]);
                out->flags = down ? rex::input::X_INPUT_KEYSTROKE_KEYDOWN
                                  : rex::input::X_INPUT_KEYSTROKE_KEYUP;
                return X_ERROR_SUCCESS;
            }
        }
        return X_ERROR_EMPTY;
    }

private:
    uint32_t reported_ = 0;
};

} // namespace

void Install(rex::input::InputSystem *input) {
    {
        std::lock_guard lock(g_mutex);
        LoadLayout();
        CountGamepads();
    }
    if (input)
        input->AddDriver(std::make_unique<TouchDriver>());
    SDL_AddEventWatch(WatchEvents, nullptr);
}

void SetStarted(bool started) {
    std::lock_guard lock(g_mutex);
    g_started = started;
}

void SetBlocked(bool blocked) {
    std::lock_guard lock(g_mutex);
    g_blocked = blocked;
    if (blocked)
        ReleaseAllLocked();
}

bool Enabled() {
    std::lock_guard lock(g_mutex);
    return g_enabled;
}

void SetEnabled(bool enabled) {
    std::lock_guard lock(g_mutex);
    g_enabled = enabled;
    ReleaseAllLocked();
    SaveLayoutLocked();
}

bool Visible() {
    std::lock_guard lock(g_mutex);
    return VisibleLocked();
}

bool GamepadConnected() {
    std::lock_guard lock(g_mutex);
    return g_gamepads > 0;
}

void BeginEdit() {
    std::lock_guard lock(g_mutex);
    ReleaseAllLocked();
    g_editing = true;
}

bool Editing() {
    std::lock_guard lock(g_mutex);
    return g_editing;
}

bool TakeBackPress() { return g_back.exchange(false); }

} // namespace renut::touch

// RenutActivity's back gesture / back key.
extern "C" JNIEXPORT void JNICALL Java_com_rexglue_renut_RenutActivity_nativeBackPressed(JNIEnv *,
                                                                                        jclass) {
    renut::touch::g_back = true;
}

namespace renut::touch {

void SetFont(ImFont *font) {
    std::lock_guard lock(g_mutex);
    g_font = font;
}

void Overlay::Draw() {
    std::lock_guard lock(g_mutex);
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    g_w = size.x;
    g_h = size.y;
    // Editing shows the controls even with a gamepad connected.
    if (!(g_started && (g_editing || (g_enabled && g_gamepads == 0))) || g_blocked)
        return;
    Layout();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *font = g_font ? g_font : ImGui::GetFont();
    const ImU32 fill = IM_COL32(10, 20, 60, 90), line = IM_COL32(255, 255, 255, 150),
                hot = IM_COL32(255, 255, 255, 110);
    const float thick = std::max(1.5f, g_unit * 0.03f);
    auto label = [&](const char *text, float x, float y, float px, ImU32 ink) {
        const ImVec2 ext = font->CalcTextSizeA(px, FLT_MAX, 0, text);
        dl->AddText(font, px, ImVec2(x - ext.x / 2 + 2, y - ext.y / 2 + 2), IM_COL32(0, 0, 40, 120), text);
        dl->AddText(font, px, ImVec2(x - ext.x / 2, y - ext.y / 2), ink, text);
    };

    // Left stick: base where the thumb is, knob at the deflection.
    float sx = g_stick_x, sy = g_stick_y;
    for (const Finger &f : g_fingers)
        if (f.role == R_STICK) sx = f.ax, sy = f.ay;
    dl->AddCircleFilled(ImVec2(sx, sy), g_stick_r, fill, 48);
    dl->AddCircle(ImVec2(sx, sy), g_stick_r, line, 48, thick);
    dl->AddCircleFilled(ImVec2(sx + g_lx * g_stick_r * 0.6f, sy - g_ly * g_stick_r * 0.6f),
                        g_stick_r * 0.42f, g_lx || g_ly ? hot : IM_COL32(255, 255, 255, 60), 32);

    // Right stick while the right half is held.
    for (const Finger &f : g_fingers) {
        if (f.role != R_LOOK) continue;
        const float r = 0.8f * g_unit;
        dl->AddCircle(ImVec2(f.ax, f.ay), r, line, 40, thick);
        dl->AddCircleFilled(ImVec2(f.ax + g_rx * r * 0.6f, f.ay - g_ry * r * 0.6f), r * 0.4f, hot, 32);
    }

    // D-pad: a cross.
    {
        const float r = g_dpad_r, w = r * 0.36f;
        auto arm = [&](float dx, float dy, uint16_t mask) {
            const ImVec2 lo(std::min(g_dpad_x + dx * r, g_dpad_x) - (dx ? 0 : w),
                            std::min(g_dpad_y + dy * r, g_dpad_y) - (dy ? 0 : w));
            const ImVec2 hi(std::max(g_dpad_x + dx * r, g_dpad_x) + (dx ? 0 : w),
                            std::max(g_dpad_y + dy * r, g_dpad_y) + (dy ? 0 : w));
            dl->AddRectFilled(lo, hi, (g_pad_buttons & mask) ? hot : fill, w * 0.3f);
            dl->AddRect(lo, hi, line, w * 0.3f, 0, thick);
        };
        arm(0, -1, rex::input::X_INPUT_GAMEPAD_DPAD_UP);
        arm(0, 1, rex::input::X_INPUT_GAMEPAD_DPAD_DOWN);
        arm(-1, 0, rex::input::X_INPUT_GAMEPAD_DPAD_LEFT);
        arm(1, 0, rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT);
    }

    for (const Button &b : g_buttons) {
        const bool down = b.trigger == 1 ? g_lt : b.trigger == 2 ? g_rt : (g_pad_buttons & b.mask);
        dl->AddCircleFilled(ImVec2(b.x, b.y), b.r, down ? hot : fill, 40);
        dl->AddCircle(ImVec2(b.x, b.y), b.r, line, 40, thick);
        const float px = std::strlen(b.label) > 2 ? b.r * 0.62f : b.r * 0.95f;
        label(b.label, b.x, b.y, px, b.ink);
    }

    if (g_editing) {
        float done_x, reset_x, by, bw, bh;
        EditButtons(done_x, reset_x, by, bw, bh);
        for (int i = 0; i < 2; ++i) {
            const float x = i ? reset_x : done_x;
            dl->AddRectFilled(ImVec2(x, by), ImVec2(x + bw, by + bh), IM_COL32(4, 10, 34, 230));
            dl->AddLine(ImVec2(x, by + bh), ImVec2(x + bw, by + bh), line, thick);
            label(i ? "RESET" : "DONE", x + bw / 2, by + bh / 2, bh * 0.6f, IM_COL32_WHITE);
        }
        label("DRAG THE CONTROLS WHERE YOU LIKE", g_w / 2, by + bh * 1.7f, bh * 0.45f,
              IM_COL32(255, 255, 255, 200));
    }
}

} // namespace renut::touch
