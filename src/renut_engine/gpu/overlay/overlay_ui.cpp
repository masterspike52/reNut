/**
 * @file    gpu/overlay/overlay_ui.cpp
 */
#include "renut_engine/gpu/overlay/overlay_ui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/native_window.h"
#include "renut_engine/gpu/backend/video.h"

#include "host_shaders/overlay_ps.spirv.h"
#include "host_shaders/overlay_vs.spirv.h"
#if defined(_WIN32)
#include "host_shaders/overlay_ps.dxil.h"
#include "host_shaders/overlay_vs.dxil.h"

#include <windows.h>
#include <windowsx.h>
#else
#include <string>

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>
#endif

namespace renut::gpu::overlay {

namespace {

std::mutex g_dialogs_mutex;
std::vector<Dialog *> g_dialogs;

ImGuiContext *g_context = nullptr;

// Makes the overlay's context current for a scope and puts the previous one
// back. The overlay's ImGui copy has only this context; the scope keeps it
// correct should anything else in the executable make another.
class ContextScope {
public:
  ContextScope() : previous_(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(g_context); }
  ~ContextScope() { ImGui::SetCurrentContext(previous_); }

private:
  ImGuiContext *previous_;
};

// A built frame, in display pixels.
struct Command {
  ImVec4 clip;
  ImTextureID texture = ImTextureID_Invalid;
  uint32_t index_offset = 0;
  uint32_t index_count = 0;
  uint32_t vertex_offset = 0;
};
struct Frame {
  float width = 0.0f;
  float height = 0.0f;
  std::vector<ImDrawVert> vertices;
  std::vector<ImDrawIdx> indices;
  std::vector<Command> commands;
};
// What ImGui asked of its textures (font atlas pages) since the renderer last
// looked: the whole texture again for a creation or an update.
struct TextureRequest {
  ImTextureID id = ImTextureID_Invalid;
  bool destroy = false;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;
};

// The latest frame and the texture requests not yet applied; both the window
// thread (building) and the presenting thread (drawing) take it.
std::mutex g_frame_mutex;
Frame g_frame;
std::vector<TextureRequest> g_texture_requests;
ImTextureID g_next_texture = 1;
std::chrono::steady_clock::time_point g_last_build;

// Honours the build's texture requests (window thread, g_frame_mutex held):
// the pixels are copied for the renderer, so ImGui may consider them done.
void TakeTextureRequests(ImDrawData *data) {
  if (!data->Textures)
    return;
  for (ImTextureData *texture : *data->Textures) {
    switch (texture->Status) {
    case ImTextureStatus_WantCreate:
    case ImTextureStatus_WantUpdates: {
      if (texture->Status == ImTextureStatus_WantCreate)
        texture->SetTexID(g_next_texture++);
      TextureRequest request;
      request.id = texture->GetTexID();
      request.width = uint32_t(texture->Width);
      request.height = uint32_t(texture->Height);
      const size_t texels = size_t(request.width) * request.height;
      request.rgba.resize(texels * 4);
      const auto *pixels = static_cast<const uint8_t *>(texture->GetPixels());
      if (texture->Format == ImTextureFormat_RGBA32) {
        std::memcpy(request.rgba.data(), pixels, texels * 4);
      } else {  // Alpha8: white with the coverage as alpha
        for (size_t i = 0; i < texels; ++i) {
          request.rgba[i * 4 + 0] = request.rgba[i * 4 + 1] = request.rgba[i * 4 + 2] = 0xFF;
          request.rgba[i * 4 + 3] = pixels[i];
        }
      }
      g_texture_requests.push_back(std::move(request));
      texture->SetStatus(ImTextureStatus_OK);
      break;
    }
    case ImTextureStatus_WantDestroy: {
      TextureRequest request;
      request.id = texture->GetTexID();
      request.destroy = true;
      g_texture_requests.push_back(std::move(request));
      texture->SetTexID(ImTextureID_Invalid);
      texture->SetStatus(ImTextureStatus_Destroyed);
      break;
    }
    default:
      break;
    }
  }
}

// The window input queued since the last build, into the overlay's ImGui.
void ApplyQueuedInput(ImGuiIO &io);

// Whether the last built frame showed a window that takes input (WantsCursor).
std::atomic<bool> g_wants_cursor{false};

// A shown top-level window that takes input; display-only ones (the FPS
// overlay, NoInputs) and ImGui's implicit fallback window don't count.
bool AnyInputWindowShown() {
  for (const ImGuiWindow *window : ImGui::GetCurrentContext()->Windows) {
    if (!window->WasActive || window->Hidden ||
        (window->Flags & (ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_ChildWindow |
                          ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup)) ||
        std::strncmp(window->Name, "Debug##Default", 14) == 0) {
      continue;
    }
    return true;
  }
  return false;
}

// One overlay frame: the dialogs' widgets, turned into the frame the renderer
// draws. On the UI thread.
void BuildFrame(float width, float height) {
  if (!g_context)
    return;
  ContextScope scope;
  ImGuiIO &io = ImGui::GetIO();
  const auto now = std::chrono::steady_clock::now();
  io.DisplaySize = ImVec2(std::max(width, 1.0f), std::max(height, 1.0f));
  io.DeltaTime = std::max(std::chrono::duration<float>(now - g_last_build).count(), 1.0e-4f);
  g_last_build = now;
  ApplyQueuedInput(io);

  ImGui::NewFrame();
  {
    std::lock_guard lock(g_dialogs_mutex);
    for (Dialog *dialog : g_dialogs)
      dialog->Draw();
  }
  ImGui::Render();
  ImDrawData *data = ImGui::GetDrawData();
  g_wants_cursor.store(AnyInputWindowShown(), std::memory_order_relaxed);

  std::lock_guard lock(g_frame_mutex);
  // Texture ids first: the commands name their textures by them.
  TakeTextureRequests(data);
  Frame frame;
  frame.width = io.DisplaySize.x;
  frame.height = io.DisplaySize.y;
  for (int n = 0; n < data->CmdListsCount; ++n) {
    const ImDrawList *list = data->CmdLists[n];
    const uint32_t vertex_base = uint32_t(frame.vertices.size());
    const uint32_t index_base = uint32_t(frame.indices.size());
    frame.vertices.insert(frame.vertices.end(), list->VtxBuffer.begin(), list->VtxBuffer.end());
    frame.indices.insert(frame.indices.end(), list->IdxBuffer.begin(), list->IdxBuffer.end());
    for (const ImDrawCmd &draw : list->CmdBuffer) {
      if (draw.UserCallback || !draw.ElemCount)
        continue;
      Command command;
      command.clip = ImVec4(draw.ClipRect.x - data->DisplayPos.x, draw.ClipRect.y - data->DisplayPos.y,
                            draw.ClipRect.z - data->DisplayPos.x, draw.ClipRect.w - data->DisplayPos.y);
      command.texture = draw.GetTexID();
      command.index_offset = index_base + draw.IdxOffset;
      command.index_count = draw.ElemCount;
      command.vertex_offset = vertex_base + draw.VtxOffset;
      frame.commands.push_back(command);
    }
  }
  g_frame = std::move(frame);
}

// --- Window input and the build pump (Windows) ------------------------------

#if defined(_WIN32)

HWND g_window = nullptr;
WNDPROC g_original_proc = nullptr;
UIThreadRunner g_run_on_ui;
std::atomic<bool> g_build_pending{false};
std::atomic<bool> g_stop{true};
std::thread g_pump;

// A window message the overlay's ImGui takes, as it arrived, with the modifier
// keys as they were then. The window procedure queues them rather than touch
// ImGui: the window's thread need not be the UI thread.
struct InputMessage {
  UINT msg;
  WPARAM wparam;
  LPARAM lparam;
  bool ctrl, shift, alt;
};
// Enough for seconds of mouse movement; past it, while no build runs, input
// is dropped.
constexpr size_t kMaxQueuedInput = 4096;
std::mutex g_input_mutex;
std::vector<InputMessage> g_input;

ImGuiKey KeyFromVirtualKey(WPARAM vk) {
  if (vk >= 'A' && vk <= 'Z')
    return ImGuiKey(ImGuiKey_A + (vk - 'A'));
  if (vk >= '0' && vk <= '9')
    return ImGuiKey(ImGuiKey_0 + (vk - '0'));
  if (vk >= VK_F1 && vk <= VK_F12)
    return ImGuiKey(ImGuiKey_F1 + (vk - VK_F1));
  switch (vk) {
  case VK_TAB: return ImGuiKey_Tab;
  case VK_LEFT: return ImGuiKey_LeftArrow;
  case VK_RIGHT: return ImGuiKey_RightArrow;
  case VK_UP: return ImGuiKey_UpArrow;
  case VK_DOWN: return ImGuiKey_DownArrow;
  case VK_PRIOR: return ImGuiKey_PageUp;
  case VK_NEXT: return ImGuiKey_PageDown;
  case VK_HOME: return ImGuiKey_Home;
  case VK_END: return ImGuiKey_End;
  case VK_INSERT: return ImGuiKey_Insert;
  case VK_DELETE: return ImGuiKey_Delete;
  case VK_BACK: return ImGuiKey_Backspace;
  case VK_SPACE: return ImGuiKey_Space;
  case VK_RETURN: return ImGuiKey_Enter;
  case VK_ESCAPE: return ImGuiKey_Escape;
  default: return ImGuiKey_None;
  }
}

bool IsInputMessage(UINT msg) {
  switch (msg) {
  case WM_MOUSEMOVE:
  case WM_LBUTTONDOWN:
  case WM_LBUTTONUP:
  case WM_RBUTTONDOWN:
  case WM_RBUTTONUP:
  case WM_MBUTTONDOWN:
  case WM_MBUTTONUP:
  case WM_MOUSEWHEEL:
  case WM_MOUSEHWHEEL:
  case WM_KEYDOWN:
  case WM_KEYUP:
  case WM_SYSKEYDOWN:
  case WM_SYSKEYUP:
  case WM_CHAR:
  case WM_SETFOCUS:
  case WM_KILLFOCUS:
    return true;
  default:
    return false;
  }
}

void ApplyInput(ImGuiIO &io, const InputMessage &m) {
  switch (m.msg) {
  case WM_MOUSEMOVE:
    io.AddMousePosEvent(float(GET_X_LPARAM(m.lparam)), float(GET_Y_LPARAM(m.lparam)));
    break;
  case WM_LBUTTONDOWN:
  case WM_LBUTTONUP:
    io.AddMouseButtonEvent(0, m.msg == WM_LBUTTONDOWN);
    break;
  case WM_RBUTTONDOWN:
  case WM_RBUTTONUP:
    io.AddMouseButtonEvent(1, m.msg == WM_RBUTTONDOWN);
    break;
  case WM_MBUTTONDOWN:
  case WM_MBUTTONUP:
    io.AddMouseButtonEvent(2, m.msg == WM_MBUTTONDOWN);
    break;
  case WM_MOUSEWHEEL:
    io.AddMouseWheelEvent(0.0f, float(GET_WHEEL_DELTA_WPARAM(m.wparam)) / float(WHEEL_DELTA));
    break;
  case WM_MOUSEHWHEEL:
    io.AddMouseWheelEvent(-float(GET_WHEEL_DELTA_WPARAM(m.wparam)) / float(WHEEL_DELTA), 0.0f);
    break;
  case WM_KEYDOWN:
  case WM_KEYUP:
  case WM_SYSKEYDOWN:
  case WM_SYSKEYUP: {
    const bool down = m.msg == WM_KEYDOWN || m.msg == WM_SYSKEYDOWN;
    io.AddKeyEvent(ImGuiMod_Ctrl, m.ctrl);
    io.AddKeyEvent(ImGuiMod_Shift, m.shift);
    io.AddKeyEvent(ImGuiMod_Alt, m.alt);
    const ImGuiKey key = KeyFromVirtualKey(m.wparam);
    if (key != ImGuiKey_None)
      io.AddKeyEvent(key, down);
    break;
  }
  case WM_CHAR:
    if (m.wparam > 0 && m.wparam < 0x10000)
      io.AddInputCharacterUTF16(static_cast<unsigned short>(m.wparam));
    break;
  case WM_SETFOCUS:
  case WM_KILLFOCUS:
    io.AddFocusEvent(m.msg == WM_SETFOCUS);
    break;
  default:
    break;
  }
}

// Sees the game window's messages first: the overlay's input is queued for
// the next build, then everything goes on to the SDK.
LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (IsInputMessage(msg)) {
    const InputMessage m{msg, wparam, lparam, GetKeyState(VK_CONTROL) < 0,
                         GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_MENU) < 0};
    std::lock_guard lock(g_input_mutex);
    if (g_input.size() < kMaxQueuedInput)
      g_input.push_back(m);
  }
  return CallWindowProcW(g_original_proc, hwnd, msg, wparam, lparam);
}

