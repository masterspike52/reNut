/**
 * @file    online/xnaddr.h
 * @brief   reNut's XNADDRs, made from a player's online ID.
 *
 * N&B's peers know each other by XNADDR, which they pass around in session
 * info and their own packets: a player learns the others of a session only
 * through the XNADDRs the host sends. reNut reaches a player by its online ID
 * (EOS PUID, eos.h), which cannot be made back from the XUID, so the XNADDR
 * carries it, and every machine makes the same XNADDR for a player:
 *   ina and inaOnline 11.x.y.z (the XUID's low 24 bits), wPortOnline the
 *   game's port, abEnet the XUID's low 48 bits (all of its hash), abOnline
 *   "RN2" and 0, then the online ID's 16 bytes.
 * All big-endian, as the game reads them.
 *
 * The IN_ADDRs the game sends to are another matter: XNetXnAddrToInAddr gives
 * one per player and session (as Xbox Live's secure connections are), handed
 * out by transport.cpp in 10.0.0.0/8.
 */
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include "renut_engine/online/eos.h"

namespace renut::online::xnaddr {

using XnAddr = std::array<uint8_t, 36>;

constexpr uint16_t kGamePort = 1000;  // N&B's VDP socket
constexpr uint8_t kXnAddrNet = 11;
constexpr uint8_t kInAddrNet = 10;
constexpr char kTag[4] = {'R', 'N', '2', 0};
constexpr size_t kTagOffset = 16;
constexpr size_t kIdOffset = 20;
constexpr uint64_t kXuidBase = 0x0009000000000000ull;

// An IN_ADDR transport.cpp handed out (host order).
inline bool IsReNutInAddr(uint32_t ina) {
  return (ina >> 24) == kInAddrNet;
}

// The XNADDR of the player with online ID 'id'.
inline XnAddr Make(const std::string &id) {
  XnAddr a{};
  const uint64_t xuid = eos::XuidOf(id);
  const uint32_t ina = (uint32_t(kXnAddrNet) << 24) | uint32_t(xuid & 0xFFFFFF);
  for (int i = 0; i < 4; ++i) {
    a[i] = uint8_t(ina >> (24 - 8 * i));
    a[4 + i] = a[i];
  }
  a[8] = uint8_t(kGamePort >> 8);
  a[9] = uint8_t(kGamePort);
  for (int i = 0; i < 6; ++i)
    a[10 + i] = uint8_t(xuid >> (40 - 8 * i));
  std::memcpy(a.data() + kTagOffset, kTag, sizeof(kTag));
  eos::IdBytes bytes{};
  if (eos::IdToBytes(id, bytes))
    std::memcpy(a.data() + kIdOffset, bytes.data(), bytes.size());
  return a;
}

// The XUID of a reNut XNADDR.
inline std::optional<uint64_t> XuidOf(const uint8_t *a) {
  if (std::memcmp(a + kTagOffset, kTag, sizeof(kTag)) != 0)
    return std::nullopt;
  uint64_t xuid = kXuidBase;
  for (int i = 0; i < 6; ++i)
    xuid |= uint64_t(a[10 + i]) << (40 - 8 * i);
  return xuid;
}

// The online ID a reNut XNADDR carries (empty when it is not one).
inline std::string IdOf(const uint8_t *a) {
  if (std::memcmp(a + kTagOffset, kTag, sizeof(kTag)) != 0)
    return {};
  eos::IdBytes bytes{};
  std::memcpy(bytes.data(), a + kIdOffset, bytes.size());
  return eos::IdFromBytes(bytes);
}

} // namespace renut::online::xnaddr
