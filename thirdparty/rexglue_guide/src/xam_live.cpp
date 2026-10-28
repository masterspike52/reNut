// XAM functions the Guide takes over from the rexglue runtime so titles see the
// RexLiveless account: identity, LIVE sign-in state, privileges and friends.
//
// These are definitions of the runtime's own import symbols. Compiled into the
// game executable (rexglue_guide_attach), they win over the runtime DLL's
// versions at link time; anything not handled here is passed on to the
// runtime's implementation, found by name.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <iterator>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>

#include "account_info.h"
#include "guest.h"
#include "live.h"
#include "live_sessions.h"
#include "xlivebase_app.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <dlfcn.h>
#include <link.h>
#endif

namespace rexglue_guide::live {
namespace {

std::mutex g_mutex;
Backend* g_backends[kMaxUsers] = {};
std::vector<std::function<void(Backend&)>> g_work;

}  // namespace

LiveSnapshot Snapshot(int user) {
  std::lock_guard lock(g_mutex);
  if (user < 0 || user >= kMaxUsers || !g_backends[user]) return LiveSnapshot{};
  return g_backends[user]->Snapshot();
}

bool Online() {
  const LiveSnapshot s = Snapshot();
  return s.signed_in && s.connected;
}

void Post(std::function<void(Backend&)> work) {
  std::lock_guard lock(g_mutex);
  g_work.push_back(std::move(work));
}

void NotifySigninChanged() {
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel) return;
  // XN_SYS_SIGNINCHANGED with the signed-in user mask: titles re-read the
  // profiles, as when a different profile signs in on a console. Player 1
  // always has the runtime's profile; players 2-4 only a Guide one.
  uint32_t mask = 1;
  for (int user = 1; user < kMaxUsers; ++user) {
    if (Snapshot(user).signed_in) mask |= 1u << user;
  }
  kernel->BroadcastNotification(0x0000000A, mask);
  // XN_LIVE_CONNECTIONCHANGED: titles track LIVE logon from this (TiP only
  // enables its LIVE features after XONLINE_S_LOGON_CONNECTION_ESTABLISHED).
  kernel->BroadcastNotification(0x02000001, Snapshot().signed_in ? 0x001510F0u : 0x001510F1u);
}

void Attach(int user, Backend* backend) {
  if (user < 0 || user >= kMaxUsers) return;
  std::lock_guard lock(g_mutex);
  g_backends[user] = backend;
}

void Pump() {
  std::vector<std::function<void(Backend&)>> work;
  Backend* backend;
  {
    std::lock_guard lock(g_mutex);
    work.swap(g_work);
    backend = g_backends[0];
  }
  if (!backend) return;
  for (auto& w : work) w(*backend);
}

}  // namespace rexglue_guide::live

using rex::X_HRESULT;
using rex::X_RESULT;

namespace {

using rex::memory::load_and_swap;
using rex::memory::store_and_swap;
using rexglue_guide::guest::CallOriginal;

uint8_t* Guest(uint32_t address) {
  return rexglue_guide::guest::At(address);
}

void SetResult(PPCContext& ctx, uint32_t value) {
  ctx.r3.u64 = value;
}

constexpr uint32_t kSignedInLocally = 1;
constexpr uint32_t kSignedInToLive = 2;

// Signed in to RexLiveless reads as signed in to LIVE even while the
// connection is down, so the player's identity never changes mid-game.
uint32_t SigninState(const rexglue_guide::LiveSnapshot& s) {
  return s.signed_in ? kSignedInToLive : kSignedInLocally;
}

// Logs the first time each override handles a call.
#define GUIDE_TRACE(name)                                                     do {                                                                          static bool logged = false;                                                 if (!logged) {                                                                logged = true;                                                              REXKRNL_INFO("Rexglue Guide: {} answered for the RexLiveless account", name);     }                                                                         } while (0)

void WriteGamertag(uint8_t* dest, size_t size, const std::string& gamertag) {
  std::memset(dest, 0, size);
  std::memcpy(dest, gamertag.data(), std::min(gamertag.size(), size - 1));
}

}  // namespace