void ApplyQueuedInput(ImGuiIO &io) {
  std::vector<InputMessage> input;
  {
    std::lock_guard lock(g_input_mutex);
    input.swap(g_input);
  }
  for (const InputMessage &m : input)
    ApplyInput(io, m);
}

// The pump's call, on the UI thread.
void BuildOnUIThread() {
  RECT rect{};
  GetClientRect(g_window, &rect);
  BuildFrame(float(rect.right - rect.left), float(rect.bottom - rect.top));
  g_build_pending.store(false, std::memory_order_release);
}

#else

// --- Window input and the build pump (SDL: Linux) ----------------------------
//
// The SDK's window is an SDL window; an event watch sees its events as SDL
// queues them, before the SDK handles them, as the window procedure does on
// Windows. Without the overlay nothing of reNut's (the first-run path setup
// included) could be seen or used.

SDL_Window *g_sdl_window = nullptr;
UIThreadRunner g_run_on_ui;
std::atomic<bool> g_build_pending{false};
std::atomic<bool> g_stop{true};
std::thread g_pump;

// An event the overlay's ImGui takes, as it arrived. A text event's string is
// SDL's for the moment only: it is copied.
struct InputEvent {
  SDL_Event event;
  std::string text;
};
constexpr size_t kMaxQueuedInput = 4096;
std::mutex g_input_mutex;
std::vector<InputEvent> g_input;

