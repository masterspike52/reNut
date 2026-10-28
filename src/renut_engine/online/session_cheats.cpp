/**
 * @file    online/session_cheats.cpp
 * @brief   Cheats in online multiplayer, when the session's host allows them.
 *
 * Cheats are off in a session (hooks.cpp) unless its host allows them: the
 * host's renut_host_cheats. Only the host's counts, and only a host can turn
 * it on while in a session (a player in someone else's session cannot).
 *
 * The others learn it from the host on channel 3 (eos.h): every couple of
 * seconds a player in a session it doesn't host asks the session's host
 * (online.h SessionHostId), and the host answers with its setting. An answer
 * counts only from the session's host; a new host or the session's end
 * forgets it (sessions.cpp), so cheats stay off until the new host answers.
 */
#include <rex/cvar.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "renut_engine/online/eos.h"
#include "renut_engine/online/online.h"
#include "renut_engine/online/online_log.h"

namespace {

bool HostCheatsValueAllowed(std::string_view value);

}  // namespace

REXCVAR_DEFINE_BOOL(renut_host_cheats, false, "Nuts&Bolts/Cheats",
                    "Online multiplayer sessions you host allow cheats for every player. Only the "
                    "host's setting counts; it cannot be turned on in a session someone else hosts.")
    .validator(HostCheatsValueAllowed);

namespace {

using namespace renut::online;

// Messages: "RC", then the kind, then (an answer) whether cheats are allowed.
constexpr uint8_t kTag0 = 'R';
constexpr uint8_t kTag1 = 'C';
constexpr uint8_t kAsk = 1;
constexpr uint8_t kAnswer = 2;

// What the session's host last answered.
std::atomic<bool> g_host_allows{false};
std::atomic<bool> g_answered{false};

bool HostCheatsValueAllowed(std::string_view value) {
  return (value != "true" && value != "1") || CanAllowSessionCheats();
}

void SendKind(const std::string &id, uint8_t kind, bool allowed) {
  const uint8_t message[4] = {kTag0, kTag1, kind, uint8_t(allowed ? 1 : 0)};
  eos::Send(id, eos::kChannelSession, message, sizeof(message), true);
}

// EOS thread.
void OnMessage(const std::string &id, const uint8_t *data, size_t size) {
  if (size < 3 || data[0] != kTag0 || data[1] != kTag1)
    return;
  if (data[2] == kAsk) {
    // Answered only by the session's host.
    if (HostingSession())
      SendKind(id, kAnswer, REXCVAR_GET(renut_host_cheats));
    return;
  }
  if (data[2] != kAnswer || size < 4 || HostingSession() || id != SessionHostId())
    return;
  const bool allowed = data[3] != 0;
  const bool before = g_host_allows.exchange(allowed);
  if (!g_answered.exchange(true) || before != allowed)
    RONL_INFO("Online: the session's host {} cheats", allowed ? "allows" : "does not allow");
}

// EOS thread, every couple of seconds: asks the session's host.
void Tick() {
  if (!InSession() || HostingSession())
    return;
  const std::string host = SessionHostId();
  if (!host.empty())
    SendKind(host, kAsk, false);
}

[[maybe_unused]] const bool g_registered = [] {
  eos::SetReceiver(eos::kChannelSession, &OnMessage);
  eos::AddTicker(&Tick);
  return true;
}();

}  // namespace

bool renut::online::SessionAllowsCheats() {
  if (!InSession())
    return false;
  if (HostingSession())
    return REXCVAR_GET(renut_host_cheats);
  return g_host_allows.load();
}

bool renut::online::CanAllowSessionCheats() {
  return !InSession() || HostingSession();
}

void renut::online::ForgetSessionCheats() {
  g_host_allows = false;
  g_answered = false;
}