// The Guide account of a player (user index 0-3), if one is signed in there.
static rexglue_guide::LiveSnapshot UserSnapshot(uint32_t user) {
  return user < uint32_t(rexglue_guide::live::kMaxUsers) ? rexglue_guide::live::Snapshot(int(user))
                                                         : rexglue_guide::LiveSnapshot{};
}

// XamUserGetXUID(user_index, type_mask, xuid_ptr). Players 2-4 have no
// runtime profile, so their Guide account answers for any XUID type.
extern "C" REX_FUNC(__imp__XamUserGetXUID) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t mask = ctx.r4.u32;
  const auto s = UserSnapshot(user);
  if (!s.signed_in || (user == 0 && !(mask & rex::system::xam::X_USER_XUID_ONLINE))) {
    return CallOriginal("__imp__XamUserGetXUID", ctx, base);
  }
  GUIDE_TRACE("XamUserGetXUID");
  if (uint8_t* out = Guest(ctx.r5.u32)) store_and_swap<uint64_t>(out, s.me.xuid);
  SetResult(ctx, 0);
}

// XamShowGamerCardUIForXUID(user_index, XUID, unused): XShowGamerCardUI. The
// runtime's is a stub that shows nothing; the Guide opens on the player's card.
extern "C" REX_FUNC(__imp__XamShowGamerCardUIForXUID) {
  const uint64_t xuid = ctx.r4.u64;
  if (!xuid || !rexglue_guide::live::Snapshot().signed_in) {
    return CallOriginal("__imp__XamShowGamerCardUIForXUID", ctx, base);
  }
  GUIDE_TRACE("XamShowGamerCardUIForXUID");
  rexglue_guide::live::RequestGamercard(xuid);
  SetResult(ctx, 0);
}

// XamUserGetSigninState(user_index)
extern "C" REX_FUNC(__imp__XamUserGetSigninState) {
  const auto s = UserSnapshot(ctx.r3.u32);
  if (!s.signed_in) return CallOriginal("__imp__XamUserGetSigninState", ctx, base);
  SetResult(ctx, SigninState(s));
}

// XamUserGetSigninInfo(user_index, flags, X_USER_SIGNIN_INFO*)
//   +0 xuid, +8 info flags, +0xC signin state, +0x10 guest number,
//   +0x14 sponsor, +0x18 name[16]
// Player 1's comes from the runtime's profile with the Guide account over it;
// players 2-4 have only the Guide account.
extern "C" REX_FUNC(__imp__XamUserGetSigninInfo) {
  constexpr uint32_t kSigninInfoSize = 0x28;
  const uint32_t user = ctx.r3.u32;
  const uint32_t flags = ctx.r4.u32;
  const uint32_t info_ptr = ctx.r5.u32;
  const auto s = UserSnapshot(user);
  uint8_t* info = Guest(info_ptr);
  if (user != 0 && s.signed_in && info) {
    std::memset(info, 0, kSigninInfoSize);
  } else {
    CallOriginal("__imp__XamUserGetSigninInfo", ctx, base);
    if (!s.signed_in || !info || ctx.r3.u32 != 0) return;
  }
  constexpr uint32_t kOfflineXuidOnly = 1;
  GUIDE_TRACE("XamUserGetSigninInfo");
  if (flags != kOfflineXuidOnly || user != 0) store_and_swap<uint64_t>(info + 0, s.me.xuid);
  store_and_swap<uint32_t>(info + 0x8, load_and_swap<uint32_t>(info + 0x8) | 1);  // LIVE enabled
  store_and_swap<uint32_t>(info + 0xC, SigninState(s));
  WriteGamertag(info + 0x18, 16, s.me.gamertag);
  SetResult(ctx, 0);
}

