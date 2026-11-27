// The game's own texts (docs/content_and_classes.md, The game's texts): what the game lists of them
// (rexfabric/content.h's DeclareGameTexts), and the mods' language assets that show their own in place of some.
//
// The texts are listed when they're first read and again after the game says they changed, on the game's terms: a
// game whose texts only its own thread can read lists them when it next can, and a read before that has what was
// there. Language assets load with the rest of the content; every loaded mod's are merged, a later mod's text over an
// earlier one's, and given to the game. Everything here may be called from any thread.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <rexfabric/content.h>

#include "../mods.h"

namespace rexfabric::content {

struct GameTexts {
  std::vector<GameTextGroup> groups;
  // Goes up each time the game lists them again, for an editor showing them; 0 before the first time.
  uint64_t version = 0;
};

// Whether the game lets mods change its texts.
bool HasGameTexts();

// The game's texts, listed first when they're out of date and the game can list them now.
std::shared_ptr<const GameTexts> ReadGameTexts();

// Loads the language assets of these mods (the ones that can load, in load order) and gives the game their texts.
// With LoadModContent.
void LoadLanguageAssets(const std::vector<ModInfo>& mods);

}  // namespace rexfabric::content