ImGuiKey KeyFromKeycode(SDL_Keycode key) {
  if (key >= SDLK_A && key <= SDLK_Z)
    return ImGuiKey(ImGuiKey_A + (key - SDLK_A));
  if (key >= SDLK_0 && key <= SDLK_9)
    return ImGuiKey(ImGuiKey_0 + (key - SDLK_0));
  if (key >= SDLK_F1 && key <= SDLK_F12)
    return ImGuiKey(ImGuiKey_F1 + (key - SDLK_F1));
  switch (key) {
  case SDLK_TAB: return ImGuiKey_Tab;
  case SDLK_LEFT: return ImGuiKey_LeftArrow;
  case SDLK_RIGHT: return ImGuiKey_RightArrow;
  case SDLK_UP: return ImGuiKey_UpArrow;
  case SDLK_DOWN: return ImGuiKey_DownArrow;
  case SDLK_PAGEUP: return ImGuiKey_PageUp;
  case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
  case SDLK_HOME: return ImGuiKey_Home;
  case SDLK_END: return ImGuiKey_End;
  case SDLK_INSERT: return ImGuiKey_Insert;
  case SDLK_DELETE: return ImGuiKey_Delete;
  case SDLK_BACKSPACE: return ImGuiKey_Backspace;
  case SDLK_SPACE: return ImGuiKey_Space;
  case SDLK_RETURN: return ImGuiKey_Enter;
  case SDLK_ESCAPE: return ImGuiKey_Escape;
  default: return ImGuiKey_None;
  }
}

