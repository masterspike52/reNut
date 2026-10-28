// Xbox LIVE stats and leaderboards, backed by RexLiveless.
//
// A title writes stats as properties per view (XSessionWriteStats) and reads
// them back as columns (XUserReadStats, XUserCreateStatsEnumeratorBy*). Which
// property feeds which column, and how it aggregates, is in the title's SPA
// (the XVC2 table, compiled from its XLAST): the Guide maps properties to
// columns from it and the server keeps one row per player per view.
//
// Special columns: 0xFFFF rank, 0xFFFE rating (orders the view), 0xFFFD gamer
// name, 0xFFFA attachment size.

#include "live_stats.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xenumerator.h>

#include "guest.h"
#include "http.h"
#include "live.h"

using namespace rexglue_guide::guest;
using json = nlohmann::json;

namespace rexglue_guide::stats {
namespace {

constexpr uint32_t kSuccess = 0;
constexpr uint32_t kNoMoreFiles = 18;

constexpr uint16_t kColumnRank = 0xFFFF;
constexpr uint16_t kColumnRating = 0xFFFE;
constexpr uint16_t kColumnGamerName = 0xFFFD;
constexpr uint16_t kColumnAttachmentSize = 0xFFFA;

// XUSER_DATA types.
constexpr uint8_t kTypeInt32 = 1, kTypeInt64 = 2, kTypeDouble = 3, kTypeUnicode = 4, kTypeFloat = 5,
                  kTypeBinary = 6, kTypeDateTime = 7, kTypeNull = 0xFF;

// Guest structure sizes.
constexpr uint32_t kSpecSize = 0x88;      // XUSER_STATS_SPEC: view, count, WORD columns[64]
constexpr uint32_t kSpecMaxColumns = 64;
constexpr uint32_t kResultsSize = 8;      // XUSER_STATS_READ_RESULTS
constexpr uint32_t kViewSize = 0x10;      // XUSER_STATS_VIEW
constexpr uint32_t kRowSize = 0x30;       // XUSER_STATS_ROW
constexpr uint32_t kColumnSize = 0x18;    // XUSER_STATS_COLUMN
constexpr uint32_t kPropertySize = 0x18;  // XUSER_PROPERTY
constexpr uint32_t kStringBudget = 64;    // bytes per string column in an enumerator's buffer

// ------------------------------------------------------------ SPA (XVC2) --

struct Field {
  uint32_t property;
  uint16_t attr;
  uint16_t aggregation;
};

std::once_flag g_schema_once;
std::map<uint32_t, std::vector<Field>> g_schema;  // view id -> columns
std::vector<Leaderboard> g_named;                 // views with a display name

// XVC2: {magic, version, size}, u16 shared view count, the shared views (u16
// columns, u16 rows, 8 bytes, then 0x20-byte fields, then an XPBM property
// list), u16 view count, the views (u32 id, u32 flags, u16 shared view, u16
// string id, u32).
void LoadSchema() {
  constexpr uint64_t kXvc2 = 0x58564332;
  const auto xdbf = REX_KERNEL_STATE()->title_xdbf();
  if (!xdbf.is_valid()) return;
  const auto block = xdbf.GetEntry(rex::system::util::XdbfSection::kMetadata, kXvc2);
  if (!block || block.size < 14 || Load32(block.buffer) != kXvc2) return;
  const uint8_t* p = block.buffer;
  const uint8_t* end = block.buffer + block.size;
  auto have = [&](size_t n) { return size_t(end - p) >= n; };

  p += 12;
  std::vector<std::vector<Field>> shared(Load16(p));
  p += 2;
  for (auto& columns : shared) {
    if (!have(12)) return;
    const uint32_t num_columns = Load16(p), num_rows = Load16(p + 2);
    p += 12;
    for (uint32_t f = 0; f < num_columns + num_rows; ++f, p += 0x20) {
      if (!have(0x20)) return;
      if (f < num_columns) columns.push_back({Load32(p + 4), Load16(p + 0xC), Load16(p + 0x10)});
    }
    if (!have(20) || Load32(p) != 0x5850424D) return;  // XPBM
    const uint32_t properties = Load32(p + 16);
    if (!have(20 + size_t(properties) * 4)) return;
    p += 20 + properties * 4;
  }
  if (!have(2)) return;
  const uint32_t num_views = Load16(p);
  p += 2;
  for (uint32_t v = 0; v < num_views && have(16); ++v, p += 16) {
    const uint32_t id = Load32(p);
    const uint32_t shared_index = Load16(p + 8);
    if (shared_index < shared.size()) g_schema[id] = shared[shared_index];
    // Unused views keep the XLAST editor's "<String text>" placeholder.
    std::string name = xdbf.GetStringTableEntry(xdbf.default_language(), Load16(p + 10));
    if ((id & 0xFFFF0000) != 0xFFFF0000 && !name.empty() && name[0] != '<') g_named.push_back({id, std::move(name)});
  }
  REXKRNL_INFO("RexLiveless: {} leaderboard view(s) in the title's SPA", g_schema.size());
}

const std::vector<Field>* ViewColumns(uint32_t view_id) {
  std::call_once(g_schema_once, LoadSchema);
  auto it = g_schema.find(view_id);
  return it == g_schema.end() ? nullptr : &it->second;
}

// XVC2 aggregation codes seen so far; anything else keeps the last value
// written (the best one, for the rating), and is logged once.
const char* Aggregation(const Field& f) {
  switch (f.aggregation) {
    case 0x8001: return "last";
    case 0x800B: return "max";
  }
  static std::once_flag logged;
  std::call_once(logged, [&] {
    REXKRNL_WARN("RexLiveless: unknown stats aggregation {:04X} (property {:08X})", f.aggregation, f.property);
  });
  return f.attr == kColumnRating ? "max" : "last";
}

// ---------------------------------------------------------------- server --

std::string Hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789ABCDEF";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += d[p[i] >> 4];
    s += d[p[i] & 15];
  }
  return s;
}

