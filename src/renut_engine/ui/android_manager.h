#pragma once
// Android: the screen before the game, in NutStaller's blueprint style (its
// bg.png art and the Andy font, from the APK's assets). Picks the game
// folder, the Vulkan driver and the settings, gets workshop vehicles and part
// mods, manages the save; Play starts the game.
//
// Drawn in the overlay's ImGui on a 1200x675 design canvas, scaled to fit the
// screen's height like NutStaller's Viewbox; on wider screens the art's grid
// carries on to the edges. A gamepad (or keyboard) moves through it too:
// D-pad or stick to move, A to press, B back, LB/RB to change page, Start
// to play.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_keyboard.h>
#include <rex/cvar.h>
#include <rex/rex_app.h>
#include <rex/ui/image_decode.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "renut_engine/app/config_file.h"
#include "renut_engine/app/android_content.h"
#include "renut_engine/app/android_driver.h"
#include "renut_engine/app/path_config_store.h"
#include "renut_engine/app/xiso.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/ui/touch_controls.h"
#include "renut_engine/gpu/backend/native_window.h"
#include "renut_engine/gpu/overlay/overlay_ui.h"

class AndroidManager : public renut::gpu::overlay::Dialog {
public:
    using CompletionFn = std::function<void(rex::PathConfig)>;

    ~AndroidManager() override {
        extract_.cancel = true;
        if (extract_thread_.joinable())
            extract_thread_.join();
        if (worker_.joinable())
            worker_.join();
    }

    void Init(const std::string &app_name, const rex::PathConfig &defaults, CompletionFn on_play) {
        app_name_ = app_name;
        defaults_ = defaults;
        on_play_ = std::move(on_play);
        std::string game = "/sdcard/reNut/game";
        if (auto saved = PathConfigStore::TryLoad(app_name))
            game = saved->game_data_root.string();
        SetBuf(game_buf_, game);
        selected_driver_ = renut::android::SelectedDriver();
        RefreshDrivers();
        RefreshIsos();
        save_dir_ = renut::content::SaveDir(defaults.user_data_root);
        visible_ = true;
    }

    void Draw() override {
        const bool back = renut::touch::TakeBackPress();
        if (!visible_) {
            if (started_)
                DrawInGame(back);
            return;
        }
        LoadAssets();
        UploadPreviews();
        const Pad pad = FeedGamepad();
        ImGuiIO &io = ImGui::GetIO();
        panel_x_ = 634.0f;
        panel_y_ = 281.0f;
        settings_h_ = 316.0f;

        // The design canvas fills the screen's height, centred; wider screens
        // get more of the blueprint grid at the sides.
        scale_ = std::min(io.DisplaySize.x / kW, io.DisplaySize.y / kH);
        origin_ = ImVec2((io.DisplaySize.x - kW * scale_) * 0.5f, (io.DisplaySize.y - kH * scale_) * 0.5f);

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // buttons draw their own
        ImGui::Begin("reNut manager", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        Backdrop(dl, io.DisplaySize);
        if (bg_)
            dl->AddImage(bg_->GetTexRef(), P(0, 0), P(kW, kH));

        if (pad.lb || pad.rb) {
            const int n = int(Page::Count);
            page_ = Page((int(page_) + (pad.rb ? 1 : n - 1)) % n);
        }
        if (pad.start && !confirm_)
            Play();

        const bool modal = confirm_.has_value();
        if (modal)
            ImGui::BeginDisabled();
        static const char *kNav[] = {"Set Up Game", "Drivers", "Settings", "Workshop", "Saves", "Credits"};
        for (int i = 0; i < int(Page::Count); ++i)
            if (NavButton(kNav[i], 402, 318 + 33.0f * i, 185, 31, page_ == Page(i)))
                page_ = Page(i);
        if (NavButton("Play", 402, 522, 185, 36, false, 24)) Play();

        Panel(634, 281, 551, 382);
        switch (page_) {
        case Page::Game: DrawGame(); break;
        case Page::Drivers: DrawDrivers(); break;
        case Page::Settings: DrawSettings(); break;
        case Page::Workshop: DrawWorkshop(); break;
        case Page::Saves: DrawSaves(); break;
        case Page::Credits: DrawCredits(); break;
        case Page::Count: break;
        }
        if (modal) {
            ImGui::EndDisabled();
            DrawConfirm(pad.b);
        }

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        // Android's keyboard comes up only while a text field wants it.
        if (io.WantTextInput != text_input_) {
            text_input_ = io.WantTextInput;
            if (SDL_Window *window = renut::gpu::GetGameSdlWindow())
                text_input_ ? SDL_StartTextInput(window) : SDL_StopTextInput(window);
        }
    }

private:
    enum class Page { Game, Drivers, Settings, Workshop, Saves, Credits, Count };
    static constexpr float kW = 1200.0f, kH = 675.0f;
    static constexpr ImU32 kBlueprint = IM_COL32(40, 78, 164, 255);  // bg.png's paper
    static constexpr ImU32 kGrid = IM_COL32(58, 93, 172, 255);       // and its grid lines
    static constexpr ImU32 kInk = IM_COL32(255, 255, 255, 255);
    static constexpr ImU32 kInkDim = IM_COL32(255, 255, 255, 200);
    static constexpr ImU32 kShadow = IM_COL32(0, 0, 40, 120);
    static constexpr ImU32 kLineDim = IM_COL32(255, 255, 255, 110);
    static constexpr ImU32 kNavy = IM_COL32(10, 20, 60, 255);
    static constexpr size_t kBufSize = 512;

    // --- assets -------------------------------------------------------------

    void LoadAssets() {
        if (assets_loaded_)
            return;
        assets_loaded_ = true;
        size_t size = 0;
        // Relative paths are the APK's assets on Android.
        if (void *ttf = SDL_LoadFile("ANDYB.TTF", &size)) {
            void *copy = IM_ALLOC(size);  // the atlas frees it with IM_FREE
            std::memcpy(copy, ttf, size);
            SDL_free(ttf);
            font_ = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(copy, int(size), 22.0f);
            renut::touch::SetFont(font_);
        }
        if (void *png = SDL_LoadFile("bg.png", &size)) {
            int w = 0, h = 0;
            auto pixels = rex::ui::DecodeImageRGBA(static_cast<const uint8_t *>(png), size, w, h);
            SDL_free(png);
            if (!pixels.empty()) {
                bg_ = IM_NEW(ImTextureData)();
                bg_->Create(ImTextureFormat_RGBA32, w, h);
                std::memcpy(bg_->GetPixels(), pixels.data(), pixels.size());
                ImGui::RegisterUserTexture(bg_);
            }
        }
    }

    // --- drawing helpers, in design units --------------------------------------

    ImVec2 P(float x, float y) const { return ImVec2(origin_.x + x * scale_, origin_.y + y * scale_); }
    ImVec2 S(float w, float h) const { return ImVec2(w * scale_, h * scale_); }
    ImFont *Font() const { return font_ ? font_ : ImGui::GetFont(); }

    // Andy text with NutStaller's soft drop shadow, vertically centred.
    void Text(const std::string &text, float x, float y, float w, float h, float size,
              ImU32 ink = kInk, bool center = false, bool shadow = true) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float px = size * scale_;
        const ImVec2 extent = Font()->CalcTextSizeA(px, FLT_MAX, w * scale_, text.c_str());
        ImVec2 pos = P(x, y);
        pos.y += (h * scale_ - extent.y) * 0.5f;
        if (center)
            pos.x += (w * scale_ - extent.x) * 0.5f;
        const ImVec4 clip(P(x, y).x, P(x, y).y, P(x + w, y + h).x, P(x + w, y + h).y);
        if (shadow)
            dl->AddText(Font(), px, ImVec2(pos.x + 2 * scale_, pos.y + 2 * scale_), kShadow,
                        text.c_str(), nullptr, w * scale_, &clip);
        dl->AddText(Font(), px, pos, ink, text.c_str(), nullptr, w * scale_, &clip);
    }

