#include "game_texts.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>

#include "../log.h"
#include "assets.h"
#include "localisation.h"

namespace rexfabric::content {
namespace {

struct Registry {
  std::mutex mutex;
  GameTextsDecl decl;
  std::shared_ptr<const GameTexts> texts = std::make_shared<GameTexts>();
  // How many times the texts were asked to be listed (GameTextsChanged), and for which of those they last were. Out of
  // date while the two differ.
  uint64_t changes = 1;
  uint64_t made_for = 0;
  // Being listed by some thread now, so a read from inside the listing doesn't list them again.
  std::atomic<bool> listing = false;
};

Registry& Get() {
  static auto* registry = new Registry();
  return *registry;
}

// Lists the game's texts when they're out of date (or always, with `force`), outside the lock, as `list` reads the
// game. False when they're out of date and couldn't be listed now.
bool Ensure(bool force) {
  Registry& registry = Get();
  std::function<bool(std::vector<GameTextGroup>&)> list;
  uint64_t changes = 0;
  uint64_t version = 0;
  {
    std::lock_guard lock(registry.mutex);
    if (!registry.decl.list) return false;
    if (!force && registry.made_for == registry.changes) return true;
    list = registry.decl.list;
    changes = registry.changes;
    version = registry.texts->version;
  }
  if (registry.listing.exchange(true)) return false;
  auto texts = std::make_shared<GameTexts>();
  const bool listed = list(texts->groups);
  registry.listing.store(false);
  if (!listed) return false;
  texts->version = version + 1;
  size_t count = 0;
  for (const GameTextGroup& group : texts->groups) count += group.texts.size();
  RF_INFO("content: the game has {} texts in {} groups", count, texts->groups.size());
  std::lock_guard lock(registry.mutex);
  registry.texts = std::move(texts);
  registry.made_for = changes;
  return true;
}

}  // namespace

void DeclareGameTexts(GameTextsDecl decl) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  if (registry.decl.list) RF_WARN("content: the game declared its texts twice; the second time counts");
  registry.decl = std::move(decl);
}

void GameTextsChanged() {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  ++registry.changes;
}

bool FillGameTexts() { return Ensure(true); }

bool HasGameTexts() {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  return static_cast<bool>(registry.decl.list);
}

std::shared_ptr<const GameTexts> ReadGameTexts() {
  Ensure(false);
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  return registry.texts;
}

void LoadLanguageAssets(const std::vector<ModInfo>& mods) {
  std::function<void(std::map<std::string, std::map<std::string, std::string>>)> apply;
  {
    Registry& registry = Get();
    std::lock_guard lock(registry.mutex);
    apply = registry.decl.apply;
  }
  if (!apply) return;
  const std::vector<Language> languages = Languages();
  std::map<std::string, std::map<std::string, std::string>> merged;
  size_t assets = 0;
  for (const ModInfo& mod : mods) {
    for (const AssetInfo& info : ScanContent(mod).assets) {
      if (info.kind != AssetKind::kLanguage) continue;
      std::string error;
      const std::optional<LanguageAsset> asset = ReadLanguageAsset(info.file, &error);
      if (!asset) {
        RF_WARN("content: [{}] {}: {}", mod.folder, info.path, error);
        continue;
      }
      if (std::none_of(languages.begin(), languages.end(), [&asset](const Language& language) { return language.code == asset->language; })) {
        RF_WARN("content: [{}] {}: the game has no language \"{}\"", mod.folder, info.path, asset->language);
        continue;
      }
      // A later mod's text over an earlier one's; an out of date one still shows until it's fixed.
      std::map<std::string, std::string>& texts = merged[asset->language];
      for (const auto& [id, translation] : asset->texts) {
        if (!translation.text.empty()) texts[id] = translation.text;
      }
      ++assets;
    }
  }
  size_t count = 0;
  for (const auto& [language, texts] : merged) count += texts.size();
  RF_INFO("content: {} language assets from mods, changing {} of the game's texts", assets, count);
  apply(std::move(merged));
}

}  // namespace rexfabric::content
