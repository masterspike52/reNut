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

// Guide side.
void Attach(int user, Backend* backend);
void Pump();

}  // namespace rexglue_guide::live
