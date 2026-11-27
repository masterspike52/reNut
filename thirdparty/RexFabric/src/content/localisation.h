// Mods' texts in the game's languages (docs/content_and_classes.md, Localisation). A mod writes its texts in its own
// language (its metadata's `language`, the game's first when it names none): every `text` value of its assets and its
// enums' display names. A string table per language, beside its content, holds their translations:
//
//   mods/MyMod/localisation/french.json
//   {"language": "french", "texts": {"MyMod/Rock:display_name": {"source": "Mossy Rock", "text": "Rocher moussu"}}}
//
// A text's key says where it is: the asset, a colon, then the property, field or enum value, and inside a value each
// struct field after a dot, each array element by its number and each map entry by its key in brackets
// ("MyMod/Rocks:Home.spots[2].label"). A translation keeps the source it was made from, so one whose source changed
// since is out of date. At runtime a text shows in the language the game shows, or as written when that language has
// no translation of it.

#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "../mods.h"
#include "values.h"

namespace rexfabric::content {

// The folder of a mod its string tables are in.
inline constexpr std::string_view kLocalisationFolder = "localisation";

struct Language {
  std::string code;  // as files and the game name it ("french")
  std::string name;  // as the player reads it ("French")
};

// The game's languages (rexfabric/content.h's DeclareLanguages), in its order; and the one it shows now, empty when
// it can't say.
std::vector<Language> Languages();
std::string CurrentLanguage();
// A language's name, or the code itself for one the game doesn't have.
std::string LanguageName(std::string_view code);
// The language a mod writes its texts in.
std::string OwnLanguage(const ModInfo& mod);

// Keys: an asset's and where in it ("MyMod/Rock", "display_name"), and the keys inside a value.
std::string TextKey(std::string_view asset, std::string_view where);
std::string FieldKey(const std::string& key, std::string_view field);
std::string ElementKey(const std::string& key, size_t index);
std::string EntryKey(const std::string& key, const nlohmann::json& map_key);

// Calls `visit` with every text in `value`, of `type`, with its key: `key` itself for a single text, and the keys of
// texts inside its structs, arrays and maps. A struct field the value lacks, or has as the struct has it, is its
// default, keyed by the struct.
void WalkTexts(const TypeLookup& types, const ValueType& type, const nlohmann::json& value, const std::string& key, const std::function<void(const std::string& key, const std::string& text)>& visit);

// A text of a mod, as its files have it: its key, the asset it's in, where there (for the editor) and its source.
struct TextEntry {
  std::string key;
  std::string asset;
  std::string where;
  std::string source;
};

// Every text of a mod's assets, in path order.
std::vector<TextEntry> ModTexts(const ModList& mods, const ModInfo& mod);

struct Translation {
  std::string source;  // the text it translates, as it was then
  std::string text;

  bool operator==(const Translation&) const = default;
};
using StringTable = std::map<std::string, Translation>;

// A mod's string table for a language: empty when it has none.
StringTable ReadStringTable(const ModInfo& mod, std::string_view language);
bool WriteStringTable(const ModInfo& mod, std::string_view language, const StringTable& table, std::string* error);
// Makes every key of `from`, or of anything inside it, start with `to` instead, in every string table of the mod.
void RenameTextKeys(const ModInfo& mod, std::string_view from, std::string_view to);

// The mods whose texts translate at runtime: the ones that loaded. Their tables are read when a language is first
// needed. With LoadModContent.
void LoadTranslations(const std::vector<ModInfo>& mods);
// A text as the game's language shows it: its translation in the mod's table for that language, or `source` when
// there's none (or the language is the mod's own). Any thread.
std::string Translate(std::string_view key, std::string_view source);

}  // namespace rexfabric::content