// XamUserGetName(user_index, buffer, buffer_length)
extern "C" REX_FUNC(__imp__XamUserGetName) {
  const auto s = UserSnapshot(ctx.r3.u32);
  uint8_t* buffer = Guest(ctx.r4.u32);
  const uint32_t length = ctx.r5.u32;
  if (!s.signed_in || !buffer || !length) {
    return CallOriginal("__imp__XamUserGetName", ctx, base);
  }
  GUIDE_TRACE("XamUserGetName");
  WriteGamertag(buffer, length, s.me.gamertag);
  SetResult(ctx, 0);
}

// XamUserCheckPrivilege(user_index, privilege, BOOL* result)
extern "C" REX_FUNC(__imp__XamUserCheckPrivilege) {
  const uint32_t user = ctx.r3.u32;
  // XUSER_INDEX_ANY (0xFF): player 1's account decides.
  if (!UserSnapshot(user == 0xFF ? 0 : user).signed_in) return CallOriginal("__imp__XamUserCheckPrivilege", ctx, base);
  GUIDE_TRACE("XamUserCheckPrivilege");
  if (uint8_t* out = Guest(ctx.r5.u32)) store_and_swap<uint32_t>(out, 1);
  SetResult(ctx, 0);
}

// XamUserAreUsersFriends(user_index, XUID* xuids, count, BOOL* result, overlapped)
extern "C" REX_FUNC(__imp__XamUserAreUsersFriends) {
  const auto s = UserSnapshot(ctx.r3.u32);
  if (!s.signed_in) return CallOriginal("__imp__XamUserAreUsersFriends", ctx, base);
  const uint8_t* xuids = Guest(ctx.r4.u32);
  const uint32_t count = ctx.r5.u32;
  bool all = count > 0 && xuids;
  for (uint32_t i = 0; all && i < count; ++i) {
    const uint64_t xuid = load_and_swap<uint64_t>(xuids + i * 8);
    all = std::any_of(s.friends.begin(), s.friends.end(),
                      [&](const rexglue_guide::Friend& f) { return f.profile.xuid == xuid; });
  }
  if (uint8_t* out = Guest(ctx.r6.u32)) store_and_swap<uint32_t>(out, all ? 1 : 0);
  if (ctx.r7.u32) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(ctx.r7.u32, 0);
    SetResult(ctx, X_ERROR_IO_PENDING);
  } else {
    SetResult(ctx, 0);
  }
}

// XamContentGetCreator(user_index, content_data, BOOL* is_creator, XUID* creator, overlapped)
// The runtime reports saves as created by the offline profile; once the title
// sees the online XUID, report that instead so it keeps owning its saves.
extern "C" REX_FUNC(__imp__XamContentGetCreator) {
  const uint32_t is_creator_ptr = ctx.r5.u32;
  const uint32_t creator_ptr = ctx.r6.u32;
  CallOriginal("__imp__XamContentGetCreator", ctx, base);
  const auto s = rexglue_guide::live::Snapshot();
  if (!s.signed_in || !creator_ptr) return;
  if (const uint8_t* is_creator = Guest(is_creator_ptr); is_creator && load_and_swap<uint32_t>(is_creator)) {
    store_and_swap<uint64_t>(Guest(creator_ptr), s.me.xuid);
  }
}