std::vector<uint8_t> Unhex(const std::string& s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
  return out;
}

std::string HexId(uint64_t v, int digits) {
  char t[17];
  std::snprintf(t, sizeof(t), "%0*llX", digits, static_cast<unsigned long long>(v));
  return t;
}

http::Response Api(const std::string& path, const json& body) {
  const LiveSnapshot s = live::Snapshot();
  if (!s.signed_in || s.token.empty()) return {};
  http::Endpoint endpoint{s.server_host, s.server_port, s.server_tls};
  return http::Request(endpoint, "POST", path, body.dump(), s.token, "application/json", 5000);
}

// The views of a read, in the order asked for; empty on failure.
json Read(json query) {
  const auto r = Api("/api/leaderboards/read", query);
  if (r.status != 200) {
    REXKRNL_WARN("RexLiveless: couldn't read leaderboards ({} {})", r.status, r.error);
    return json::array();
  }
  try {
    return json::parse(r.body).value("views", json::array());
  } catch (...) {
    return json::array();
  }
}

// --------------------------------------------------------------- writing --

// One XUSER_PROPERTY as a server column value: {type, value} or {type, data}.
json PropertyValue(const uint8_t* prop) {
  const uint8_t type = prop[8];
  const uint8_t* v = prop + 0x10;
  json out = {{"type", type}, {"value", 0}, {"data", ""}};
  switch (type) {
    case kTypeInt32: out["value"] = int32_t(Load32(v)); break;
    case kTypeInt64:
    case kTypeDateTime: out["value"] = int64_t(Load64(v)); break;
    case kTypeDouble: out["value"] = std::bit_cast<double>(Load64(v)); break;
    case kTypeFloat: out["value"] = double(std::bit_cast<float>(Load32(v))); break;
    case kTypeUnicode:
    case kTypeBinary:
      if (const uint8_t* data = At(Load32(v + 4))) out["data"] = Hex(data, std::min<uint32_t>(Load32(v), 1024));
      break;
  }
  return out;
}

}  // namespace

