#include "nuttool/studio.h"

#include <atomic>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include "renut_engine/app/app_paths.h"
#include "renut_engine/mods/custom_parts.h"
#include "assetdb.hpp"
#include "image.hpp"
#include "renutmodel.hpp"
#include "renutworld.hpp"

namespace renut::nuttool {
namespace {

namespace fs = std::filesystem;

// Bumped whenever the stage below changes, so an older studio is made again.
constexpr int kStageVersion = 3;
constexpr uint8_t kTypeScript = 0x19;
constexpr float kPi = 3.14159265358979f;

// The stage: a round floor, its outer ring darker, standing on a short wall; Banjo waits behind the camera.
constexpr float kFloorRadius = 9.0f;
constexpr float kInnerRadius = 7.0f;
constexpr float kRimDepth = 0.6f;
constexpr int kSegments = 64;

// NutTool's icon (icon/nuttool.png), a texture of the studio's bundle.
constexpr unsigned char kIconPng[] = {
#include "nuttool.png.inc"
};
constexpr const char kIconName[] = "aid_texture_renut_nuttool_icon";
constexpr uint32_t kIconSide = 128;

std::atomic<bool> g_ready{false};

std::string StageObj() {
  std::ostringstream obj;
  obj << "mtllib stage.mtl\n";
  obj << "v 0 0 0\n";  // 1: the middle
  for (int i = 0; i < kSegments; ++i) {
    const float a = 2 * kPi * float(i) / kSegments, c = std::cos(a), s = std::sin(a);
    obj << "v " << c * kInnerRadius << " 0 " << s * kInnerRadius << "\n";              // 2 + 4i
    obj << "v " << c * kFloorRadius << " 0 " << s * kFloorRadius << "\n";              // 3 + 4i
    obj << "v " << c * kFloorRadius << " " << -kRimDepth << " " << s * kFloorRadius << "\n";  // 4 + 4i
    obj << "v " << c * kInnerRadius << " 0 " << s * kInnerRadius << "\n";              // 5 + 4i: the ring's own edge
  }
  const auto at = [](int i, int k) { return 2 + 4 * (i % kSegments) + k; };
  // Faces counter-clockwise seen from where they face (up, or out for the wall).
  obj << "usemtl floor\n";
  for (int i = 0; i < kSegments; ++i)
    obj << "f 1 " << at(i + 1, 0) << " " << at(i, 0) << "\n";
  obj << "usemtl ring\n";
  for (int i = 0; i < kSegments; ++i)
    obj << "f " << at(i, 3) << " " << at(i + 1, 3) << " " << at(i + 1, 1) << " " << at(i, 1) << "\n";
  obj << "usemtl rim\n";
  for (int i = 0; i < kSegments; ++i)
    obj << "f " << at(i, 1) << " " << at(i + 1, 1) << " " << at(i + 1, 2) << " " << at(i, 2) << "\n";
  return obj.str();
}

constexpr const char kStageMtl[] = "newmtl floor\nKd 0.56 0.57 0.60\n"
                                   "newmtl ring\nKd 0.34 0.36 0.40\n"
                                   "newmtl rim\nKd 0.22 0.23 0.26\n";

bool WriteText(const fs::path &file, const std::string &text) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << text;
  return bool(out);
}

int MadeVersion(const fs::path &marker) {
  std::ifstream in(marker);
  int version = 0;
  in >> version;
  return in ? version : 0;
}

} // namespace

fs::path NutToolFolder() { return renut::paths::WritableFolder() / "nuttool"; }

fs::path StudioFolder() { return NutToolFolder() / "studio"; }

bool PrepareStudio(const fs::path &assets) {
  const fs::path folder = StudioFolder();
  const fs::path marker = folder / "stage_version.txt";
  std::error_code ec;
  if (MadeVersion(marker) == kStageVersion && fs::is_directory(folder / "worlds", ec)) {
    g_ready = true;
    return true;
  }
  try {
    fs::remove_all(folder, ec);
    fs::create_directories(folder / "stage", ec);
    if (!WriteText(folder / "stage" / "stage.obj", StageObj()) || !WriteText(folder / "stage" / "stage.mtl", kStageMtl))
      return false;
    nut::renutworld::World world;
    world.id = kStudioId;
    world.name = "NutTool";
    world.model = folder / "stage" / "stage.obj";
    world.spawn = std::array<float, 3>{0.0f, 1.0f, kInnerRadius + 1.0f};
    world.spawnYaw = 180;
    world.exitDoor = false;
    world.townDoor = false;
    world.bundle = "nuttool";
    world.sky = "1c1f26,3a3f4b";
    world.smoothDegrees = 30;
    nut::renutworld::make(assets, folder, world, true, [](const std::string &) {});
    // The main menu's NUTTOOL icon, loaded with the game in the studio's bundle.
    const nut::Bytes png(std::begin(kIconPng), std::end(kIconPng));
    std::string summary;
    const nut::Bytes icon = nut::renutworld::textureMaker(assets, kIconSide)(kIconName, nut::image::decodePng(png), summary);
    nut::renutmodel::addToMod(folder, world.bundle, {{IconId(), icon}}, {});
    // The world maker writes a mod's own files too; reNut declares the studio itself, so they go.
    fs::remove(folder / "mod.toml", ec);
    fs::remove_all(folder / "scripts", ec);
  } catch (const std::exception &) {
    fs::remove_all(folder, ec);
    return false;
  }
  if (!WriteText(marker, std::to_string(kStageVersion) + "\n"))
    return false;
  g_ready = true;
  return true;
}

bool StudioReady() { return g_ready.load(); }

uint32_t IconId() { return nut::assetdb::idFromName(kIconName).value_or(0); }

uint32_t StudioScene() { return renut::mods::AssetId(kTypeScript, std::string("renutworld_") + kStudioId); }

} // namespace renut::nuttool