    static std::string Upper(std::string s) {
        for (char &c : s)
            c = char(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    // The paper colour and the art's 30-unit grid (lines at x = 2 + 30k,
    // y = 7 + 30k), over the whole screen; bg.png covers the middle.
    void Backdrop(ImDrawList *dl, ImVec2 display) {
        dl->AddRectFilled(ImVec2(0, 0), display, kBlueprint);
        const float x0 = -origin_.x / scale_, x1 = (display.x - origin_.x) / scale_;
        const float y0 = -origin_.y / scale_, y1 = (display.y - origin_.y) / scale_;
        for (float x = 2 + 30 * std::floor((x0 - 2) / 30); x <= x1; x += 30)
            dl->AddRectFilled(ImVec2(P(x, 0).x, 0), ImVec2(P(x + 2, 0).x, display.y), kGrid);
        for (float y = 7 + 30 * std::floor((y0 - 7) / 30); y <= y1; y += 30)
            dl->AddRectFilled(ImVec2(0, P(0, y).y), ImVec2(display.x, P(0, y + 2).y), kGrid);
    }

    // A press that turned into a scroll is no tap; a gamepad or key press is.
    static bool Tapped(bool clicked, float scale) {
        const float slop = 12.0f * scale;
        return clicked && (!ImGui::IsMouseReleased(ImGuiMouseButton_Left) ||
                           ImGui::GetIO().MouseDragMaxDistanceSqr[0] < slop * slop);
    }

    // The gamepad's (or keyboard's) cursor: NutStaller's dashed white outline.
    void FocusRing(ImVec2 a, ImVec2 b) {
        if (!ImGui::IsItemFocused() || !GImGui->NavCursorVisible)
            return;
        const float x = (a.x - origin_.x) / scale_, y = (a.y - origin_.y) / scale_;
        Dashed(x + 1, y + 1, (b.x - a.x) / scale_ - 2, (b.y - a.y) / scale_ - 2, kInk, 3, 2);
    }

    // BkButton at a screen position: a gradient slab, darker on the left, with
    // a faint line under it.
    // id: a stable one for buttons in scrolled lists (else label + position).
    bool SlabButton(const std::string &label, ImVec2 a, ImVec2 size_px, bool selected, float size,
                    const char *id = nullptr) {
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(id ? id : label.c_str());
        ImGui::PushID(id ? 0 : int(a.x) * 7919 + int(a.y));
        const bool clicked = Tapped(ImGui::InvisibleButton("##b", size_px), scale_);
        ImGui::PopID();
        ImGui::PopID();
        const ImVec2 b(a.x + size_px.x, a.y + size_px.y);
        const bool down = ImGui::IsItemActive();
        const bool hot = ImGui::IsItemHovered() || (ImGui::IsItemFocused() && GImGui->NavCursorVisible);
        const int left = selected || down ? 250 : hot ? 225 : 185;
        const int right = selected || down ? 90 : hot ? 45 : 15;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImU32 l = IM_COL32(4, 10, 34, left), r = IM_COL32(4, 10, 34, right);
        dl->AddRectFilledMultiColor(a, b, l, r, r, l);
        dl->AddLine(ImVec2(a.x, b.y - 0.5f * scale_), ImVec2(b.x, b.y - 0.5f * scale_), kLineDim);
        TextAt(Upper(label), ImVec2(a.x + 10 * scale_, a.y + scale_), size_px.x - 14 * scale_,
               size_px.y - 2 * scale_, size);
        FocusRing(a, b);
        return clicked;
    }

    bool NavButton(const std::string &label, float x, float y, float w, float h, bool selected,
                   float size = 22) {
        return SlabButton(label, P(x, y), S(w, h), selected, size);
    }

    // Text at a screen position (for scrolled lists), one line, clipped.
    void TextAt(const std::string &text, ImVec2 pos, float w_px, float h_px, float size,
                ImU32 ink = kInk) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float px = size * scale_;
        const ImVec4 clip(pos.x, pos.y, pos.x + w_px, pos.y + h_px);
        const ImVec2 at(pos.x, pos.y + (h_px - px) * 0.5f);
        dl->AddText(Font(), px, ImVec2(at.x + 2 * scale_, at.y + 2 * scale_), kShadow, text.c_str(), nullptr,
                    0, &clip);
        dl->AddText(Font(), px, at, ink, text.c_str(), nullptr, 0, &clip);
    }

    // BkPagePanel: translucent navy with a dashed border.
    void Panel(float x, float y, float w, float h) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(P(x, y), P(x + w, y + h), IM_COL32(14, 28, 80, 170));
        Dashed(x + 1.5f, y + 1.5f, w - 3, h - 3, kLineDim, 3);
    }