namespace {

// XFriendsCreateEnumerator through XLiveBase (0xFC / 0x58020). buffer_length
// points at a marshaled argument list: user index, start index, count,
// DWORD* buffer size, HANDLE* enumerator.
uint32_t CreateFriendsEnumerator(uint32_t args_ptr) {
  using rex::kernel::xam::apps::X_CREATE_FRIENDS_ENUMERATOR;
  using rex::kernel::xam::apps::X_ONLINE_FRIEND;
  auto* args = reinterpret_cast<X_CREATE_FRIENDS_ENUMERATOR*>(Guest(args_ptr));
  if (!args) return X_E_INVALIDARG;
  auto arg32 = [](const auto& entry) {
    const uint8_t* value = Guest(uint32_t(entry.argument_value_ptr));
    return value ? load_and_swap<uint32_t>(value) : 0u;
  };
  const uint32_t start = arg32(args->friends_starting_index);
  const uint32_t amount = std::max(1u, arg32(args->friends_amount));
  uint8_t* size_out = Guest(uint32_t(args->buffer_ptr.argument_value_ptr));
  uint8_t* handle_out = Guest(uint32_t(args->handle_ptr.argument_value_ptr));
  if (!handle_out) return X_E_INVALIDARG;
  store_and_swap<uint32_t>(handle_out, 0);

  auto* kernel = REX_KERNEL_STATE();
  auto e = rex::system::object_ref<rex::system::XStaticUntypedEnumerator>(
      new rex::system::XStaticUntypedEnumerator(kernel, amount, sizeof(X_ONLINE_FRIEND)));
  if (XFAILED(e->Initialize(0xFF, 0xFC, 0x58021, 0x58022, 0))) return X_E_FAIL;

  const auto s = rexglue_guide::live::Snapshot();
  char title[9];
  std::snprintf(title, sizeof(title), "%08X", kernel->title_id());
  for (size_t i = start; i < s.friends.size() && i < start + amount; ++i) {
    const auto& f = s.friends[i];
    auto* out = reinterpret_cast<X_ONLINE_FRIEND*>(e->AppendItem());
    std::memset(out, 0, sizeof(*out));
    out->xuid = f.profile.xuid;
    std::snprintf(out->Gamertag, sizeof(out->Gamertag), "%s", f.profile.gamertag.c_str());
    uint32_t state = X_ONLINE_FRIENDSTATE_ENUM_CONSOLE_XBOX360;
    if (f.online) state |= X_ONLINE_FRIENDSTATE_FLAG_ONLINE;
    if (f.online && f.title_id == title) {
      state |= X_ONLINE_FRIENDSTATE_FLAG_PLAYING;
      // Hosting a session friends may join through presence: titles show
      // "join" (not just "invite") from the joinable flag and the session id,
      // and join with XSessionSearchByID on it.
      if (rexglue_guide::sessions::FriendJoinableSession(f.profile.xuid, out->session_id)) {
        state |= X_ONLINE_FRIENDSTATE_FLAG_JOINABLE;
      }
    }
    out->state = state;
    out->title_id = f.online ? uint32_t(std::strtoul(f.title_id.c_str(), nullptr, 16)) : 0;
    const std::string& presence = f.online ? f.status : std::string();
    const size_t n = std::min<size_t>(presence.size(), X_MAX_RICHPRESENCE_SIZE - 1);
    for (size_t c = 0; c < n; ++c) out->wszRichPresence[c] = char16_t(static_cast<unsigned char>(presence[c]));
    out->cchRichPresence = uint32_t(n);
  }
  if (size_out) store_and_swap<uint32_t>(size_out, uint32_t(e->items_per_enumerate() * e->item_size()));
  store_and_swap<uint32_t>(handle_out, e->handle());
  return X_E_SUCCESS;
}

// XTitleServerCreateEnumerator's list: the RexLiveless server, named to match
// what the title looks for. With a service mask (Viva Pinata TiP: "XLS.SS07R.*")
// it's that mask's name; the XDK writes the mask into the enumerator's private
// data after making it, so the names are made when the title enumerates.
// Without one (Nuts & Bolts asks for every server and matches them against its
// own "XLS.Express.*" itself) it's the server under each known title's prefix.
class TitleServerEnumerator : public rex::system::XEnumerator {
 public:
  static constexpr uint32_t kServerInfoSize = 208;  // XTITLE_SERVER_INFO: IN_ADDR, flags, char[200]
  static constexpr const char* kKnownMasks[] = {"XLS.SS07R.*", "XLS.Express.*"};

  TitleServerEnumerator(rex::system::KernelState* kernel, uint32_t items)
      : XEnumerator(kernel, items, kServerInfoSize), items_(items) {}

  void set_mask(const uint8_t* private_data) { mask_ = private_data; }