// XSessionWriteStats: {session, XUID (unaligned at +4), view count,
// XSESSION_VIEW_PROPERTIES* (view id, property count, XUSER_PROPERTY*)}.
uint32_t WriteStats(const uint8_t* buf) {
  const LiveSnapshot s = live::Snapshot();
  const uint64_t xuid = Load64(buf + 4);
  if (xuid != s.me.xuid) {
    // Only the signed-in player's own stats go to the server.
    REXKRNL_INFO("RexLiveless: XSessionWriteStats for {:016X} skipped (not this player)", xuid);
    return kSuccess;
  }
  const uint32_t num_views = Load32(buf + 12);
  const uint8_t* views = At(Load32(buf + 16));
  json out = json::array();
  for (uint32_t v = 0; views && v < num_views && v < 64; ++v) {
    const uint8_t* view = views + v * 12;
    const uint32_t view_id = Load32(view);
    const uint8_t* props = At(Load32(view + 8));
    const auto* columns = ViewColumns(view_id);
    if (!columns || !props) {
      REXKRNL_INFO("RexLiveless: stats view {:08X} isn't in the title's SPA; not written", view_id);
      continue;
    }
    json list = json::array();
    for (uint32_t i = 0; i < Load32(view + 4) && i < 64; ++i) {
      const uint8_t* prop = props + i * kPropertySize;
      for (const Field& f : *columns) {
        if (f.property != Load32(prop) || f.attr == kColumnRank || f.attr == kColumnGamerName ||
            f.attr == kColumnAttachmentSize) {
          continue;
        }
        json c = PropertyValue(prop);
        c["attr"] = f.attr;
        c["op"] = Aggregation(f);
        list.push_back(std::move(c));
      }
    }
    if (!list.empty()) out.push_back({{"id", view_id}, {"columns", std::move(list)}});
  }
  if (out.empty()) return kSuccess;
  json body = {{"title_id", HexId(REX_KERNEL_STATE()->title_id(), 8)}, {"views", std::move(out)}};
  std::thread([body = std::move(body)] {
    const auto r = Api("/api/leaderboards/write", body);
    if (r.status != 200) REXKRNL_WARN("RexLiveless: couldn't write stats ({} {})", r.status, r.error);
  }).detach();
  return kSuccess;
}