    void Dashed(float x, float y, float w, float h, ImU32 col, float dash, float thick = 1) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        auto line = [&](float x0, float y0, float x1, float y1) {
            const float len = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
            for (float t = 0; t < len; t += dash * 2) {
                const float e = std::min(t + dash, len);
                dl->AddLine(P(x0 + (x1 - x0) * t / len, y0 + (y1 - y0) * t / len),
                            P(x0 + (x1 - x0) * e / len, y0 + (y1 - y0) * e / len), col,
                            thick * scale_);
            }
        };
        line(x, y, x + w, y);
        line(x + w, y, x + w, y + h);
        line(x + w, y + h, x, y + h);
        line(x, y + h, x, y);
    }

    // Page coordinates are relative to the panel.
    void Label(const std::string &text, float x, float y, float w, float h, float size = 18,
               bool dim = false) {
        Text(text, panel_x_ + x, panel_y_ + y, w, h, size, dim ? kInkDim : kInk);
    }

    bool Button(const std::string &text, float x, float y, float w, float h, float size = 20,
                bool selected = false) {
        return NavButton(text, panel_x_ + x, panel_y_ + y, w, h, selected, size);
    }

    // BkValueBox: a navy slab with an editable value.
    bool Field(const char *id, std::string &buf, float x, float y, float w, float h) {
        ImGui::SetCursorScreenPos(P(panel_x_ + x, panel_y_ + y));
        ImGui::PushFont(Font(), 18 * scale_);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(kNavy));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImGui::ColorConvertU32ToFloat4(kNavy));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(30 / 255.f, 55 / 255.f, 130 / 255.f, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(6 * scale_, (h * scale_ - 18 * scale_) * 0.5f));
        ImGui::SetNextItemWidth(w * scale_);
        const bool changed = ImGui::InputText(id, buf.data(), kBufSize);
        FocusRing(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        ImGui::PopFont();
        return changed;
    }

    static void SetBuf(std::string &buf, const std::string &value) {
        buf = value;
        buf.resize(kBufSize, '\0');
    }

    // --- pages -----------------------------------------------------------------

    void DrawGame() {
        Label("SET UP GAME", 18, 16, 400, 30, 26);
        Label("GAME FOLDER (THE ONE WITH DEFAULT.XEX)", 19, 58, 500, 26, 16, true);
        Field("##game", game_buf_, 19, 88, 513, 32);

        const std::filesystem::path game = game_buf_.c_str();
        std::error_code ec;
        const bool storage = renut::android::HasStorageAccess();
        std::string state;
        if (!storage)
            state = "RENUT CAN'T READ SHARED STORAGE YET";
        else if (std::filesystem::exists(game / "default.xex", ec))
            state = "DEFAULT.XEX FOUND - READY TO PLAY";
        else
            state = "NO DEFAULT.XEX IN THIS FOLDER";
        Label(state, 19, 128, 400, 28, 18, !storage);
        if (!storage && Button("Allow file access", 19, 160, 240, 32))
            renut::android::RequestStorageAccess();

        DrawExtract(storage);
        Label("DRIVER: " + Upper(renut::android::DriverStatus()), 19, 350, 513, 24, 15, true);
        if (!error_.empty())
            Label(error_, 19, 200, 513, 24, 16);
    }

    // --- disc image extraction (extract-xiso) ------------------------------------

    void RefreshIsos() {
        isos_.clear();
        std::error_code ec;
        for (const char *dir : {"/sdcard/reNut", "/sdcard/Download"}) {
            for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
                std::string ext = entry.path().extension().string();
                for (char &c : ext)
                    c = char(std::tolower(static_cast<unsigned char>(c)));
                if (entry.is_regular_file(ec) && ext == ".iso")
                    isos_.push_back(entry.path());
            }
        }
        std::sort(isos_.begin(), isos_.end());
    }

    bool Extracting() const { return extract_.running; }

    void DrawExtract(bool storage) {
        if (extract_thread_.joinable() && !extract_.running)
            extract_thread_.join();

        Label("OR EXTRACT A DISC IMAGE (.ISO IN /SDCARD/RENUT OR DOWNLOAD):", 19, 206, 513, 22, 15,
              true);
        if (Extracting()) {
            const float done = extract_.total ? float(extract_.done) / float(extract_.total) : 0.0f;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            // BkProgress: a white outline filled from the left.
            dl->AddRect(P(panel_x_ + 19, panel_y_ + 236), P(panel_x_ + 419, panel_y_ + 252), kInk);
            dl->AddRectFilled(P(panel_x_ + 21, panel_y_ + 238),
                              P(panel_x_ + 21 + 396 * done, panel_y_ + 250), kInk);
            if (Button("Cancel", 430, 230, 105, 28, 17))
                extract_.cancel = true;
            std::string current;
            {
                std::lock_guard lock(extract_.mutex);
                current = extract_.current;
            }
            Label(std::to_string(int(done * 100)) + "% " + Upper(current), 19, 262, 513, 22, 14,
                  true);
            return;
        }
        float y = 232;
        if (!storage) {
            Label("ALLOW FILE ACCESS TO SEE THEM", 19, y, 513, 22, 15, true);
            return;
        }
        if (isos_.empty())
            Label("NONE FOUND", 19, y, 300, 22, 15, true);
        for (size_t i = 0; i < isos_.size() && i < 2; ++i, y += 32) {
            if (Button("Extract " + isos_[i].filename().string(), 19, y, 405, 28, 17))
                StartExtract(isos_[i]);
        }
        if (Button("Rescan", 430, 232, 105, 28, 17))
            RefreshIsos();
        if (!extract_message_.empty())
            Label(extract_message_, 19, 300, 513, 40, 15);
    }

    void StartExtract(const std::filesystem::path &iso) {
        std::filesystem::path out = game_buf_.c_str();
        if (out.empty())
            out = "/sdcard/reNut/game";
        SetBuf(game_buf_, out.string());
        error_.clear();
        extract_message_.clear();
        extract_.cancel = false;
        extract_.running = true;
        extract_thread_ = std::thread([this, iso, out] {
            const bool ok = renut::xiso::Extract(iso, out, extract_);
            {
                std::lock_guard lock(extract_.mutex);
                extract_message_ = ok ? "EXTRACTED TO " + Upper(out.string())
                                      : "EXTRACTION FAILED: " + Upper(extract_.error);
            }
            extract_.running = false;
        });
    }

    void RefreshDrivers() {
        drivers_ = renut::android::InstalledDrivers();
        zips_ = renut::android::DriverZips();
    }

    void DrawDrivers() {
        Label("VULKAN DRIVER", 18, 16, 400, 30, 26);
        Label("NOW: " + Upper(renut::android::DriverStatus()), 19, 50, 513, 24, 16, true);

        float y = 82;
        if (Button("System driver", 19, y, 300, 30, 20, selected_driver_.empty()))
            selected_driver_.clear();
        y += 36;
        for (const auto &d : drivers_) {
            if (y > 260)
                break;
            if (Button(d.name, 19, y, 300, 30, 20, selected_driver_ == d.id))
                selected_driver_ = d.id;
            if (Button("Remove", 330, y, 110, 30, 18)) {
                renut::android::RemoveDriver(d.id);
                if (selected_driver_ == d.id)
                    selected_driver_.clear();
                RefreshDrivers();
                return;
            }
            y += 36;
        }

        // Zips in /sdcard/reNut/drivers, not yet installed.
        y = std::max(y + 6, 200.0f);
        Label("INSTALL FROM /SDCARD/RENUT/DRIVERS:", 19, y, 513, 22, 15, true);
        y += 26;
        if (zips_.empty())
            Label("NO DRIVER ZIPS THERE (ADRENOTOOLS FORMAT, E.G. TURNIP)", 19, y, 513, 22, 15, true);
        for (const auto &zip : zips_) {
            if (y > 300)
                break;
            if (Button("+ " + zip.filename().string(), 19, y, 420, 28, 17)) {
                std::string error;
                const std::string id = renut::android::InstallDriverZip(zip, &error);
                driver_message_ = id.empty() ? "COULDN'T INSTALL: " + Upper(error) : "INSTALLED " + Upper(id);
                if (!id.empty())
                    selected_driver_ = id;
                RefreshDrivers();
                return;
            }
            y += 32;
        }
        if (Button("Rescan", 450, 82, 85, 30, 18))
            RefreshDrivers();

        const bool changed = selected_driver_ != renut::android::SelectedDriver() ||
                             selected_driver_ != renut::android::LoadedDriver();
        if (changed) {
            if (Button("Restart to apply", 19, 336, 240, 32)) {
                renut::android::SelectDriver(selected_driver_);
                SaveSettings();
                renut::android::RestartApp();
            }
        } else if (!driver_message_.empty()) {
            Label(driver_message_, 19, 336, 513, 32, 16, true);
        }
    }

    // reNut's own options (and a few of the SDK's), straight from the cvars.
    static bool ShownInSettings(const rex::cvar::FlagEntry &f) {
        static const char *kHidden[] = {"renut_gpu_dred", "renut_vulkan_debug_layers",
                                        "renut_gpu_backend", "renut_pipeline_list",
                                        "renut_shader_capture_dir", "renut_mods_folder",
                                        "renut_online_trace", "renut_gamertag", "renut_online"};
        for (const char *h : kHidden)
            if (f.name == h)
                return false;
        if (f.name == "audio_mute" || f.name == "vibration")
            return true;
        if (f.category.rfind("Nuts&Bolts/", 0) != 0 || f.category == "Nuts&Bolts/Debug")
            return false;
        return f.type == rex::cvar::FlagType::Boolean ||
               (f.type == rex::cvar::FlagType::String && f.constraints.HasAllowedValues());
    }

    void DrawSettings() {
        Label("SETTINGS", 18, 16, 300, 30, 26);
        if (!settings_message_.empty())
            Label(settings_message_, 300, 16, 235, 30, 16, true);

        ImGui::SetCursorScreenPos(P(panel_x_ + 12, panel_y_ + 54));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("##settings", S(527, settings_h_), ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoScrollbar);
        // Touch scrolling: drag the list.
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
            ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
        }

        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float row = 38 * scale_;
        int n = 0;
        std::string category;
        for (const auto &f : rex::cvar::GetRegistry()) {
            if (!ShownInSettings(f))
                continue;
            const ImVec2 at(top.x, top.y + n * row);
            const std::string value = f.getter ? f.getter() : std::string();
            std::string title = f.name;
            if (title.rfind("renut_", 0) == 0)
                title = title.substr(6);
            std::replace(title.begin(), title.end(), '_', ' ');

            dl->AddText(Font(), 18 * scale_, ImVec2(at.x + 2 * scale_, at.y + 10 * scale_), kShadow,
                        Upper(title).c_str());
            dl->AddText(Font(), 18 * scale_, ImVec2(at.x, at.y + 8 * scale_), kInkDim,
                        Upper(title).c_str());

            // The value: a slab that steps through the choices on a tap.
            ImGui::SetCursorScreenPos(ImVec2(at.x + 300 * scale_, at.y + 3 * scale_));
            ImGui::PushID(f.name.c_str());
            const ImVec2 size = S(210, 30);
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const bool tapped = Tapped(ImGui::InvisibleButton("##v", size), scale_);
            const ImU32 slab = ImGui::IsItemActive() ? IM_COL32(30, 55, 130, 255) : kNavy;
            dl->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), slab);
            FocusRing(a, ImVec2(a.x + size.x, a.y + size.y));
            std::string shown = value;
            if (f.type == rex::cvar::FlagType::Boolean)
                shown = value == "true" ? "ON" : "OFF";
            const ImVec2 ext = Font()->CalcTextSizeA(18 * scale_, FLT_MAX, 0, Upper(shown).c_str());
            dl->AddText(Font(), 18 * scale_,
                        ImVec2(a.x + (size.x - ext.x) * 0.5f, a.y + (size.y - ext.y) * 0.5f), kInk,
                        Upper(shown).c_str());
            ImGui::PopID();
            if (tapped) {
                std::string next = value == "true" ? "false" : "true";
                if (f.type == rex::cvar::FlagType::String) {
                    const auto &values = f.constraints.allowed_values;
                    auto it = std::find(values.begin(), values.end(), value);
                    next = it == values.end() || it + 1 == values.end() ? values.front() : *(it + 1);
                }
                rex::cvar::SetFlagByName(f.name, next);
                SaveSettings();
            }
            ++n;
        }
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + n * row));
        ImGui::Dummy(ImVec2(1, 1));
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void SaveSettings() {
        if (!defaults_.config_path.empty()) {
            renut::config::Save(defaults_.config_path);
            settings_message_ = "SAVED";
        }
    }

    void DrawCredits() {
        Label("CREDITS", 16, 14, 300, 30, 26);
        Label("RENUT BY MASTERSPIKE52 AND CONTRIBUTORS", 16, 56, 516, 26);
        static const char *kLines[] = {
            "REXGLUE TEAM - REXGLUE SDK",
            "SOLARCOOKIES - MIDASM HOOKS AND CRT FUNCTIONS",
            "VALCOMDRIFTY - RENUT LOGO",
            "OLIEGAMERTV - NUTS AND BOLTS DOCUMENTATION",
            "BYLAWS - LIBADRENOTOOLS",
            "ANDROID PORT, NUTSTALLER, WORKSHOP - ETONEDEMID",
        };
        for (int i = 0; i < int(std::size(kLines)); i++)
            Label(kLines[i], 16, 88 + i * 26.0f, 516, 24, 16, true);
    }

    // --- gamepad -------------------------------------------------------------------

    // Presses this frame the manager handles itself (ImGui gets the D-pad, the
    // left stick as a D-pad, A and B).
    struct Pad {
        bool lb = false, rb = false, start = false, b = false;
    };

    Pad FeedGamepad() {
        ImGuiIO &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        enum { kUp, kDown, kLeft, kRight, kA, kB, kLB, kRB, kStart, kCount };
        static constexpr SDL_GamepadButton kButtons[kCount] = {
            SDL_GAMEPAD_BUTTON_DPAD_UP,       SDL_GAMEPAD_BUTTON_DPAD_DOWN,      SDL_GAMEPAD_BUTTON_DPAD_LEFT,
            SDL_GAMEPAD_BUTTON_DPAD_RIGHT,    SDL_GAMEPAD_BUTTON_SOUTH,          SDL_GAMEPAD_BUTTON_EAST,
            SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_START};
        static constexpr ImGuiKey kKeys[kCount] = {
            ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown,  ImGuiKey_GamepadDpadLeft,
            ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight,
            ImGuiKey_None,          ImGuiKey_None,             ImGuiKey_None};
        bool down[kCount] = {};
        int count = 0;
        if (SDL_JoystickID *ids = SDL_GetGamepads(&count)) {
            for (int i = 0; i < count; ++i) {
                SDL_Gamepad *pad = SDL_GetGamepadFromID(ids[i]);
                if (!pad)
                    pad = SDL_OpenGamepad(ids[i]);  // left open; SDL counts the opens
                if (!pad)
                    continue;
                for (int k = 0; k < kCount; ++k)
                    down[k] |= SDL_GetGamepadButton(pad, kButtons[k]);
                const int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
                const int y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
                down[kLeft] |= x < -20000;
                down[kRight] |= x > 20000;
                down[kUp] |= y < -20000;
                down[kDown] |= y > 20000;
            }
            SDL_free(ids);
        }
        Pad pad;
        for (int k = 0; k < kCount; ++k) {
            if (down[k] == pad_down_[k])
                continue;
            pad_down_[k] = down[k];
            if (kKeys[k] != ImGuiKey_None)
                io.AddKeyEvent(kKeys[k], down[k]);
            if (down[k]) {
                pad.lb |= k == kLB;
                pad.rb |= k == kRB;
                pad.start |= k == kStart;
                pad.b |= k == kB;
            }
        }
        return pad;
    }

    // --- confirmation --------------------------------------------------------------

    struct Confirm {
        std::string text;
        std::function<void()> yes;
        bool focused = false;
    };

    void Ask(std::string text, std::function<void()> yes) { confirm_ = Confirm{std::move(text), std::move(yes)}; }

    void DrawConfirm(bool cancel) {
        ImGuiIO &io = ImGui::GetIO();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 20, 140));
        Panel(330, 245, 540, 185);
        Text(confirm_->text, 352, 260, 496, 110, 18);
        if (!confirm_->focused) {
            ImGui::SetKeyboardFocusHere();
            confirm_->focused = true;
        }
        const bool yes = NavButton("Yes", 548, 380, 140, 34, false);
        const bool no = NavButton("No", 704, 380, 140, 34, false);
        if (yes) {
            auto action = std::move(confirm_->yes);
            confirm_.reset();
            action();
        } else if (no || cancel) {
            confirm_.reset();
        }
    }

    // --- scrolled lists ------------------------------------------------------------

    struct Row {
        std::string title, line1, line2, line3, preview, button;
        std::function<void()> action;
    };

    // Rows with a preview, four lines and a button, dragged to scroll.
    void DrawRows(const char *id, float x, float y, float w, float h, const std::vector<Row> &rows,
                  const std::string &empty) {
        ImGui::SetCursorScreenPos(P(panel_x_ + x, panel_y_ + y));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild(id, S(w, h), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f))
            ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float row_h = 88;
        if (rows.empty())
            TextAt(empty, top, w * scale_, 24 * scale_, 15, kInkDim);
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row &r = rows[i];
            const ImVec2 at(top.x, top.y + i * row_h * scale_);
            auto D = [&](float dx, float dy) { return ImVec2(at.x + dx * scale_, at.y + dy * scale_); };
            dl->AddRectFilled(D(0, 6), D(128, 78), kNavy);
            auto preview = previews_.find(r.preview);
            if (preview != previews_.end() && preview->second.tex) {
                const Preview &pv = preview->second;
                const float k = std::min(128.0f / pv.w, 72.0f / pv.h);
                const float pw = pv.w * k, ph = pv.h * k;
                dl->AddImage(pv.tex->GetTexRef(), D((128 - pw) / 2, 6 + (72 - ph) / 2),
                             D((128 + pw) / 2, 6 + (72 + ph) / 2));
            }
            TextAt(Upper(r.title), D(138, 2), 255 * scale_, 24 * scale_, 19);
            TextAt(Upper(r.line1), D(138, 26), 255 * scale_, 18 * scale_, 14, kInkDim);
            TextAt(r.line2, D(138, 44), 255 * scale_, 18 * scale_, 14, kInkDim);
            TextAt(Upper(r.line3), D(138, 62), 255 * scale_, 18 * scale_, 13, kInkDim);
            dl->AddLine(D(0, row_h - 2), D(w - 4, row_h - 2), kLineDim);
            if (!r.button.empty()) {
                ImGui::PushID(int(i));
                if (SlabButton(r.button, D(w - 122, 26), S(110, 32), false, 18, "act") && !busy_)
                    pending_ = r.action;  // run after the lock is let go
                ImGui::PopID();
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + rows.size() * row_h * scale_));
        ImGui::Dummy(ImVec2(1, 1));
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // --- workshop ------------------------------------------------------------------

    struct Preview {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        ImTextureData *tex = nullptr;
    };

    // One job at a time off the render thread (downloads).
    void StartJob(std::function<void()> job) {
        if (busy_)
            return;
        if (worker_.joinable())
            worker_.join();
        busy_ = true;
        worker_ = std::thread([this, job = std::move(job)] {
            job();
            busy_ = false;
        });
    }

    void SetStatus(std::string status) {
        std::lock_guard lock(mutex_);
        status_ = std::move(status);
    }

    // Under mutex_.
    void LoadInstalledLocked() {
        installed_ = renut::content::LoadInstalled(save_dir_);
        installed_mods_ = renut::content::LoadInstalledMods(save_dir_);
    }

    void RefreshWorkshop() {
        SetStatus("LOADING THE WORKSHOP...");
        StartJob([this] {
            std::string error;
            auto index = renut::content::FetchIndex(&error);
            {
                std::lock_guard lock(mutex_);
                LoadInstalledLocked();
                if (index)
                    index_ = *index;
                status_ = index ? "" : "ERROR: " + Upper(error);
            }
            if (!index)
                return;
            std::vector<std::string> paths;
            for (const auto &b : index->blueprints)
                paths.push_back(b.preview);
            for (const auto &m : index->mods)
                paths.push_back(m.preview);
            for (const auto &path : paths) {
                {
                    std::lock_guard lock(mutex_);
                    if (path.empty() || previews_.count(path))
                        continue;
                }
                Preview preview;
                if (auto data = renut::content::Download(path))
                    preview.rgba = renut::android::DecodeImage(*data, 320, preview.w, preview.h);
                std::lock_guard lock(mutex_);
                previews_[path] = std::move(preview);
            }
        });
    }

    // Decoded previews become textures here, on the render thread.
    void UploadPreviews() {
        std::lock_guard lock(mutex_);
        for (auto &[path, p] : previews_) {
            if (p.tex || p.rgba.empty())
                continue;
            p.tex = IM_NEW(ImTextureData)();
            p.tex->Create(ImTextureFormat_RGBA32, p.w, p.h);
            std::memcpy(p.tex->GetPixels(), p.rgba.data(), p.rgba.size());
            ImGui::RegisterUserTexture(p.tex);
            p.rgba = {};
        }
    }

    void RunWorkshopJob(std::string doing, std::function<bool(std::string *)> job, std::string done) {
        SetStatus(Upper(doing));
        StartJob([this, job = std::move(job), done = std::move(done)] {
            std::string error;
            const bool ok = job(&error);
            std::lock_guard lock(mutex_);
            LoadInstalledLocked();
            status_ = ok ? Upper(done) : "ERROR: " + Upper(error);
        });
    }

    std::string ModName(const renut::content::Index &index, const std::string &id) const {
        for (const auto &m : index.mods)
            if (m.id == id)
                return m.name;
        return "mod " + id;
    }

    static std::string Join(const std::vector<std::string> &items) {
        std::string out;
        for (const auto &i : items)
            out += (out.empty() ? "" : ", ") + i;
        return out;
    }

    void DrawWorkshop() {
        Label("WORKSHOP", 18, 16, 300, 30, 26);
        if (!workshop_loaded_) {
            workshop_loaded_ = true;
            RefreshWorkshop();
        }
        if (Button(busy_ ? "Working..." : "Refresh", 400, 14, 135, 30, 18) && !busy_)
            RefreshWorkshop();
        static const char *kTabs[] = {"Vehicles", "Parts", "Installed"};
        for (int i = 0; i < 3; ++i)
            if (Button(kTabs[i], 19 + i * 132.0f, 52, 126, 28, 18, workshop_tab_ == i))
                workshop_tab_ = i;

        std::optional<renut::content::Index> index;
        std::vector<renut::content::Installed> installed;
        std::vector<renut::content::InstalledMod> installed_mods;
        std::string status;
        {
            std::lock_guard lock(mutex_);
            index = index_;
            installed = installed_;
            installed_mods = installed_mods_;
            status = status_;
        }
        auto has = [&](const std::string &id) {
            return std::any_of(installed.begin(), installed.end(), [&](const auto &i) { return i.id == id; });
        };
        auto has_mod = [&](const std::string &mod_id) {
            return std::any_of(installed_mods.begin(), installed_mods.end(),
                               [&](const auto &m) { return m.mod_id == mod_id; });
        };
        const auto save = save_dir_;
        std::vector<Row> rows;
        std::string empty;
        if (workshop_tab_ == 0 && index) {
            empty = "NO VEHICLES IN THE WORKSHOP YET";
            for (const auto &e : index->blueprints) {
                std::vector<std::string> needs;
                for (const auto &id : e.requires_mods)
                    needs.push_back(ModName(*index, id));
                for (const auto &m : e.missing)
                    needs.push_back(m + " - not in the workshop");
                Row r{e.name, "by " + e.author + " - " + std::to_string(e.parts) + " parts", e.description,
                      needs.empty() ? "" : "needs: " + Join(needs), e.preview, has(e.id) ? "Remove" : "Install"};
                if (has(e.id))
                    r.action = [this, save, e] {
                        RunWorkshopJob("removing " + e.name + "...", [save, e](std::string *) {
                            renut::content::RemoveVehicle(save, e.id);
                            return true;
                        }, "removed " + e.name);
                    };
                else
                    r.action = [this, save, e, idx = *index] {
                        RunWorkshopJob("installing " + e.name + "...", [save, e, idx](std::string *error) {
                            return renut::content::InstallVehicle(save, e, idx, error);
                        }, "installed " + e.name + " - it is in the garage");
                    };
                rows.push_back(std::move(r));
            }
        } else if (workshop_tab_ == 1 && index) {
            empty = "NO PART MODS IN THE WORKSHOP YET";
            for (const auto &m : index->mods) {
                Row r{m.name,
                      "by " + m.author + (m.version.empty() ? "" : " - v" + m.version) + " - " +
                          std::to_string(m.part_names.size()) + " parts",
                      m.description, "parts: " + Join(m.part_names), m.preview,
                      has_mod(m.mod_id) ? "Remove" : "Install"};
                if (has_mod(m.mod_id))
                    r.action = [this, save, m] {
                        RunWorkshopJob("removing " + m.name + "...", [save, m](std::string *) {
                            renut::content::RemoveMod(save, m.mod_id);
                            return true;
                        }, "removed " + m.name);
                    };
                else
                    r.action = [this, save, m] {
                        RunWorkshopJob("installing " + m.name + "...", [save, m](std::string *error) {
                            return renut::content::InstallMod(save, m, error);
                        }, "installed " + m.name);
                    };
                rows.push_back(std::move(r));
            }
        } else if (workshop_tab_ == 2) {
            empty = "NOTHING FROM THE WORKSHOP YET";
            for (const auto &i : installed) {
                Row r{i.name, "vehicle by " + i.author + " - " + std::to_string(i.parts) + " parts", i.description,
                      "", i.preview, "Remove"};
                r.action = [this, save, i] {
                    RunWorkshopJob("removing " + i.name + "...", [save, i](std::string *) {
                        renut::content::RemoveVehicle(save, i.id);
                        return true;
                    }, "removed " + i.name);
                };
                rows.push_back(std::move(r));
            }
            for (const auto &m : installed_mods) {
                Row r{m.name, "part mod by " + m.author + " - " + std::to_string(m.parts) + " parts",
                      m.description, "", m.preview, "Remove"};
                r.action = [this, save, m] {
                    RunWorkshopJob("removing " + m.name + "...", [save, m](std::string *) {
                        renut::content::RemoveMod(save, m.mod_id);
                        return true;
                    }, "removed " + m.name);
                };
                rows.push_back(std::move(r));
            }
        }
        DrawRows("##workshop", 12, 88, 527, 258, rows, empty);
        if (pending_) {
            auto action = std::move(pending_);
            pending_ = nullptr;
            action();
        }
        Label(status.empty() ? "SHARE VEHICLES AND PARTS FROM NUTSTALLER ON A PC" : status, 19, 350, 513, 24, 15,
              true);
    }

    // --- saves ---------------------------------------------------------------------

    void RefreshSaves() {
        saves_loaded_ = true;
        save_summary_ = renut::content::Summarize(save_dir_);
        backups_ = renut::content::Backups();
        imports_ = renut::content::ImportCandidates();
        renaming_ = -1;
    }

    static std::string Kb(uint64_t bytes) { return std::to_string(std::max<uint64_t>(1, bytes / 1024)) + " KB"; }

    void DoReplace(const std::filesystem::path &source, const std::string &note, const std::string &done) {
        std::filesystem::path kept;
        std::string error;
        if (renut::content::Replace(save_dir_, source, note, &kept, &error))
            saves_status_ = kept.empty() ? done : done + " - THE SAVE BEFORE IT IS IN " + Upper(kept.filename().string());
        else
            saves_status_ = "ERROR: " + Upper(error);
        RefreshSaves();
    }

    void DrawSaves() {
        Label("SAVES", 18, 16, 300, 30, 26);
        if (!renut::android::HasStorageAccess()) {
            Label("BACKUPS GO TO /SDCARD/RENUT/BACKUPS: RENUT NEEDS FILE ACCESS", 19, 56, 513, 24, 16, true);
            if (Button("Allow file access", 19, 88, 240, 32))
                renut::android::RequestStorageAccess();
            return;
        }
        if (!saves_loaded_)
            RefreshSaves();
        const auto &s = save_summary_;
        Label(s.bytes == 0 ? "NO SAVE YET - PLAY THE GAME ONCE, OR IMPORT ONE"
                           : std::to_string(s.slots) + " SAVE SLOT(S) - " + std::to_string(s.vehicles) +
                                 " VEHICLES - " + std::to_string(s.replays) + " REPLAYS - " + Kb(s.bytes),
              19, 48, 513, 24, 16);
        if (Button("Back up now", 19, 80, 180, 30, 18)) {
            std::string error;
            const auto file = renut::content::BackUp(save_dir_, "", &error);
            saves_status_ = file.empty() ? "ERROR: " + Upper(error) : "BACKED UP TO " + Upper(file.filename().string());
            RefreshSaves();
        }
        if (Button(saves_import_ ? "Show backups" : "Import a save...", 205, 80, 200, 30, 18, saves_import_)) {
            saves_import_ = !saves_import_;
            RefreshSaves();
        }
        if (Button("Rescan", 411, 80, 124, 30, 18))
            RefreshSaves();

        ImGui::SetCursorScreenPos(P(panel_x_ + 12, panel_y_ + 120));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("##saves", S(527, 222), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f))
            ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float row_h = 38;
        std::function<void()> after;  // list changes wait until the loop is done
        int n = 0;
        auto at = [&](float dx) { return ImVec2(top.x + dx * scale_, top.y + (n * row_h + 3) * scale_); };
        if (saves_import_) {
            if (imports_.empty())
                TextAt("PUT A SAVE FOLDER (4D5307ED, OR ONE WITH IT INSIDE) IN /SDCARD/RENUT", at(0), 520 * scale_,
                       30 * scale_, 14, kInkDim);
            for (const auto &dir : imports_) {
                ImGui::PushID(n);
                TextAt(Upper(dir.filename().string()), at(4), 380 * scale_, 30 * scale_, 17);
                if (SlabButton("Import", at(400), S(110, 30), false, 17, "import"))
                    after = [this, dir] {
                        Ask("Replace your save with /sdcard/reNut/" + dir.filename().string() +
                                "?\n\nYour current save is backed up first.",
                            [this, dir] { DoReplace(dir, "import", "IMPORTED"); });
                    };
                ImGui::PopID();
                ++n;
            }
        } else {
            if (backups_.empty())
                TextAt("NO BACKUPS YET - THEY GO TO /SDCARD/RENUT/BACKUPS", at(0), 520 * scale_, 30 * scale_, 14,
                       kInkDim);
            for (int i = 0; i < int(backups_.size()); ++i) {
                const auto b = backups_[i];
                const std::string when = b.made.empty() ? b.file.filename().string() : b.made;
                ImGui::PushID(i);
                if (renaming_ == i) {
                    ImGui::SetCursorScreenPos(at(0));
                    ImGui::PushFont(Font(), 16 * scale_);
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(kNavy));
                    ImGui::SetNextItemWidth(226 * scale_);
                    if (rename_focus_) {
                        ImGui::SetKeyboardFocusHere();
                        rename_focus_ = false;
                    }
                    const bool enter = ImGui::InputText("##name", rename_buf_.data(), kBufSize,
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
                    FocusRing(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                    ImGui::PopStyleColor();
                    ImGui::PopFont();
                    if (SlabButton("OK", at(236), S(84, 30), false, 16, "ok") || enter)
                        after = [this, b] {
                            std::string error;
                            const auto to = renut::content::RenameBackup(b.file, rename_buf_.c_str(), &error);
                            saves_status_ = to.empty() ? "ERROR: " + Upper(error)
                                                       : "RENAMED TO " + Upper(to.filename().string());
                            RefreshSaves();
                        };
                    if (SlabButton("Cancel", at(326), S(88, 30), false, 16, "cancel"))
                        after = [this] { renaming_ = -1; };
                } else {
                    TextAt(b.note.empty() || b.made.empty() ? (b.made.empty() ? Upper(b.note) : when + " - " + Kb(b.bytes))
                                                            : Upper(b.note) + " (" + when.substr(5, 11) + ")",
                           at(4), 226 * scale_, 30 * scale_, 15);
                    if (SlabButton("Rename", at(236), S(84, 30), false, 16, "rename"))
                        after = [this, i, b] {
                            renaming_ = i;
                            rename_focus_ = true;
                            SetBuf(rename_buf_, b.made.empty() ? b.file.stem().string() : b.note);
                        };
                    if (SlabButton("Restore", at(326), S(88, 30), false, 16, "restore"))
                        after = [this, b, when] {
                            Ask("Replace your save with the backup from " + when +
                                    "?\n\nYour current save is backed up first.",
                                [this, b] { DoReplace(b.file, "restore", "RESTORED"); });
                        };
                }
                if (SlabButton("Delete", at(420), S(76, 30), false, 16, "delete"))
                    after = [this, b, when] {
                        Ask("Delete the backup from " + when + "? This cannot be undone.", [this, b] {
                            std::error_code ec;
                            std::filesystem::remove(b.file, ec);
                            saves_status_ = ec ? "ERROR: " + Upper(ec.message()) : "BACKUP DELETED";
                            RefreshSaves();
                        });
                    };
                ImGui::PopID();
                ++n;
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + n * row_h * scale_));
        ImGui::Dummy(ImVec2(1, 1));
        ImGui::EndChild();
        ImGui::PopStyleColor();
        if (after)
            after();
        Label(saves_status_.empty() ? Upper(save_dir_.string()) : saves_status_, 19, 350, 513, 24, 14, true);
    }

    // --- in game: the side menu on the back gesture ------------------------------

    void SetMenuOpen(bool open) {
        menu_open_ = open;
        menu_page_ = 0;
        renut::touch::SetBlocked(open);
        // The game holds still under the menu (the last frame stays up).
        renut::gpu::Video::SetPaused(open);
        if (!open && text_input_) {
            text_input_ = false;
            if (SDL_Window *window = renut::gpu::GetGameSdlWindow())
                SDL_StopTextInput(window);
        }
    }

    void DrawInGame(bool back) {
        if (back) {
            if (renut::touch::Editing())
                renut::touch::BeginEdit();  // stays in edit mode: Done leaves it
            else
                SetMenuOpen(!menu_open_);
        }
        if (!menu_open_)
            return;
        LoadAssets();
        if (FeedGamepad().b && menu_page_ == 0) {
            SetMenuOpen(false);
            return;
        }
        ImGuiIO &io = ImGui::GetIO();
        scale_ = std::min(io.DisplaySize.x / kW, io.DisplaySize.y / kH);
        origin_ = ImVec2(0, (io.DisplaySize.y - kH * scale_) * 0.5f);
        panel_x_ = 0;
        panel_y_ = 0;
        settings_h_ = 590.0f;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
        ImGui::Begin("reNut side menu", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse |
                         ImGuiWindowFlags_NoBackground);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        // Dim the game; a tap beside the menu closes it.
        dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 20, 110));
        ImGui::SetCursorScreenPos(P(560, 0));
        if (ImGui::InvisibleButton("##outside", ImVec2(io.DisplaySize.x, io.DisplaySize.y)))
            SetMenuOpen(false);
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(P(560, 0).x, io.DisplaySize.y), IM_COL32(14, 28, 80, 235));
        Dashed(558.5f, -2, 0, kH + 4, kLineDim, 3);

        if (menu_page_ == 1) {
            DrawSettings();
            if (Button("< Back", 400, 14, 130, 32, 20))
                menu_page_ = 0;
        } else {
            Label("RENUT", 24, 18, 300, 40, 32);
            Label("PAUSED", 24, 56, 300, 24, 16, true);
            float y = 110;
            if (Button("Resume", 24, y, 300, 36, 22)) SetMenuOpen(false);
            y += 46;
            if (Button("Settings", 24, y, 300, 36, 22)) menu_page_ = 1;
            y += 46;
            if (Button("Touch layout", 24, y, 300, 36, 22)) {
                SetMenuOpen(false);
                renut::touch::BeginEdit();
            }
            y += 46;
            const bool touch = renut::touch::Enabled();
            if (Button(touch ? "Touch controls: on" : "Touch controls: off", 24, y, 300, 36, 22))
                renut::touch::SetEnabled(!touch);
            y += 46;
            if (Button("Exit to menu", 24, y + 20, 300, 36, 22)) {
                SaveSettings();
                renut::android::RestartApp();
            }
            if (renut::touch::GamepadConnected())
                Label("GAMEPAD CONNECTED: TOUCH CONTROLS HIDDEN", 24, 560, 520, 22, 15, true);
            Label("MAPPINGS: A B X Y RIGHT THUMB, STICK LEFT THUMB, DRAG RIGHT HALF = CAMERA", 24, 590,
                  520, 22, 13, true);
            Label("BACK GESTURE: OPEN / CLOSE THIS MENU", 24, 616, 520, 22, 15, true);
        }

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    // --- play ------------------------------------------------------------------

    void Play() {
        if (Extracting())
            return;
        const std::filesystem::path game = game_buf_.c_str();
        std::error_code ec;
        if (!std::filesystem::exists(game / "default.xex", ec)) {
            page_ = Page::Game;
            error_ = renut::android::HasStorageAccess() ? "NO DEFAULT.XEX THERE: PICK THE GAME FOLDER"
                                                        : "ALLOW FILE ACCESS FIRST";
            return;
        }
        rex::PathConfig cfg = defaults_;
        cfg.game_data_root = game;
        PathConfigStore::SavedPaths saved;
        saved.game_data_root = cfg.game_data_root;
        saved.user_data_root = cfg.user_data_root;
        saved.update_data_root = cfg.update_data_root;
        PathConfigStore::Save(app_name_, saved);
        visible_ = false;
        started_ = true;
        renut::touch::SetStarted(true);
        if (text_input_) {
            text_input_ = false;
            if (SDL_Window *window = renut::gpu::GetGameSdlWindow())
                SDL_StopTextInput(window);
        }
        on_play_(std::move(cfg));
    }

    std::string app_name_;
    rex::PathConfig defaults_;
    CompletionFn on_play_;
    bool visible_ = false;
    bool assets_loaded_ = false;
    bool text_input_ = false;
    ImFont *font_ = nullptr;
    ImTextureData *bg_ = nullptr;
    float scale_ = 1.0f;
    ImVec2 origin_;
    Page page_ = Page::Game;
    std::string game_buf_;
    std::string error_;
    std::string selected_driver_;
    std::string driver_message_;
    std::string settings_message_;
    std::vector<renut::android::Driver> drivers_;
    std::vector<std::filesystem::path> zips_;
    std::vector<std::filesystem::path> isos_;
    float panel_x_ = 634.0f, panel_y_ = 281.0f, settings_h_ = 316.0f;
    bool started_ = false;
    bool menu_open_ = false;
    int menu_page_ = 0;  // 0 main, 1 settings
    renut::xiso::Progress extract_;
    std::thread extract_thread_;
    std::string extract_message_;  // written by the worker under extract_.mutex

    bool pad_down_[9] = {};
    std::optional<Confirm> confirm_;
    std::filesystem::path save_dir_;

    // workshop: the worker writes these under mutex_
    std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> busy_{false};
    std::optional<renut::content::Index> index_;
    std::vector<renut::content::Installed> installed_;
    std::vector<renut::content::InstalledMod> installed_mods_;
    std::map<std::string, Preview> previews_;
    std::string status_;
    bool workshop_loaded_ = false;
    int workshop_tab_ = 0;
    std::function<void()> pending_;

    // saves
    bool saves_loaded_ = false;
    bool saves_import_ = false;
    renut::content::SaveSummary save_summary_;
    std::vector<renut::content::Backup> backups_;
    std::vector<std::filesystem::path> imports_;
    std::string saves_status_;
    int renaming_ = -1;
    bool rename_focus_ = false;
    std::string rename_buf_;
};
