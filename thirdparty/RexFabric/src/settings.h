// Settings by name, as mods name them, and the settings Lua mods add.

#pragma once

#include <string>
#include <string_view>

#include <rex/setting.h>

namespace rexfabric {

// The scope of the settings mods add: "mods.<name>".
inline constexpr std::string_view kModSettingScope = "mods";

// A setting by its full name, a mod setting's own name, or an old flat name ("renut_vsync" for renut.renut_vsync).
rex::SettingBase* FindSetting(std::string_view name);

// Adds a mod's setting of type T, with its value from the config file if it has one. Null when it can't register.
template <typename T>
rex::Setting<T>* AddModSetting(std::string_view name, const T& value, std::string_view help, std::string category, std::string mod);

}  // namespace rexfabric
