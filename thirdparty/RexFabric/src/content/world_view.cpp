#include "world_view.h"

#include <exception>
#include <memory>
#include <mutex>

#include "../log.h"

namespace rexfabric::content {
namespace {

std::mutex g_mutex;
std::shared_ptr<const WorldViewDecl> g_view;

std::shared_ptr<const WorldViewDecl> View() {
  std::lock_guard lock(g_mutex);
  return g_view;
}

}  // namespace

void SetWorldView(WorldViewDecl decl) {
  std::lock_guard lock(g_mutex);
  g_view = decl.camera ? std::make_shared<const WorldViewDecl>(std::move(decl)) : nullptr;
}

bool HasWorldView() { return View() != nullptr; }

std::optional<WorldCamera> WorldCameraNow() {
  const std::shared_ptr<const WorldViewDecl> view = View();
  if (!view) return std::nullopt;
  try {
    WorldCamera camera;
    if (view->camera(camera)) return camera;
  } catch (const std::exception& error) {
    RF_WARN("content: the game's camera: {}", error.what());
  }
  return std::nullopt;
}

void AimIntoWorld(std::vector<WorldRay> rays) {
  const std::shared_ptr<const WorldViewDecl> view = View();
  if (!view || !view->aim) return;
  try {
    view->aim(std::move(rays));
  } catch (const std::exception& error) {
    RF_WARN("content: aiming into the world: {}", error.what());
  }
}

std::vector<WorldHit> WorldHits() {
  const std::shared_ptr<const WorldViewDecl> view = View();
  if (!view || !view->hits) return {};
  try {
    return view->hits();
  } catch (const std::exception& error) {
    RF_WARN("content: the world's hits: {}", error.what());
    return {};
  }
}

}  // namespace rexfabric::content
