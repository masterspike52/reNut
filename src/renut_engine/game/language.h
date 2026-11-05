/**
 * @file    game/language.h
 * @brief   The game's text language, as an option (language.cpp).
 */
#pragma once

namespace renut::game {

// Applies renut_language: unless it is Auto, sets user_language (the console
// language the game asks for as it boots) to the chosen language. Once at
// startup, after the config is loaded and before the runtime is built
// (RenutApp::OnPostInitLogging).
void ApplyLanguage();

}  // namespace renut::game