bool IsInputEvent(Uint32 type) {
  switch (type) {
  case SDL_EVENT_MOUSE_MOTION:
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
  case SDL_EVENT_MOUSE_BUTTON_UP:
  case SDL_EVENT_MOUSE_WHEEL:
  case SDL_EVENT_KEY_DOWN:
  case SDL_EVENT_KEY_UP:
  case SDL_EVENT_TEXT_INPUT:
  case SDL_EVENT_WINDOW_FOCUS_GAINED:
  case SDL_EVENT_WINDOW_FOCUS_LOST:
    return true;
  default:
    return false;
  }
}

// Mouse positions are in window coordinates; the overlay is in pixels.
void ApplyInput(ImGuiIO &io, const InputEvent &input, float pixel_density) {
  const SDL_Event &e = input.event;
  switch (e.type) {
  case SDL_EVENT_MOUSE_MOTION:
    io.AddMousePosEvent(e.motion.x * pixel_density, e.motion.y * pixel_density);
    break;
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
  case SDL_EVENT_MOUSE_BUTTON_UP: {
    const int button = e.button.button == SDL_BUTTON_LEFT    ? 0
                       : e.button.button == SDL_BUTTON_RIGHT ? 1
                       : e.button.button == SDL_BUTTON_MIDDLE ? 2
                                                              : -1;
    io.AddMousePosEvent(e.button.x * pixel_density, e.button.y * pixel_density);
    if (button >= 0)
      io.AddMouseButtonEvent(button, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
    break;
  }
  case SDL_EVENT_MOUSE_WHEEL:
    io.AddMouseWheelEvent(-e.wheel.x, e.wheel.y);
    break;
  case SDL_EVENT_KEY_DOWN:
  case SDL_EVENT_KEY_UP: {
    io.AddKeyEvent(ImGuiMod_Ctrl, (e.key.mod & SDL_KMOD_CTRL) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (e.key.mod & SDL_KMOD_SHIFT) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (e.key.mod & SDL_KMOD_ALT) != 0);
    const ImGuiKey key = KeyFromKeycode(e.key.key);
    if (key != ImGuiKey_None)
      io.AddKeyEvent(key, e.type == SDL_EVENT_KEY_DOWN);
    break;
  }
  case SDL_EVENT_TEXT_INPUT:
    io.AddInputCharactersUTF8(input.text.c_str());
    break;
  case SDL_EVENT_WINDOW_FOCUS_GAINED:
  case SDL_EVENT_WINDOW_FOCUS_LOST:
    io.AddFocusEvent(e.type == SDL_EVENT_WINDOW_FOCUS_GAINED);
    break;
  default:
    break;
  }
}

// Sees the events first: the overlay's input is queued for the next build,
// and every event goes on to the SDK.
bool SDLCALL OverlayEventWatch(void *, SDL_Event *event) {
  if (event && IsInputEvent(event->type)) {
    InputEvent input{*event, {}};
    if (event->type == SDL_EVENT_TEXT_INPUT && event->text.text)
      input.text = event->text.text;
    std::lock_guard lock(g_input_mutex);
    if (g_input.size() < kMaxQueuedInput)
      g_input.push_back(std::move(input));
  }
  return true;
}

void ApplyQueuedInput(ImGuiIO &io) {
  std::vector<InputEvent> input;
  {
    std::lock_guard lock(g_input_mutex);
    input.swap(g_input);
  }
  const float density = g_sdl_window ? SDL_GetWindowPixelDensity(g_sdl_window) : 1.0f;
  for (const InputEvent &event : input)
    ApplyInput(io, event, density > 0.0f ? density : 1.0f);
}

// The pump's call, on the UI thread.
void BuildOnUIThread() {
  uint32_t width = 0, height = 0;
  GetRenderWindowPixelSize(width, height);
  BuildFrame(float(width), float(height));
  g_build_pending.store(false, std::memory_order_release);
}

#endif

// --- Rendering (the presenting thread, State().mutex held) ------------------

struct OverlayTexture {
  std::unique_ptr<plume::RenderTexture> texture;
  std::unique_ptr<plume::RenderTextureView> view;
  uint32_t slot = kInvalidDescriptor;
};
std::unordered_map<ImTextureID, OverlayTexture> g_textures;
std::unique_ptr<plume::RenderShader> g_vs;
std::unique_ptr<plume::RenderShader> g_ps;
std::unique_ptr<plume::RenderPipeline> g_pipeline;
bool g_pipeline_failed = false;

const plume::RenderInputSlot kVertexSlot(0, sizeof(ImDrawVert),
                                         plume::RenderInputSlotClassification::PER_VERTEX_DATA);

void ReleaseTexture(OverlayTexture &t) {
  // Frames in flight may still sample it: retire rather than free.
  if (t.slot != kInvalidDescriptor)
    Video::FreeTextureSlotLocked(t.slot);
  Video::RetireLocked(std::move(t.view));
  Video::RetireLocked(std::move(t.texture));
  t.slot = kInvalidDescriptor;
}

void ApplyTextureRequests() {
  plume::RenderDevice *device = Video::Device();
  for (TextureRequest &request : g_texture_requests) {
    auto it = g_textures.find(request.id);
    if (it != g_textures.end()) {
      ReleaseTexture(it->second);
      g_textures.erase(it);
    }
    if (request.destroy || !request.width || !request.height)
      continue;
    OverlayTexture t;
    t.texture = device->createTexture(plume::RenderTextureDesc::Texture2D(
        request.width, request.height, 1, plume::RenderFormat::R8G8B8A8_UNORM));
    if (!t.texture)
      continue;
    t.texture->setName("renut overlay texture");
    t.view = t.texture->createTextureView(
        plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8G8B8A8_UNORM));
    t.slot = t.view ? Video::AllocateTextureSlotLocked() : kInvalidDescriptor;
    if (t.slot == kInvalidDescriptor ||
        !Video::UploadRGBA8Locked(t.texture.get(), request.width, request.height,
                                  request.rgba.data())) {
      ReleaseTexture(t);
      continue;
    }
    Video::SetTextureSlotLocked(t.slot, t.texture.get(), t.view.get());
    g_textures.emplace(request.id, std::move(t));
  }
  g_texture_requests.clear();
}

