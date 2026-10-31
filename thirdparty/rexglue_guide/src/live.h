#pragma once
// State shared between the Guide (UI thread) and the XAM function overrides,
// which run on the game's threads.

#include <functional>

#include "backend.h"

namespace rexglue_guide::live {

constexpr int kMaxUsers = 4;

// A player's signed-in account and friends as of the last sync (user index
// 0-3, as the title numbers its players). Safe on any thread. LIVE sessions,
// stats and the title server go through player 1's (user 0).
LiveSnapshot Snapshot(int user = 0);

// Player 1 signed in and talking to the server: what titles see as "signed
// in to LIVE".
bool Online();

// Runs work on the UI thread with player 1's backend, next frame.
void Post(std::function<void(Backend&)> work);

// Tells titles a player's profile signed in or out (XN_SYS_SIGNINCHANGED).
void NotifySigninChanged();

// XShowGamerCardUI from the title: the Guide opens on that player's gamer
// card, next frame. Any thread.
void RequestGamercard(uint64_t xuid);

// Why leaderboard scores and replays can't be posted now (Config::
// leaderboard_block), empty when they can. Any thread.
std::string LeaderboardBlockReason();
// The same, telling the player when they can't (once in a while, not for
// every view written): for a score or replay about to be posted, which then
// fails.
std::string LeaderboardBlocked();

// The app's build name (Config::build), "" for none: sent with leaderboard
// scores and replays. Any thread.
const std::string& Build();

// Guide side.
void Attach(int user, Backend* backend);
void Pump();

}  // namespace rexglue_guide::live
