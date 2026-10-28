#pragma once
// Files from assets/, compiled in (tools/prepare_assets.py builds them).

#include <cstddef>
#include <cstdint>
#include <span>

namespace rexglue_guide::assets {

enum class Icon {
  kAccount,
  kFriends,
  kPreferences,
  kPrivacy,
  kSystem,
  kTurnOff,
  kApps,
  kPin,
  kMusic,
  kKeyRb,
  kKeyLs,
  kKeyCursorLb,
  kKeyCursorRb,
  kKeyStart,
  kKeyLt,
  kCount,
};

enum class Sound { kPageLeft, kPageRight, kSelect, kOpen, kNotify, kMove, kBack, kClose, kCount };

enum class Font { kRegular, kSemibold, kLight, kCount };

constexpr int kGamerpicCount = 8;

std::span<const uint8_t> IconPng(Icon icon);
std::span<const uint8_t> GamerpicPng(int index);
std::span<const uint8_t> SoundWav(Sound sound);
std::span<const uint8_t> FontTtf(Font font);

}  // namespace rexglue_guide::assets