bool EnsurePipeline() {
  if (g_pipeline || g_pipeline_failed)
    return g_pipeline != nullptr;
  plume::RenderDevice *device = Video::Device();
  g_vs = CreateHostShader(device, RENUT_HOST_SHADER(overlay_vs), "overlay_vs");
  g_ps = CreateHostShader(device, RENUT_HOST_SHADER(overlay_ps), "overlay_ps");
  if (g_vs && g_ps) {
    const plume::RenderInputElement elements[] = {
        plume::RenderInputElement("POSITION", 0, 0, plume::RenderFormat::R32G32_FLOAT, 0,
                                  offsetof(ImDrawVert, pos)),
        plume::RenderInputElement("TEXCOORD", 0, 1, plume::RenderFormat::R32G32_FLOAT, 0,
                                  offsetof(ImDrawVert, uv)),
        plume::RenderInputElement("COLOR", 0, 2, plume::RenderFormat::R8G8B8A8_UNORM, 0,
                                  offsetof(ImDrawVert, col)),
    };
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = State().utility_layout.get();
    desc.vertexShader = g_vs.get();
    desc.pixelShader = g_ps.get();
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = plume::RenderFormat::B8G8R8A8_UNORM;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::AlphaBlend();
    desc.depthTargetFormat = plume::RenderFormat::UNKNOWN;
    desc.inputSlots = &kVertexSlot;
    desc.inputSlotsCount = 1;
    desc.inputElements = elements;
    desc.inputElementsCount = 3;
    g_pipeline = device->createGraphicsPipeline(desc);
  }
  if (!g_pipeline) {
    RGPU_ERROR("Overlay pipeline creation failed");
    g_pipeline_failed = true;
  }
  return g_pipeline != nullptr;
}

} // namespace

