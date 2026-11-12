/**
 * @file        reporter/ui_imgui_d3d11.cpp
 * @brief       The crash reporter window on Windows: ImGui over Win32 and
 *              Direct3D 11
 *
 * @remarks     This program runs because something already went badly wrong,
 *              quite possibly in the graphics driver, so it must degrade rather
 *              than fail. Direct3D 11 is tried on the GPU first and then on
 *              WARP, Windows' own software rasteriser, which works with no
 *              usable GPU at all. When neither starts, RunWin32Ui's plain
 *              controls take over. Vulkan has no software fallback of its own,
 *              and it is the driver stack the game was using when it died.
 */

#include "util.h"

#include <string>
#include <vector>

#include <d3d11.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "reporter.h"
#include "ui_imgui.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace reporter {

namespace {

struct Direct3D {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  IDXGISwapChain* swap_chain = nullptr;
  ID3D11RenderTargetView* target = nullptr;

  bool Create(HWND window) {
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    // The GPU, then WARP when the GPU or its driver won't start.
    for (const D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
      D3D_FEATURE_LEVEL level;
      const HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc, &swap_chain, &device, &level, &context);
      if (SUCCEEDED(result)) {
        CreateTarget();
        return true;
      }
      g_session.window_error = "D3D11CreateDeviceAndSwapChain failed: " + std::to_string(static_cast<unsigned long>(result));
    }
    return false;
  }

  void CreateTarget() {
    ID3D11Texture2D* back_buffer = nullptr;
    swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    device->CreateRenderTargetView(back_buffer, nullptr, &target);
    back_buffer->Release();
  }

  void Resize(UINT width, UINT height) {
    if (!swap_chain || width == 0 || height == 0) return;
    if (target) target->Release();
    target = nullptr;
    swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    CreateTarget();
  }

  void Destroy() {
    if (target) target->Release();
    if (swap_chain) swap_chain->Release();
    if (context) context->Release();
    if (device) device->Release();
  }
};

Direct3D g_d3d;

LRESULT CALLBACK WindowProc(HWND window, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (ImGui_ImplWin32_WndProcHandler(window, msg, wparam, lparam)) return true;
  switch (msg) {
    case WM_SIZE:
      if (wparam != SIZE_MINIMIZED) g_d3d.Resize(LOWORD(lparam), HIWORD(lparam));
      return 0;
    case WM_CLOSE:
      g_quit = true;
      return 0;
    default:
      return DefWindowProcW(window, msg, wparam, lparam);
  }
}

}  // namespace

bool RunImGuiUi() {
  ImGui_ImplWin32_EnableDpiAwareness();
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW window_class = {sizeof(window_class)};
  window_class.style = CS_CLASSDC;
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = L"rexcrashreporter";
  RegisterClassExW(&window_class);

  const std::string title = WindowTitle();
  const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
  HWND window = CreateWindowExW(0, window_class.lpszClassName, Widen(title).c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(900 * scale), static_cast<int>(700 * scale), nullptr, nullptr, instance, nullptr);
  if (!window || !g_d3d.Create(window)) {
    if (!window) g_session.window_error = "CreateWindowExW failed";
    g_d3d.Destroy();
    if (window) DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, instance);
    return false;
  }
  ShowWindow(window, SW_SHOWDEFAULT);
  UpdateWindow(window);
  SetForegroundWindow(window);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;  // Never leave settings files behind.
  ApplyStyle();
  ImGui::GetStyle().ScaleAllSizes(scale);
  ImGui::GetStyle().FontScaleDpi = scale;
  ImGui_ImplWin32_Init(window);
  ImGui_ImplDX11_Init(g_d3d.device, g_d3d.context);

  while (!g_quit) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) g_quit = true;
    }
    if (g_quit) break;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawWindow();
    ImGui::Render();

    const float clear[4] = {0.09f, 0.09f, 0.11f, 1.0f};
    g_d3d.context->OMSetRenderTargets(1, &g_d3d.target, nullptr);
    g_d3d.context->ClearRenderTargetView(g_d3d.target, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_d3d.swap_chain->Present(1, 0);
  }

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  g_d3d.Destroy();
  DestroyWindow(window);
  UnregisterClassW(window_class.lpszClassName, instance);

  FinishWindow();
  return true;
}

}  // namespace reporter