namespace {

// --------------------------------------------------------------- reading --

struct Spec {
  uint32_t view;
  std::vector<uint16_t> columns;
};

std::vector<Spec> ReadSpecs(uint32_t specs_ptr, uint32_t count) {
  std::vector<Spec> specs;
  const uint8_t* p = At(specs_ptr);
  for (uint32_t i = 0; p && i < count && i < 64; ++i, p += kSpecSize) {
    Spec s{Load32(p), {}};
    for (uint32_t c = 0; c < Load32(p + 4) && c < kSpecMaxColumns; ++c) s.columns.push_back(Load16(p + 8 + c * 2));
    specs.push_back(std::move(s));
  }
  return specs;
}

bool IsStringColumn(uint32_t view, uint16_t attr) {
  if (attr == kColumnGamerName) return true;
  if (const auto* columns = ViewColumns(view)) {
    for (const Field& f : *columns) {
      if (f.attr == attr) return (f.property >> 28) == kTypeUnicode || (f.property >> 28) == kTypeBinary;
    }
  }
  return false;
}

// Guest memory for strings: after the results while the buffer lasts, then
// the system heap.
struct Space {
  uint32_t next, end;
  uint32_t Alloc(uint32_t size) {
    size = (size + 7) & ~7u;
    if (next + size <= end) {
      const uint32_t p = next;
      next += size;
      return p;
    }
    return REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
  }
};

void WriteData(uint8_t* value, Space& space, const std::vector<uint8_t>& data) {
  const uint32_t ptr = data.empty() ? 0 : space.Alloc(uint32_t(data.size()));
  if (uint8_t* dst = At(ptr)) std::memcpy(dst, data.data(), data.size());
  Store32(value, ptr ? uint32_t(data.size()) : 0);
  Store32(value + 4, ptr);
}

// One XUSER_STATS_COLUMN: {WORD id, XUSER_DATA (type at +8, value at +0x10)}.
void WriteColumn(uint8_t* col, Space& space, uint32_t view, uint16_t attr, const json& row) {
  std::memset(col, 0, kColumnSize);
  Store16(col, attr);
  uint8_t* value = col + 0x10;
  if (attr == kColumnRank) {
    col[8] = kTypeInt32;
    Store32(value, row.value("rank", 0u));
    return;
  }
  if (attr == kColumnGamerName) {
    col[8] = kTypeUnicode;
    std::vector<uint8_t> text;
    for (char c : row.value("gamertag", std::string())) text.insert(text.end(), {0, uint8_t(c)});
    text.insert(text.end(), {0, 0});
    WriteData(value, space, text);
    return;
  }
  const json columns = row.value("columns", json::object());
  const std::string key = std::to_string(attr);
  if (!columns.contains(key)) {
    // No value yet: the column's type with nothing in it.
    col[8] = kTypeNull;
    if (const auto* fields = ViewColumns(view)) {
      for (const Field& f : *fields) {
        if (f.attr == attr) col[8] = uint8_t(f.property >> 28);
      }
    }
    if (attr == kColumnRating && col[8] == kTypeNull) col[8] = kTypeInt64;
    return;
  }
  const json& c = columns[key];
  const uint8_t type = uint8_t(c.value("type", 0));
  const json number = c.value("value", json(0));
  col[8] = type;
  switch (type) {
    case kTypeInt32: Store32(value, uint32_t(number.get<int32_t>())); break;
    case kTypeInt64:
    case kTypeDateTime: Store64(value, uint64_t(number.get<int64_t>())); break;
    case kTypeDouble: Store64(value, std::bit_cast<uint64_t>(number.get<double>())); break;
    case kTypeFloat: Store32(value, std::bit_cast<uint32_t>(float(number.get<double>()))); break;
    case kTypeUnicode:
    case kTypeBinary: WriteData(value, space, Unhex(c.value("data", std::string()))); break;
  }
}

// Lays out XUSER_STATS_READ_RESULTS at out_ptr: header, the views, their
// rows, their columns, then strings. `views` are the server's, one per spec
// in order. Returns the number of rows written.
uint32_t WriteResults(uint32_t out_ptr, uint32_t out_size, const std::vector<Spec>& specs, const json& views) {
  uint8_t* out = At(out_ptr);
  const uint32_t views_ptr = out_ptr + kResultsSize;
  uint32_t next = views_ptr + uint32_t(specs.size()) * kViewSize;
  uint32_t total_rows = 0;
  std::vector<uint32_t> rows_ptrs;
  for (size_t v = 0; v < specs.size(); ++v) {
    const size_t rows = v < views.size() ? views[v].value("rows", json::array()).size() : 0;
    rows_ptrs.push_back(next);
    next += uint32_t(rows) * (kRowSize + uint32_t(specs[v].columns.size()) * kColumnSize);
  }
  Space space{next, out_ptr + out_size};
  Store32(out, uint32_t(specs.size()));
  Store32(out + 4, specs.empty() ? 0 : views_ptr);
  for (size_t v = 0; v < specs.size(); ++v) {
    const Spec& spec = specs[v];
    const json rows = v < views.size() ? views[v].value("rows", json::array()) : json::array();
    uint8_t* view = At(views_ptr + uint32_t(v) * kViewSize);
    Store32(view, spec.view);
    Store32(view + 4, v < views.size() ? views[v].value("total", 0u) : 0u);
    Store32(view + 8, uint32_t(rows.size()));
    Store32(view + 0xC, rows.empty() ? 0 : rows_ptrs[v]);
    const uint32_t columns_ptr = rows_ptrs[v] + uint32_t(rows.size()) * kRowSize;
    for (size_t r = 0; r < rows.size(); ++r) {
      const json& row = rows[r];
      uint8_t* dst = At(rows_ptrs[v] + uint32_t(r) * kRowSize);
      std::memset(dst, 0, kRowSize);
      Store64(dst, std::strtoull(row.value("xuid", std::string("0")).c_str(), nullptr, 16));
      Store32(dst + 8, row.value("rank", 0u));
      Store64(dst + 0x10, uint64_t(int64_t(row.value("rating", 0.0))));
      const std::string gamertag = row.value("gamertag", std::string());
      std::memcpy(dst + 0x18, gamertag.data(), std::min<size_t>(gamertag.size(), 15));
      const uint32_t row_columns = columns_ptr + uint32_t(r * spec.columns.size()) * kColumnSize;
      Store32(dst + 0x28, uint32_t(spec.columns.size()));
      Store32(dst + 0x2C, spec.columns.empty() ? 0 : row_columns);
      for (size_t c = 0; c < spec.columns.size(); ++c) {
        WriteColumn(At(row_columns + uint32_t(c) * kColumnSize), space, spec.view, spec.columns[c], row);
      }
    }
    total_rows += uint32_t(rows.size());
  }
  return total_rows;
}

json ViewIds(const std::vector<Spec>& specs) {
  json ids = json::array();
  for (const Spec& s : specs) ids.push_back(s.view);
  return ids;
}

std::string Gamertag(const LiveSnapshot& s, uint64_t xuid) {
  if (xuid == s.me.xuid) return s.me.gamertag;
  for (const auto& f : s.friends) {
    if (f.profile.xuid == xuid) return f.profile.gamertag;
  }
  return {};
}

}  // namespace

