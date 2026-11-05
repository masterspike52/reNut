/**
 * @file    game/language.cpp
 * @brief   LANGUAGE in RENUT SETTINGS: the game's text language.
 *
 * N&B has no language option of its own: as it boots it asks the console
 * (XGetLanguage) and loads that language's loctext. The runtime answers from
 * its user_language setting, which nothing exposed: the game stayed English
 * unless user_language was edited into renut.toml by hand.
 *
 * renut_language is that option. Auto leaves user_language alone (whatever
 * the config or the runtime's default says); a language writes its Xbox 360
 * language code over user_language before the runtime is built
 * (RenutApp::OnPostInitLogging), so the game's boot reads the choice. The
 * game reads it once, so a change takes a restart: the cvar's lifecycle says
 * so, and the menus listing the Nuts&Bolts categories (the pause menu's
 * RENUT SETTINGS tab, the F8 settings dialog) show it under LANGUAGE with
 * their restart mark.
 *
 * Only the languages the game has text for are offered. The USA disc also
 * carries Czech, Dutch, Norwegian and Swedish loctext, but no XLanguage code
 * names them (the console picked them by locale), and it has no Portuguese
 * text, so XLanguage 9 would read as English: those are left out.
 */
#include "renut_engine/game/language.h"

#include <rex/cvar.h>

#include <string>

#include "renut_engine/app/renut_logging.h"

REXCVAR_DEFINE_STRING(renut_language, "Auto", "Nuts&Bolts/Language",
                      "The game's text language, read once as it boots. Auto asks the host "
                      "(the user_language setting). Takes effect after a restart.")
    .allowed({"Auto", "English", "Japanese", "German", "French", "Spanish", "Italian", "Korean",
              "Traditional Chinese", "Simplified Chinese", "Polish", "Russian"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace renut::game {

namespace {

// XLanguage, as XGetLanguage answers it.
struct Language {
  const char *name;
  int code;
};
constexpr Language kLanguages[] = {
    {"English", 1},  {"Japanese", 2}, {"German", 3},  {"French", 4},
    {"Spanish", 5},  {"Italian", 6},  {"Korean", 7},  {"Traditional Chinese", 8},
    {"Simplified Chinese", 10},       {"Polish", 11}, {"Russian", 12},
};

}  // namespace

void ApplyLanguage() {
  const std::string language = REXCVAR_GET(renut_language);
  if (language == "Auto")
    return;
  for (const Language &entry : kLanguages) {
    if (language != entry.name)
      continue;
    rex::cvar::SetFlagByName("user_language", std::to_string(entry.code));
    RNUT_INFO("language: {} (user_language {})", entry.name, entry.code);
    return;
  }
  RNUT_WARN("language: '{}' isn't a language renut_language offers; Auto", language);
}

}  // namespace renut::game
