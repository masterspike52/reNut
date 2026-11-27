#include "viewport.h"

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>

#include "../log.h"

namespace rexfabric::content {
namespace {

std::mutex g_mutex;
std::shared_ptr<const ViewportRenderer> g_renderer;
std::atomic<uint64_t> g_next_viewport{1};
bool g_warned = false;

std::shared_ptr<const ViewportRenderer> Renderer() {
  std::lock_guard lock(g_mutex);
  return g_renderer;
}

}  // namespace

void SetViewportRenderer(ViewportRenderer renderer) {
  std::lock_guard lock(g_mutex);
  g_renderer = renderer.show ? std::make_shared<const ViewportRenderer>(std::move(renderer)) : nullptr;
}

uint64_t NewViewport() {
  return g_next_viewport.fetch_add(1);
}

std::optional<ViewportImage> ShowViewport(uint64_t viewport, const ViewportView& view) {
  const std::shared_ptr<const ViewportRenderer> renderer = Renderer();
  if (!renderer) {
    std::lock_guard lock(g_mutex);
    if (!g_warned) RF_WARN("content: no viewport renderer: the game has not registered one, so avatars can't be previewed. Components can still be edited by their values");
    g_warned = true;
    return std::nullopt;
  }
  try {
    return renderer->show(viewport, view);
  } catch (const std::exception& error) {
    return ViewportImage{.message = error.what()};
  }
}

void CloseViewport(uint64_t viewport) {
  const std::shared_ptr<const ViewportRenderer> renderer = Renderer();
  if (!renderer || !renderer->close) return;
  try {
    renderer->close(viewport);
  } catch (const std::exception& error) {
    RF_WARN("content: closing viewport {}: {}", viewport, error.what());
  }
}

}  // namespace rexfabric::content