// XUserReadStats: {title id, XUID count, XUID*, spec count, XUSER_STATS_SPEC*,
// results buffer size, XUSER_STATS_READ_RESULTS*}. The title has already
// checked the buffer is big enough for every player having a row. Players
// without one come back unranked with empty columns.
uint32_t ReadStats(const uint8_t* buf) {
  const uint32_t title_id = Load32(buf);
  const uint32_t num_xuids = std::min<uint32_t>(Load32(buf + 4), 100);
  const uint8_t* xuids = At(Load32(buf + 8));
  const auto specs = ReadSpecs(Load32(buf + 16), Load32(buf + 12));
  const uint32_t out_size = Load32(buf + 20);
  const uint32_t out_ptr = Load32(buf + 24);
  if (!At(out_ptr) || !xuids) return kSuccess;

  std::vector<uint64_t> players;
  json hex = json::array();
  for (uint32_t i = 0; i < num_xuids; ++i) {
    players.push_back(Load64(xuids + i * 8));
    hex.push_back(HexId(players.back(), 16));
  }
  const json found = Read({{"title_id", HexId(title_id, 8)}, {"views", ViewIds(specs)}, {"by", "players"}, {"xuids", hex}});

  // A row per player, in the order asked, whether or not they're ranked.
  const LiveSnapshot s = live::Snapshot();
  json views = json::array();
  for (size_t v = 0; v < specs.size(); ++v) {
    const json view = v < found.size() ? found[v] : json::object();
    json rows = json::array();
    for (uint64_t xuid : players) {
      json row = {{"xuid", HexId(xuid, 16)}, {"gamertag", Gamertag(s, xuid)}, {"rank", 0}, {"rating", 0}};
      for (const json& r : view.value("rows", json::array())) {
        if (std::strtoull(r.value("xuid", std::string("0")).c_str(), nullptr, 16) == xuid) row = r;
      }
      rows.push_back(std::move(row));
    }
    views.push_back({{"total", view.value("total", 0u)}, {"rows", std::move(rows)}});
  }
  WriteResults(out_ptr, out_size, specs, views);
  REXKRNL_INFO("RexLiveless: XUserReadStats {} view(s), {} player(s)", specs.size(), players.size());
  return kSuccess;
}

namespace {

// A stats enumerator: one XEnumerate returns the whole XUSER_STATS_READ_RESULTS
// (every view asked for), read from the server when it's called.
class StatsEnumerator : public rex::system::XEnumerator {
 public:
  StatsEnumerator(rex::system::KernelState* kernel, uint32_t size, json query, std::vector<Spec> specs)
      : XEnumerator(kernel, 1, size), size_(size), query_(std::move(query)), specs_(std::move(specs)) {}

  uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer_data, uint32_t* written_count) override {
    if (done_) return kNoMoreFiles;
    done_ = true;
    std::memset(buffer_data, 0, size_);
    const uint32_t rows = WriteResults(buffer_ptr, size_, specs_, Read(query_));
    if (written_count) *written_count = rows;
    return kSuccess;
  }

 private:
  uint32_t size_;
  json query_;
  std::vector<Spec> specs_;
  bool done_ = false;
};

// ------------------------------------------------- the Guide's leaderboards --

constexpr uint32_t kBoardRows = 100;
constexpr auto kBoardRefresh = std::chrono::seconds(10);

struct CachedBoard {
  Board board;
  std::chrono::steady_clock::time_point read_at;
  bool reading = false;
};
std::mutex g_boards_mutex;
std::map<uint32_t, CachedBoard> g_boards;

BoardRow ParseRow(const json& r) {
  return {r.value("rank", 0u), std::strtoull(r.value("xuid", std::string("0")).c_str(), nullptr, 16),
          r.value("gamertag", std::string()), int64_t(r.value("rating", 0.0))};
}

