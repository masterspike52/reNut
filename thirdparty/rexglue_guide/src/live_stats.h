#pragma once
// Xbox LIVE stats and leaderboards, backed by RexLiveless (live_stats.cpp).

#include <cstdint>
#include <string>
#include <vector>

namespace rexglue_guide::stats {

// XGI messages (app 0xFB) with their marshaled buffers; both return a
// Win32 error code.
uint32_t WriteStats(const uint8_t* buf);  // XSessionWriteStats (0x000B0025)
uint32_t ReadStats(const uint8_t* buf);   // XUserReadStats (0x000B0021)

// The running title's leaderboards that have a name in its SPA, in its order.
struct Leaderboard {
  uint32_t id;
  std::string name;
};
std::vector<Leaderboard> Leaderboards();

struct BoardRow {
  uint32_t rank = 0;
  uint64_t xuid = 0;
  std::string gamertag;
  int64_t rating = 0;
};
struct Board {
  bool loading = true;
  bool failed = false;
  uint32_t total = 0;
  std::vector<BoardRow> rows;  // the top rows, then the player's own if lower
};
// A leaderboard as last read from the server. Reads it in the background when
// it's missing or older than a few seconds, so call it every frame.
Board TopRows(uint32_t view_id);

}  // namespace rexglue_guide::stats