  // Listed whenever the player is signed in, even before the first sync has
  // told the Guide the server's title-server port: titles enumerate once, at
  // sign-in (Nuts & Bolts), and connect much later (XNetServerToInAddr, net.cpp).
  uint32_t WriteItems(uint32_t, uint8_t* buffer, uint32_t* written_count) override {
    if (done_ || !rexglue_guide::live::Snapshot().signed_in) return 18;  // ERROR_NO_MORE_FILES
    done_ = true;
    const std::string mask =
        mask_ ? std::string(reinterpret_cast<const char*>(mask_), strnlen(reinterpret_cast<const char*>(mask_), 199)) : "";
    std::vector<std::string> masks;
    if (mask.empty()) masks.assign(std::begin(kKnownMasks), std::end(kKnownMasks));
    else masks.push_back(mask);
    uint32_t count = 0;
    for (const std::string& m : masks) {
      if (count >= items_) break;
      const size_t star = m.find('*');
      const std::string name = star == std::string::npos ? m : m.substr(0, star) + "RexLiveless";
      uint8_t* item = buffer + count++ * kServerInfoSize;
      std::memset(item, 0, kServerInfoSize);
      store_and_swap<uint32_t>(item, 0xAC000001);  // any address; XNetServerToInAddr maps it
      std::snprintf(reinterpret_cast<char*>(item + 8), 200, "%s", name.c_str());
    }
    if (written_count) *written_count = count;
    return 0;
  }

 private:
  uint32_t items_;
  const uint8_t* mask_ = nullptr;
  bool done_ = false;
};

}  // namespace

// XamCreateEnumeratorHandle(user, app, open msg, close msg, extra size, items,
// flags, HANDLE*). The runtime has none; the one enumerator titles need from
// it is XTitleServerCreateEnumerator (XLiveBase 0x58039), which lists the
// title servers: the RexLiveless one, whatever the mask.
extern "C" REX_FUNC(__imp__XamCreateEnumeratorHandle) {
  const uint32_t user = ctx.r3.u32, app = ctx.r4.u32, open = ctx.r5.u32, close = ctx.r6.u32;
  const uint32_t extra = ctx.r7.u32, items = std::max(1u, ctx.r8.u32), flags = ctx.r9.u32;
  uint8_t* out = Guest(ctx.r10.u32);
  if (app != 0xFC || open != 0x58039 || !out) return CallOriginal("__imp__XamCreateEnumeratorHandle", ctx, base);
  GUIDE_TRACE("XTitleServerCreateEnumerator");
  auto e = rex::system::object_ref<TitleServerEnumerator>(new TitleServerEnumerator(REX_KERNEL_STATE(), items));
  void* private_data = nullptr;
  if (XFAILED(e->Initialize(user, app, open, close, flags, extra, &private_data))) return SetResult(ctx, X_E_FAIL);
  if (extra) e->set_mask(static_cast<const uint8_t*>(private_data));
  store_and_swap<uint32_t>(out, e->handle());
  SetResult(ctx, 0);
}

// XMsgInProcessCall(app, message, arg1, arg2)
extern "C" REX_FUNC(__imp__XMsgInProcessCall) {
  const uint32_t app = ctx.r3.u32;
  const uint32_t message = ctx.r4.u32;
  const auto s = rexglue_guide::live::Snapshot();
  if (app == 0xFC && s.signed_in) {
    switch (message) {
      case 0x00058003:  // XLiveBase logon HRESULT
        return SetResult(ctx, s.connected ? X_ONLINE_S_LOGON_CONNECTION_ESTABLISHED : X_ONLINE_S_LOGON_DISCONNECTED);
      case 0x00058023:  // XInviteGetAcceptedInfo
        return SetResult(ctx, rexglue_guide::sessions::FillAcceptedInvite(ctx.r6.u32) ? 0 : X_E_FAIL);
      case 0x00058020:  // XFriendsCreateEnumerator
        GUIDE_TRACE("XFriendsCreateEnumerator");
        return SetResult(ctx, CreateFriendsEnumerator(ctx.r6.u32));
      default:
        break;
    }
  }
  CallOriginal("__imp__XMsgInProcessCall", ctx, base);
}