void ReadBoard(uint32_t view_id) {
  const std::string title = HexId(REX_KERNEL_STATE()->title_id(), 8);
  Board board;
  board.loading = false;
  const json top = Read({{"title_id", title}, {"views", json::array({view_id})}, {"by", "rank"}, {"start", 1}, {"count", kBoardRows}});
  if (top.empty()) {
    board.failed = true;
  } else {
    board.total = top[0].value("total", 0u);
    for (const json& r : top[0].value("rows", json::array())) board.rows.push_back(ParseRow(r));
    // The player's own row when it's below the top ones.
    const uint64_t me = live::Snapshot().me.xuid;
    const bool listed = std::any_of(board.rows.begin(), board.rows.end(), [&](const BoardRow& r) { return r.xuid == me; });
    if (me && !listed && board.total > board.rows.size()) {
      const json mine = Read({{"title_id", title}, {"views", json::array({view_id})}, {"by", "players"}, {"xuids", json::array({HexId(me, 16)})}});
      if (!mine.empty()) {
        for (const json& r : mine[0].value("rows", json::array())) board.rows.push_back(ParseRow(r));
      }
    }
  }
  std::lock_guard lock(g_boards_mutex);
  CachedBoard& cached = g_boards[view_id];
  cached.board = std::move(board);
  cached.read_at = std::chrono::steady_clock::now();
  cached.reading = false;
}

}  // namespace

std::vector<Leaderboard> Leaderboards() {
  std::call_once(g_schema_once, LoadSchema);
  return g_named;
}

Board TopRows(uint32_t view_id) {
  std::lock_guard lock(g_boards_mutex);
  auto [it, added] = g_boards.try_emplace(view_id);
  CachedBoard& cached = it->second;
  if (!cached.reading && (added || std::chrono::steady_clock::now() - cached.read_at >= kBoardRefresh)) {
    cached.reading = true;
    std::thread(ReadBoard, view_id).detach();
  }
  return cached.board;
}

}  // namespace rexglue_guide::stats

using namespace rexglue_guide::stats;

// XamUserCreateStatsEnumerator(title id, kind, pivot, rows, spec count,
// XUSER_STATS_SPEC*, DWORD* buffer size, HANDLE*): behind
// XUserCreateStatsEnumeratorByXuid (kind 0, pivot XUID), ByRank (1, first
// rank) and ByRating (2, rating). The runtime's is a stub.
extern "C" REX_FUNC(__imp__XamUserCreateStatsEnumerator) {
  const rexglue_guide::LiveSnapshot s = rexglue_guide::live::Snapshot();
  uint8_t* size_out = At(ctx.r9.u32);
  uint8_t* handle_out = At(ctx.r10.u32);
  if (!s.signed_in || !handle_out) return CallOriginal("__imp__XamUserCreateStatsEnumerator", ctx, base);
  const uint32_t title_id = ctx.r3.u32, kind = ctx.r4.u32;
  const uint64_t pivot = ctx.r5.u64;
  const uint32_t num_rows = std::min<uint32_t>(ctx.r6.u32, 100);
  auto specs = ReadSpecs(ctx.r8.u32, ctx.r7.u32);

  json query = {{"title_id", HexId(title_id, 8)}, {"views", ViewIds(specs)}, {"count", num_rows}};
  switch (kind) {
    case 0: query["by"] = "xuid"; query["pivot"] = HexId(pivot, 16); break;
    case 2: query["by"] = "rating"; query["pivot"] = int64_t(pivot); break;
    default: query["by"] = "rank"; query["start"] = uint32_t(pivot); break;
  }

  // Room for every view full, strings included.
  uint32_t size = kResultsSize + uint32_t(specs.size()) * kViewSize;
  for (const Spec& spec : specs) {
    uint32_t strings = 0;
    for (uint16_t c : spec.columns) strings += IsStringColumn(spec.view, c) ? kStringBudget : 0;
    size += num_rows * (kRowSize + uint32_t(spec.columns.size()) * kColumnSize + strings);
  }

  auto e = rex::system::object_ref<StatsEnumerator>(
      new StatsEnumerator(REX_KERNEL_STATE(), size, std::move(query), std::move(specs)));
  if (XFAILED(e->Initialize(0, 0xFB, 0, 0, 0))) return Return(ctx, 0x80004005);
  if (size_out) Store32(size_out, size);
  Store32(handle_out, e->handle());
  Return(ctx, kSuccess);
}
