#pragma once

#include <rex/result.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>

namespace renut::settings {

// The scope of every reNut setting; a member keeps its old cvar name, so "renut.<old name>".
inline constexpr const char *kScope = "renut";

// Queues a settings class for RegisterAll. Safe during static initialization.
void Add(rex::SettingOwner &owner);

// Registers every queued class (from RenutApp::OnConfigure), keeping each old cvar name as an alias.
rex::Result<void> RegisterAll(rex::SettingsRegistry &registry);

}  // namespace renut::settings
