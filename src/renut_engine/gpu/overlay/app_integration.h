/**
 * @file    gpu/overlay/app_integration.h
 * @brief   What RenutApp needs to run the native renderer in the SDK's detached
 *          ("bring your own renderer") mode.
 */
#pragma once

#include <functional>
#include <memory>

namespace rex {
struct RuntimeConfig;
namespace ui {
class ImGuiDrawer;
class ImmediateDrawer;
class WindowedAppContext;
} // namespace ui
} // namespace rex

namespace renut::gpu {

// OnPreSetup: no SDK graphics system and no GPU plugin.
void ConfigureRuntime(rex::RuntimeConfig &config);

// OnCreateImmediateDrawer: the plume-backed ImGui drawer.
std::unique_ptr<rex::ui::ImmediateDrawer> CreateOverlayDrawer();

// UI thread, once the window exists: creates the host device, starts reNut's
// overlay (overlay_ui.h), routes the SDK's menus into Present, and presents
// overlay-only frames whenever the guest is not presenting (before it starts,
// e.g. the path wizard, and while it stalls).
bool StartRenderer(rex::ui::WindowedAppContext &app_context,
                   std::function<rex::ui::ImGuiDrawer *()> imgui_drawer);

// OnWindowPixelSizeChanged.
void OnWindowResized();

// OnShutdown.
void ShutdownRenderer();

} // namespace renut::gpu