void AddDialog(Dialog *dialog) {
  std::lock_guard lock(g_dialogs_mutex);
  if (dialog && std::find(g_dialogs.begin(), g_dialogs.end(), dialog) == g_dialogs.end())
    g_dialogs.push_back(dialog);
}

void RemoveDialog(Dialog *dialog) {
  std::lock_guard lock(g_dialogs_mutex);
  std::erase(g_dialogs, dialog);
}

bool WantsCursor() {
  return g_wants_cursor.load(std::memory_order_relaxed);
}

namespace {

// The overlay's ImGui context. CreateContext leaves any context that was
// current current.
void CreateOverlayContext() {
  g_context = ImGui::CreateContext();
  ContextScope scope;
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.BackendPlatformName = "renut";
  io.BackendRendererName = "renut";
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
#if !defined(_WIN32)
  // ImGui has a clipboard of its own on Windows only; elsewhere it is SDL's
  // (the friend codes' Copy and Paste, and Ctrl+C / Ctrl+V in text fields).
  // Called while building, on the UI thread, SDL's video thread.
  ImGuiPlatformIO &platform = ImGui::GetPlatformIO();
  platform.Platform_SetClipboardTextFn = [](ImGuiContext *, const char *text) {
    SDL_SetClipboardText(text ? text : "");
  };
  platform.Platform_GetClipboardTextFn = [](ImGuiContext *) -> const char * {
    static std::string text;  // ImGui keeps the pointer until the next call
    char *clipboard = SDL_GetClipboardText();
    text = clipboard ? clipboard : "";
    SDL_free(clipboard);
    return text.c_str();
  };
#endif
  ImGui::StyleColorsDark();
}

// A frame every 16 ms, on the UI thread; one queued at a time.
void StartPump() {
  g_last_build = std::chrono::steady_clock::now();
  g_stop.store(false, std::memory_order_release);
  g_pump = std::thread([] {
    while (!g_stop.load(std::memory_order_acquire)) {
      if (!g_build_pending.exchange(true, std::memory_order_acq_rel))
        g_run_on_ui([] { BuildOnUIThread(); });
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
  });
}

void StopPump() {
  g_stop.store(true, std::memory_order_release);
  if (g_pump.joinable())
    g_pump.join();
}

} // namespace

bool Start(UIThreadRunner run_on_ui_thread) {
  if (g_context)
    return true;
#if defined(_WIN32)
  plume::RenderWindow window{};
  if (!run_on_ui_thread || !GetNativeRenderWindow(window))
    return false;
  g_window = static_cast<HWND>(window);
  g_run_on_ui = std::move(run_on_ui_thread);
  CreateOverlayContext();
  g_original_proc = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(OverlayWindowProc)));
