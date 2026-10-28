/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @brief       Xbox 360 XAM account structures, ported from xenia-canary
 *              netplay (src/xenia/kernel/xam/xam.h).
 * @modified    2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <string>

#include <rex/assert.h>
#include <rex/memory.h>
#include <rex/string.h>
#include <rex/string/utf8.h>

namespace rex {
namespace system {
namespace xam {

// User slot constants (match XAM).
constexpr uint8_t XUserMaxUserCount = 4;
constexpr uint8_t XUserIndexLatest = 0xFD;
constexpr uint8_t XUserIndexNone = 0xFE;
constexpr uint8_t XUserIndexAny = 0xFF;

// XamUserGetXUID type masks.
constexpr uint32_t X_USER_XUID_OFFLINE = 0x01;
constexpr uint32_t X_USER_XUID_ONLINE = 0x02;
constexpr uint32_t X_USER_XUID_GUEST = 0x04;

// XamUserGetSigninInfo flags (netplay semantics).
#define X_USER_GET_SIGNIN_INFO_ONLINE_XUID_ONLY 0x00000001
#define X_USER_GET_SIGNIN_INFO_OFFLINE_XUID_ONLY 0x00000002

#define X_USER_INFO_FLAG_LIVE_ENABLED 0x00000001
#define X_USER_INFO_FLAG_GUEST 0x00000002

enum class X_USER_SIGNIN_STATE : uint32_t {
  NotSignedIn,
  SignedInLocally,  // Offline
  SignedInToLive,   // Online
};

// Country codes are the XOnlineCountry values from the online spa data.
enum class XOnlineCountry : uint8_t {
  kNone = 0,
  kUnitedStates = 103,
};

inline bool IsOnlineXUID(uint64_t xuid) {
  return ((xuid >> 48) & 0xFFFF) == 0x0009;
}
inline bool IsOfflineXUID(uint64_t xuid) {
  return ((xuid >> 48) & 0xF000) == 0xE000;
}
inline bool IsGuestXUID(uint64_t xuid) {
  const uint32_t HighPart = xuid >> 48;
  // Guest XUIDs begin with 0xB1XY where X is sponsor guest number.
  return (HighPart & 0xFF00) == 0xB100;
}
inline bool IsValidXUID(uint64_t xuid) {
  return IsOnlineXUID(xuid) || IsOfflineXUID(xuid) || IsGuestXUID(xuid);
}

#pragma pack(push, 4)
struct X_XAMACCOUNTINFO {
  enum AccountReservedFlags {
    kPasswordProtected = 0x10000000,
    kLiveEnabled = 0x20000000,
    kRecovering = 0x40000000,
    kVersionMask = 0x000000FF
  };

  enum AccountUserFlags {
    kPaymentInstrumentCreditCard = 1,

    kCountryMask = 0xFF00,
    kSubscriptionTierMask = 0xF00000,
    kLanguageMask = 0x3E000000,

    kParentalControlEnabled = 0x1000000,
  };

  enum AccountXboxLiveServiceProvider {
    LiveDisabled = 0,
    ProductionNet = 0x50524F44,  // 'PROD'
    PartnerNet = 0x50415254      // 'PART'
  };

  enum AccountSubscriptionTier {
    kSubscriptionTierNone = 0,
    kSubscriptionTierSilver = 3,
    kSubscriptionTierGold = 6,
    kSubscriptionTierFamilyGold = 9
  };

  enum AccountLiveFlags { kAcctRequiresManagement = 1 };

  rex::be<uint32_t> reserved_flags;
  rex::be<uint32_t> live_flags;
  char16_t gamertag[0x10];
  rex::be<uint64_t> xuid_online;  // 0x0009....
  rex::be<uint32_t> cached_user_flags;
  rex::be<uint32_t> network_id;
  char passcode[4];
  char online_domain[0x14];
  char online_kerberos_realm[0x18];
  char online_key[0x10];
  char passport_membername[0x72];
  char passport_password[0x20];
  char owner_passport_membername[0x72];

  bool IsPasscodeEnabled() const {
    return static_cast<bool>(reserved_flags &
                             AccountReservedFlags::kPasswordProtected);
  }

  bool IsLiveEnabled() const {
    return static_cast<bool>(reserved_flags &
                             AccountReservedFlags::kLiveEnabled);
  }

  uint64_t GetOnlineXUID() const { return xuid_online; }

  std::string_view GetOnlineDomain() const {
    return std::string_view(online_domain);
  }

  uint32_t GetReservedFlags() const { return reserved_flags; }
  uint32_t GetCachedFlags() const { return cached_user_flags; }

  XOnlineCountry GetCountry() const {
    return static_cast<XOnlineCountry>((cached_user_flags & kCountryMask) >> 8);
  }

  AccountSubscriptionTier GetSubscriptionTier() const {
    return static_cast<AccountSubscriptionTier>(
        (cached_user_flags & kSubscriptionTierMask) >> 20);
  }

  bool IsParentalControlled() const {
    return static_cast<bool>((cached_user_flags & kLanguageMask) >> 24);
  }

  uint32_t GetLanguage() const {
    return (cached_user_flags & kLanguageMask) >> 25;
  }

  AccountXboxLiveServiceProvider GetXboxLiveServiceProvider() const {
    return static_cast<AccountXboxLiveServiceProvider>(network_id.get());
  }

  std::string GetGamertagString() const {
    // gamertag is big-endian UTF-16 on disk; byte-swap into host order.
    std::u16string tag;
    tag.reserve(0x10);
    for (size_t i = 0; i < 0x10; ++i) {
      char16_t c = rex::byte_swap(gamertag[i]);
      if (!c) {
        break;
      }
      tag.push_back(c);
    }
    return rex::string::to_utf8(tag);
  }

  void SetGamertag(const std::string_view gamertag_utf8) {
    const std::u16string gamertag_u16 = rex::string::to_utf16(gamertag_utf8);
    std::memset(gamertag, 0, sizeof(gamertag));
    rex::string::copy_and_swap_truncating(
        gamertag, gamertag_u16, sizeof(gamertag) / sizeof(gamertag[0]));
  }

  void ToggleLiveFlag(bool is_live) {
    reserved_flags = reserved_flags & ~AccountReservedFlags::kLiveEnabled;

    if (is_live) {
      reserved_flags = reserved_flags | AccountReservedFlags::kLiveEnabled;
    }
  }

  void SetCountry(XOnlineCountry country) {
    cached_user_flags = cached_user_flags & ~kCountryMask;
    cached_user_flags =
        cached_user_flags |
        ((static_cast<uint32_t>(country) << 8) & kCountryMask);
  }

  void SetLanguage(uint32_t language) {
    cached_user_flags = cached_user_flags & ~kLanguageMask;
    cached_user_flags =
        cached_user_flags | ((language << 25) & kLanguageMask);
  }

  void SetSubscriptionTier(AccountSubscriptionTier sub_tier) {
    cached_user_flags = cached_user_flags & ~kSubscriptionTierMask;
    cached_user_flags =
        cached_user_flags |
        ((static_cast<uint32_t>(sub_tier) << 20) & kSubscriptionTierMask);
  }

  void SetXboxLiveServiceProvider(
      AccountXboxLiveServiceProvider service_provider) {
    network_id = static_cast<uint32_t>(service_provider);
  }
};
static_assert_size(X_XAMACCOUNTINFO, 0x17C);
#pragma pack(pop)

}  // namespace xam
}  // namespace system
}  // namespace rex
