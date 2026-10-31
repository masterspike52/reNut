#pragma once
// The on-screen keyboard's button and key icons, compiled in.

#include <cstdint>
#include <span>
#include <string_view>

namespace rexglue_guide::assets {

// The PNG of an icon by its Kenney Input Prompts name (e.g. "xbox_lb"), or
// empty if it isn't included.
std::span<const uint8_t> KeyIconPng(std::string_view name);

}  // namespace rexglue_guide::assets