#else
  g_sdl_window = GetGameSdlWindow();
  if (!run_on_ui_thread || !g_sdl_window)
    return false;
  g_run_on_ui = std::move(run_on_ui_thread);
  CreateOverlayContext();
  SDL_AddEventWatch(OverlayEventWatch, nullptr);
  // Text fields get typed characters only while SDL's text input is on; SDL
  // wants that switched on from its video thread, the UI thread.
#if !defined(__ANDROID__)  // there it brings up the on-screen keyboard
  g_run_on_ui([] { SDL_StartTextInput(g_sdl_window); });
#endif
#endif
  StartPump();
  return true;
}

void Stop() {
  StopPump();
#if defined(_WIN32)
  if (g_window && g_original_proc &&
      GetWindowLongPtrW(g_window, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(OverlayWindowProc)) {
    SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_proc));
  }
#else
  if (g_sdl_window)
    SDL_RemoveEventWatch(OverlayEventWatch, nullptr);
#endif
}

void DrawLocked(plume::RenderCommandList *cmd, uint32_t width, uint32_t height) {
  std::lock_guard lock(g_frame_mutex);
  if (!g_texture_requests.empty())
    ApplyTextureRequests();
  const Frame &frame = g_frame;
  if (frame.commands.empty() || frame.width <= 0.0f || frame.height <= 0.0f || !EnsurePipeline())
    return;

  UploadRing &uploads = Video::UploadsLocked();
  const UploadAllocation vertices =
      uploads.Upload(frame.vertices.data(), sizeof(ImDrawVert) * frame.vertices.size(), 16);
  const UploadAllocation indices =
      uploads.Upload(frame.indices.data(), sizeof(ImDrawIdx) * frame.indices.size(), 16);
  if (!vertices || !indices)
    return;

  cmd->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  cmd->setGraphicsPipelineLayout(State().utility_layout.get());
  cmd->setPipeline(g_pipeline.get());
  cmd->setGraphicsDescriptorSet(State().texture_set.get(), 0);
  cmd->setGraphicsDescriptorSet(State().sampler_set.get(), 1);
  const plume::RenderVertexBufferView vertex_view(vertices.ref, vertices.size);
  cmd->setVertexBuffers(0, &vertex_view, 1, &kVertexSlot);
  const plume::RenderIndexBufferView index_view(
      indices.ref, indices.size,
      sizeof(ImDrawIdx) == 2 ? plume::RenderFormat::R16_UINT : plume::RenderFormat::R32_UINT);
  cmd->setIndexBuffer(&index_view);

  // Display pixels to clip space (y down), and to back buffer pixels for the
  // clip rectangles when the two differ in size.
  UtilityPush push{};
  push.scale[0] = 2.0f / frame.width;
  push.scale[1] = -2.0f / frame.height;
  push.translate[0] = -1.0f;
  push.translate[1] = 1.0f;
  push.sampler_index = kLinearClampSamplerSlot;
  const float to_x = float(width) / frame.width;
  const float to_y = float(height) / frame.height;
  for (const Command &command : frame.commands) {
    auto it = g_textures.find(command.texture);
    if (it == g_textures.end())
      continue;
    const int32_t left = std::clamp(int32_t(command.clip.x * to_x), 0, int32_t(width));
    const int32_t top = std::clamp(int32_t(command.clip.y * to_y), 0, int32_t(height));
    const int32_t right = std::clamp(int32_t(command.clip.z * to_x + 0.5f), 0, int32_t(width));
    const int32_t bottom = std::clamp(int32_t(command.clip.w * to_y + 0.5f), 0, int32_t(height));
    if (right <= left || bottom <= top)
      continue;
    cmd->setScissors(plume::RenderRect(left, top, right, bottom));
    push.texture_index = it->second.slot;
    cmd->setGraphicsPushConstants(0, &push, 0, sizeof(push));
    cmd->drawIndexedInstanced(command.index_count, 1, command.index_offset,
                              int32_t(command.vertex_offset), 0);
  }
  cmd->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));
}

} // namespace renut::gpu::overlay
