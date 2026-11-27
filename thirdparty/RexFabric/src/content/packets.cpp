#include "packets.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <optional>

#include "../log.h"
#include "assets.h"
#include "localisation.h"
#include "types.h"
#include "values.h"

namespace rexfabric::content {
namespace {

// Where in a packet's file its texts are, for their keys (localisation.h, TextKey).
constexpr std::string_view kNameText = "name";
constexpr std::string_view kHelpText = "help";

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

bool Same(std::string_view a, std::string_view b) { return Lower(a) == Lower(b); }

// A mod's packet as it loaded: the packet with its texts as the mod wrote them, translated when it's listed, and the
// mod's place in the load order.
struct Loaded {
  Packet packet;
  size_t mod_order = 0;
};

struct Registry {
  std::mutex mutex;
  bool declared = false;
  PacketsDecl decl;
  std::vector<Loaded> loaded;  // sorted as Packets lists them
  uint64_t revision = 0;       // up by one at each load
};

Registry& Get() {
  static auto* registry = new Registry();
  return *registry;
}

// A mod's packet as the game reads it: its texts translated into the language the game shows.
Packet Translated(Packet packet) {
  packet.name = Translate(TextKey(packet.path, kNameText), packet.name);
  packet.help = Translate(TextKey(packet.path, kHelpText), packet.help);
  return packet;
}

// Each field of the look struct, with the packet's value or the field's default.
AvatarValues LookValues(const std::string& look_struct, const nlohmann::json& look) {
  AvatarValues values;
  const std::optional<Struct> declared = look_struct.empty() ? std::nullopt : FindStruct(look_struct);
  if (!declared) return values;
  for (const Property& field : declared->fields) {
    const nlohmann::json* set = nullptr;
    for (const auto& [name, value] : look.items()) {
      if (Same(name, field.name)) set = &value;
    }
    values[field.name] = ToPropertyValue(set ? *set : field.value);
  }
  return values;
}

// Why a packet's base isn't one of the game's, or nothing.
std::string BaseProblem(const std::string& base_enum, const std::string& base) {
  const std::optional<Enum> bases = FindEnum(base_enum);
  if (!bases) return "the game's packet base enum " + base_enum + " isn't there";
  const bool known = std::any_of(bases->values.begin(), bases->values.end(), [&base](const EnumValue& value) { return value.name == base; });
  return known ? std::string() : "its base \"" + base + "\" isn't one of " + base_enum;
}

}  // namespace

void DeclarePackets(PacketsDecl decl) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  if (registry.declared) {
    RF_ERROR("content: the game's packets are declared twice; the second is ignored");
    return;
  }
  registry.declared = true;
  registry.decl = std::move(decl);
}

bool HasPackets() {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  return registry.declared;
}

std::string PacketBaseEnum() {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  return registry.decl.base;
}

std::string PacketLookStruct(std::string_view base) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  const auto found = registry.decl.looks.find(std::string(base));
  return found != registry.decl.looks.end() ? found->second : std::string();
}

std::vector<std::string> GamePacketPaths() {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  std::vector<std::string> paths;
  for (const Packet& packet : registry.decl.built_in) paths.push_back(packet.path);
  return paths;
}

void LoadPackets(const std::vector<ModInfo>& mods) {
  if (!HasPackets()) return;
  const std::string base_enum = PacketBaseEnum();
  std::vector<Loaded> loaded;
  for (size_t order = 0; order < mods.size(); ++order) {
    const ModInfo& mod = mods[order];
    for (const AssetInfo& info : ScanContent(mod).assets) {
      if (info.kind != AssetKind::kPacket) continue;
      std::string error;
      std::optional<PacketAsset> asset = ReadPacketAsset(info.file, &error);
      if (asset) error = BaseProblem(base_enum, asset->base);
      if (!asset || !error.empty()) {
        RF_WARN("content: [{}] {}: {}", mod.folder, info.path, error);
        continue;
      }
      Packet packet{.path = info.path, .mod = mod.folder, .base = asset->base, .name = asset->name, .help = asset->help, .icon = asset->icon, .look = LookValues(PacketLookStruct(asset->base), asset->look), .unlock = asset->unlock, .level = asset->level, .price = asset->price, .order = asset->order};
      loaded.push_back({std::move(packet), order});
    }
  }
  std::stable_sort(loaded.begin(), loaded.end(), [](const Loaded& a, const Loaded& b) {
    if (a.mod_order != b.mod_order) return a.mod_order < b.mod_order;
    if (a.packet.order != b.packet.order) return a.packet.order < b.packet.order;
    return Lower(a.packet.path) < Lower(b.packet.path);
  });
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  RF_INFO("content: {} packets from mods", loaded.size());
  registry.loaded = std::move(loaded);
  ++registry.revision;
}

uint64_t PacketsRevision() {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  return registry.revision;
}

std::vector<Packet> Packets() {
  std::vector<Packet> packets;
  std::vector<Packet> mods;
  {
    Registry& registry = Get();
    std::lock_guard lock(registry.mutex);
    packets = registry.decl.built_in;
    for (const Loaded& loaded : registry.loaded) mods.push_back(loaded.packet);
  }
  // Outside the lock: a translation may read a string table.
  for (Packet& packet : mods) packets.push_back(Translated(std::move(packet)));
  return packets;
}

std::optional<Packet> FindPacket(std::string_view path) {
  std::optional<Packet> found;
  bool game = false;
  {
    Registry& registry = Get();
    std::lock_guard lock(registry.mutex);
    for (const Packet& packet : registry.decl.built_in) {
      if (!found && Same(packet.path, path)) {
        found = packet;
        game = true;
      }
    }
    for (const Loaded& loaded : registry.loaded) {
      if (!found && Same(loaded.packet.path, path)) found = loaded.packet;
    }
  }
  if (!found || game) return found;
  return Translated(std::move(*found));
}

}  // namespace rexfabric::content
