#include <rex/hook.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include "rex_macros.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_MSC_VER)
#include <stdlib.h>
#define RENUT_BSWAP32(x) _byteswap_ulong(x)
#else
#define RENUT_BSWAP32(x) __builtin_bswap32(x)
#endif

// -----------------------------------------------------------------------------
// Guest globals / constants
// -----------------------------------------------------------------------------
static constexpr uint32_t kItemCount = 0x82FA32F4;  // dword_82FA32F4 (content row count, BE)

// XUI item struct offsets (from sub_823E4A38): +8 = wide text buffer,
// +268 = flag, +272 = wcslen.
static constexpr uint32_t kItemTextOff = 8;
static constexpr uint32_t kItemFlagOff = 268;
static constexpr uint32_t kItemLenOff = 272;

// Pause-menu object field offsets used by the dispatch guard (from sub_825A8D68).
static constexpr uint32_t kMenuListOff = 284;   // *(a1+284) = list container handle
static constexpr uint32_t kMenuList2Off = 248;  // *(a1+248) = alternate source handle
// Dpad nav message sources (from sub_825A76E0 / sub_825A6DB8 "leftButton"/"rightButton").
static constexpr uint32_t kPauseLeftMsgOff = 264;   // *(a1+264) = dpad-left ("leftButton")
static constexpr uint32_t kPauseRightMsgOff = 268;  // *(a1+268) = dpad-right ("rightButton")

// ---- Section (tab strip) --------------------------------------------------
// The pause screen is one XUI scene with a data-driven tab strip:
//   dword_82FA32C4[] = visible section ids, dword_82FA32C0 = count
//   sub_825A6708 builds the strip; sub_825A8110 sets each tab's icon
//   (off_82E51538[2*id]); sub_825A7CF0 sets the selected section's title text
//   (off_82E5153C[2*id]); sub_825A7D90(obj,mode) switches sections (mode = id,
//   jump table bounded mode<=7).
// We add a synthetic section id 8 = "reNut Settings" whose content is the cvar
// list. The game only ever uses ids 0..7.
static constexpr uint32_t kSectionArray = 0x82FA32C4;  // dword_82FA32C4[] (BE)
static constexpr uint32_t kSectionCount = 0x82FA32C0;  // dword_82FA32C0   (BE)
static constexpr uint32_t kGuestSectionMax = 12;       // room before dword_82FA32F4
static constexpr uint32_t kSectionRenut = 8;           // our synthetic section id
static constexpr uint32_t kSectionTitleElemOff = 196;  // *(a1+196) = title text element
static constexpr uint32_t kPauseModeField = 440;       // *(a1+0x1B8) = active section/mode
// Borrow the icon of section 3 ("pausemenutownmap"). The town-map section is
// never added to the pause strip, so that texture is otherwise unused - the
// rexglue texture replacer can swap it to give our tab a custom icon without
// affecting anything else.
static constexpr uint32_t kSectionIconBorrow = 3;

// -----------------------------------------------------------------------------
// Originals + recompiled helpers we call back into
// -----------------------------------------------------------------------------
REX_EXTERN(__imp__sub_825A96C8);  // original item label callback
REX_EXTERN(__imp__sub_825A8D68);  // original dispatch callback
REX_EXTERN(__imp__sub_825A7CF0);  // original section title-text setter
REX_EXTERN(__imp__sub_825A8110);  // original tab icon setter
REX_EXTERN(__imp__sub_825A7D90);  // original section switch (setMode)
REX_EXTERN(__imp__sub_825A76E0);  // original pause input handler

// Rebuild the pause list (re-runs the builder, which re-fires our injection hook).
REX_IMPORT(sub_825A89B0, RenutRebuildPauseMenu, void(uint32_t));
// Query the currently selected row index of a list container.
REX_IMPORT(sub_8257AD00, RenutGetCurSel, uint32_t(uint32_t));
// Set an XUI text element's text (element handle, guest wide-string ptr).
REX_IMPORT(sub_829C87A0, RenutSetTitleText, uint32_t(uint32_t, uint32_t));

// -----------------------------------------------------------------------------
// Host-side menu state
// -----------------------------------------------------------------------------
using rex::cvar::FlagType;

struct CvarRow {
  std::string name;
  FlagType type;
  std::vector<std::string> allowed;
  std::optional<double> min;
  std::optional<double> max;
};

// One collapsible category (e.g. "Cheats", "Graphics").
struct CvarCategory {
  std::string display;  // friendly name (text after the last '/')
  bool expanded = true;
  std::vector<CvarRow> cvars;
};

// A flattened display row: either a category header or a cvar under it.
enum class RowKind { Header, Cvar };
struct DisplayRow {
  RowKind kind;
  size_t cat;   // index into g_cats
  size_t cvar;  // index into g_cats[cat].cvars (Cvar rows only)
};

static bool g_cvarMenuOpen = false;         // the pause content list is showing cvars
static std::vector<CvarCategory> g_cats;    // categorized snapshot (stable while open)
static std::vector<DisplayRow> g_rows;      // flattened visible rows (per expand state)
// Per-row guest item-object pointer, captured by the label callback. Lets a
// value change rewrite that row's text buffer directly (no list rebuild -> no
// cursor/scroll movement). Reset on every rebuild; refilled as rows are labeled.
static std::vector<uint32_t> g_rowItemObj;

// One-shot: row index to re-select after the next rebuild (-1 = top). Lets a
// toggle / expand keep the cursor on the row instead of jumping back to the top.
static int g_restoreSel = -1;

// Deferred rebuild: a content-row select (toggle / collapse) can change the row
// count, but rebuilding the list (delete+insert XUI messages) WHILE the list
// widget is still dispatching that select re-enters XUI and corrupts its item
// bounds. So we flag the rebuild here and apply it from the once-per-frame tick,
// outside any XUI event dispatch.
static bool g_pendingRebuild = false;
static uint32_t g_pendingRebuildObj = 0;

// Friendly category name = the text after the last '/'
// (e.g. "Nuts&Bolts/Cheats" -> "Cheats").
static std::string PrettyCategory(const std::string& category) {
  size_t slash = category.rfind('/');
  std::string name = (slash == std::string::npos) ? category : category.substr(slash + 1);
  return name.empty() ? "General" : name;
}

// Flatten g_cats into g_rows honoring each category's expanded state.
static void RebuildRows() {
  g_rows.clear();
  for (size_t c = 0; c < g_cats.size(); ++c) {
    g_rows.push_back({RowKind::Header, c, 0});
    if (g_cats[c].expanded) {
      for (size_t v = 0; v < g_cats[c].cvars.size(); ++v)
        g_rows.push_back({RowKind::Cvar, c, v});
    }
  }
  // Item objects from the old layout are stale; the label callback refills these.
  g_rowItemObj.assign(g_rows.size(), 0);
}

// Snapshot the editable cvars grouped by category, preserving first-seen order.
// Called when the section opens so ordering stays fixed for the duration.
static void SnapshotCvars() {
  g_cats.clear();
  for (const auto& e : rex::cvar::GetRegistry()) {
    if (e.category.rfind("Nuts&Bolts", 0) != 0)
      continue;  // only Nuts&Bolts cvars belong in the in-game section (rest: F4)
    // Commands (e.g. StopNSwap) are kept: they render as a clickable "> name"
    // row that runs the command on A-press (see CvarRowLabel / the dispatch).
    CvarRow row;
    row.name = e.name;
    row.type = e.type;
    row.allowed = e.constraints.allowed_values;
    row.min = e.constraints.min;
    row.max = e.constraints.max;

    const std::string pretty = PrettyCategory(e.category);
    size_t ci = g_cats.size();
    for (size_t i = 0; i < g_cats.size(); ++i) {
      if (g_cats[i].display == pretty) {
        ci = i;
        break;
      }
    }
    if (ci == g_cats.size())
      g_cats.push_back({pretty, true, {}});
    g_cats[ci].cvars.push_back(std::move(row));
  }
  RebuildRows();
}

static uint32_t CvarModeRowCount() {
  return static_cast<uint32_t>(g_rows.size());
}

// -----------------------------------------------------------------------------
// Guest memory helpers (guest globals/values are stored big-endian)
// -----------------------------------------------------------------------------
static inline uint32_t ReadGuestBE32(uint8_t* base, uint32_t gaddr) {
  return RENUT_BSWAP32(*reinterpret_cast<uint32_t*>(base + gaddr));
}
static inline void WriteGuestBE32(uint8_t* base, uint32_t gaddr, uint32_t value) {
  *reinterpret_cast<uint32_t*>(base + gaddr) = RENUT_BSWAP32(value);
}

// Write an ASCII string into a guest XUI item's wide (UTF-16BE) text buffer.
// strcpy_s_0 (used by the game) copies 16-bit code units until a 16-bit NUL, and
// guest wide strings are big-endian, so each char becomes {0x00, ch}.
static void SetItemLabel(uint8_t* base, uint32_t itemObj, const std::string& text) {
  uint8_t* dst = base + itemObj + kItemTextOff;
  constexpr size_t kMaxChars = 120;  // buffer is 128 wide chars; leave headroom
  size_t n = text.size() < kMaxChars ? text.size() : kMaxChars;
  for (size_t i = 0; i < n; ++i) {
    dst[i * 2 + 0] = 0x00;
    dst[i * 2 + 1] = static_cast<uint8_t>(text[i]);
  }
  dst[n * 2 + 0] = 0x00;  // 16-bit NUL terminator
  dst[n * 2 + 1] = 0x00;
  WriteGuestBE32(base, itemObj + kItemFlagOff, 0);
  WriteGuestBE32(base, itemObj + kItemLenOff, static_cast<uint32_t>(n));
}

// Write an ASCII string as a UTF-16BE, NUL-terminated wide string into guest
// memory (for passing to recompiled XUI text setters).
static void WriteWideBE(uint8_t* base, uint32_t gaddr, const char* s) {
  uint8_t* d = base + gaddr;
  size_t i = 0;
  for (; s[i]; ++i) {
    d[i * 2 + 0] = 0x00;
    d[i * 2 + 1] = static_cast<uint8_t>(s[i]);
  }
  d[i * 2 + 0] = 0x00;
  d[i * 2 + 1] = 0x00;
}

// -----------------------------------------------------------------------------
// Label text for a cvar row
// -----------------------------------------------------------------------------
// A non-bool cvar is "cyclable" (dpad left/right changes it) if it has an
// allowed-value list or a numeric range.
static bool IsCyclable(const CvarRow& row) {
  if (row.type == FlagType::Boolean)
    return false;
  if (!row.allowed.empty())
    return true;
  const bool numeric = row.type == FlagType::Int32 || row.type == FlagType::Int64 ||
                       row.type == FlagType::Uint32 || row.type == FlagType::Uint64;
  return numeric && (row.min.has_value() || row.max.has_value());
}

static std::string CvarRowLabel(const CvarRow& row) {
  if (row.type == FlagType::Command)
    return std::string("> ") + row.name;  // clickable action (A to run)
  std::string value = rex::cvar::GetFlagByName(row.name);
  if (row.type == FlagType::Boolean) {
    // Checkbox on the left: [x] = on, [ ] = off.
    bool on = (value == "true" || value == "1" || value == "yes");
    return std::string(on ? "[x] " : "[ ] ") + row.name;
  }
  if (IsCyclable(row))
    return row.name + ":  < " + value + " >";  // dpad left/right cycles the choice
  return row.name + ":  " + value;             // display only
}

// Label for a flattened display row: a "[-]/[+] Category" header or an indented
// cvar row.
static std::string DisplayRowLabel(uint32_t index) {
  const DisplayRow& row = g_rows[index];
  const CvarCategory& cat = g_cats[row.cat];
  if (row.kind == RowKind::Header)
    return std::string(cat.expanded ? "[-] " : "[+] ") + cat.display;
  return "    " + CvarRowLabel(cat.cvars[row.cvar]);
}

// Rewrite one row's text buffer directly using the item object the label
// callback captured. No list rebuild, so the cursor and scroll don't move.
// Returns false if we have no captured item object for that row (fall back to a
// rebuild).
static bool UpdateRowInPlace(uint8_t* base, uint32_t sel) {
  if (sel < g_rowItemObj.size() && g_rowItemObj[sel] != 0) {
    SetItemLabel(base, g_rowItemObj[sel], DisplayRowLabel(sel));
    return true;
  }
  return false;
}

// -----------------------------------------------------------------------------
// Apply a change to the cvar at the given row. `forward` = next (A / dpad-right),
// !forward = previous (dpad-left). Bools ignore direction (just toggle).
// -----------------------------------------------------------------------------
static void ApplyCvarChange(const CvarRow& row, bool forward) {
  const std::string cur = rex::cvar::GetFlagByName(row.name);

  if (row.type == FlagType::Boolean) {
    bool on = (cur == "true" || cur == "1" || cur == "yes");
    rex::cvar::SetFlagByName(row.name, on ? "false" : "true");
    return;
  }

  // String or numeric with an explicit allowed list: cycle to the next/prev value.
  if (!row.allowed.empty()) {
    const size_t n = row.allowed.size();
    size_t idx = 0;
    for (size_t i = 0; i < n; ++i) {
      if (row.allowed[i] == cur) {
        idx = i;
        break;
      }
    }
    size_t next = forward ? (idx + 1) % n : (idx + n - 1) % n;
    rex::cvar::SetFlagByName(row.name, row.allowed[next]);
    return;
  }

  // Numeric with a range: step by +/-1, wrapping min..max.
  if (row.type == FlagType::Int32 || row.type == FlagType::Int64 ||
      row.type == FlagType::Uint32 || row.type == FlagType::Uint64) {
    long long v = std::strtoll(cur.c_str(), nullptr, 10);
    long long nv = v + (forward ? 1 : -1);
    if (row.max && nv > static_cast<long long>(*row.max))
      nv = row.min ? static_cast<long long>(*row.min) : static_cast<long long>(*row.max);
    else if (row.min && nv < static_cast<long long>(*row.min))
      nv = row.max ? static_cast<long long>(*row.max) : static_cast<long long>(*row.min);
    rex::cvar::SetFlagByName(row.name, std::to_string(nv));
    return;
  }

  // Anything else (free-form string, double): leave unchanged - display only.
}

// -----------------------------------------------------------------------------
// Persistence: write enabled (non-default) cvars to renut.toml.
//
// This MERGES into any existing file instead of replacing it: every line that
// isn't a managed cvar assignment (comments, blanks, section headers, keys we
// don't own) is preserved verbatim. A managed cvar is updated in place when it's
// non-default, or its line is dropped when it's back to default. Newly-enabled
// cvars are appended. Commands are never written (they have no value). This is
// why we don't use rex::cvar::SaveConfig(), which truncates the whole file.
// -----------------------------------------------------------------------------
static constexpr const char* kRenutConfigPath = "renut.toml";

static std::string TomlQuote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '\\' || c == '"')
      out += '\\';
    out += c;
  }
  out += '"';
  return out;
}

static std::string TrimWs(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos)
    return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// Not static: the controls overlay (overlays/mnk_controls_dialog.h) persists
// rebinds through this too.
void RenutSaveConfig() {
  // desired[name] = formatted TOML value, for every value cvar != its default.
  // managed = every value-cvar name, so we can update or drop its existing line.
  std::unordered_map<std::string, std::string> desired;
  std::unordered_set<std::string> managed;
  for (const auto& e : rex::cvar::GetRegistry()) {
    if (e.type == FlagType::Command)
      continue;  // commands carry no persistable value
    managed.insert(e.name);
    const std::string val = e.getter();
    if (val != e.default_value)
      desired[e.name] = (e.type == FlagType::String) ? TomlQuote(val) : val;
  }

  // Read the existing file (if any), rewriting managed lines and keeping the rest.
  std::vector<std::string> out;
  bool fileExisted = false;
  {
    std::ifstream in(kRenutConfigPath);
    if (in) {
      fileExisted = true;
      std::string line;
      while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
          line.pop_back();  // normalize CRLF

        const std::string trimmed = TrimWs(line);
        bool handled = false;
        if (!trimmed.empty() && trimmed[0] != '#' && trimmed[0] != '[') {
          const size_t eq = trimmed.find('=');
          if (eq != std::string::npos) {
            const std::string key = TrimWs(trimmed.substr(0, eq));
            if (managed.count(key)) {
              auto it = desired.find(key);
              if (it != desired.end()) {
                out.push_back(key + " = " + it->second);  // update in place
                desired.erase(it);
              }
              // else: managed key is back at default -> drop this line
              handled = true;
            }
          }
        }
        if (!handled)
          out.push_back(line);  // preserve comments / unknown keys / sections
      }
    }
  }

  // Nothing to write and no file to update: don't create an empty file.
  if (!fileExisted && desired.empty())
    return;

  // Append any newly-enabled cvars (sorted for a stable diff).
  std::vector<std::pair<std::string, std::string>> added(desired.begin(), desired.end());
  std::sort(added.begin(), added.end());
  for (const auto& kv : added)
    out.push_back(kv.first + " = " + kv.second);

  std::ofstream f(kRenutConfigPath, std::ios::trunc);
  if (!f) {
    REXLOG_ERROR("RenutSaveConfig: failed to open {}", kRenutConfigPath);
    return;
  }
  for (const auto& l : out)
    f << l << "\n";
}

// =============================================================================
// Midasm hook: content-row count injection (LABEL_52 of the builder, guest
// 0x825A8C88). When our section is showing cvars, override the row count with
// our flattened display-row count; the label/dispatch overrides supply the rest
// from g_rows. Otherwise leave the game's normal menu untouched.
// =============================================================================
void pauseMenu_InjectItems() {
  if (!g_cvarMenuOpen)
    return;
  REX_PPC_MEMBASE_PTR(membase);
  WriteGuestBE32(membase, kItemCount, CvarModeRowCount());
}

// =============================================================================
// Midasm hook: cursor restore (builder tail, at the XuiContainerSetCurSel call,
// guest 0x825A8D18). The game hard-codes selection 0 (li r4,0); when a toggle
// asked us to preserve the cursor, override r4 with that row index.
// =============================================================================
void pauseMenu_RestoreSel(PPCRegister& r4) {
  if (g_restoreSel >= 0) {
    r4.u32 = static_cast<uint32_t>(g_restoreSel);
    g_restoreSel = -1;  // one-shot
  }
}

// =============================================================================
// Override: per-item label callback (sub_825A96C8)
//   a1 = r3 (context), a2 = r4 (item index), a3 = r5 (item XUI object)
// =============================================================================
REX_HOOK_RAW(sub_825A96C8) {
  const uint32_t index = ctx.r4.u32;
  const uint32_t itemObj = ctx.r5.u32;

  if (g_cvarMenuOpen && index < g_rows.size()) {
    if (index < g_rowItemObj.size())
      g_rowItemObj[index] = itemObj;  // capture for in-place value updates
    SetItemLabel(base, itemObj, DisplayRowLabel(index));
    ctx.r3.u64 = 1;
    return;
  }

  __imp__sub_825A96C8(ctx, base);
}

// =============================================================================
// Override: selection/dispatch callback (sub_825A8D68)
//   a1 = r3 (pause object), a2 = r4 (message source), a3 = r5, a4 = r6
// =============================================================================
REX_HOOK_RAW(sub_825A8D68) {
  const uint32_t pauseObj = ctx.r3.u32;
  const uint32_t source = ctx.r4.u32;

  // Same guard as the original: only act on the pause list's own selection.
  const uint32_t list = ReadGuestBE32(base, pauseObj + kMenuListOff);
  const uint32_t list2 = ReadGuestBE32(base, pauseObj + kMenuList2Off);
  const bool forUs = (source == list || source == list2);

  if (forUs && g_cvarMenuOpen) {
    uint32_t sel;
    {
      rex::CallFrame frame{ctx};
      sel = RenutGetCurSel(frame, base, list);
    }
    if (sel < g_rows.size()) {
      const DisplayRow& row = g_rows[sel];
      if (row.kind == RowKind::Header) {
        g_cats[row.cat].expanded = !g_cats[row.cat].expanded;  // collapse/expand
        RebuildRows();
        g_restoreSel = -1;  // count changed: reset to top to avoid scroll drift
        g_pendingRebuildObj = pauseObj;
        g_pendingRebuild = true;  // full rebuild next frame (outside the event)
      } else {
        const CvarRow& cv = g_cats[row.cat].cvars[row.cvar];
        if (cv.type == FlagType::Command) {
          rex::cvar::InvokeCommand(cv.name, "");  // clickable action: run it
        } else {
          ApplyCvarChange(cv, true);  // A = toggle / next
          RenutSaveConfig();          // persist to renut.toml
          if (!UpdateRowInPlace(base, sel)) {  // in place: cursor/scroll stay put
            g_restoreSel = static_cast<int>(sel);
            g_pendingRebuildObj = pauseObj;
            g_pendingRebuild = true;  // fallback: full rebuild
          }
        }
      }
      ctx.r3.u64 = 1;
      return;
    }
  }

  __imp__sub_825A8D68(ctx, base);
}

// =============================================================================
// Frame tick (called once per frame from fpsCount_hook). Applies a deferred
// content-list rebuild outside XUI's event dispatch so a row-count change can't
// corrupt the list widget mid-message.
// =============================================================================
void renutCvarMenu_FrameTick() {
  if (!g_pendingRebuild)
    return;
  g_pendingRebuild = false;
  if (g_cvarMenuOpen)
    RenutRebuildPauseMenu(g_pendingRebuildObj);  // auto-context call
}

// =============================================================================
// "reNut Settings" pause-menu SECTION (its own tab in the section strip)
// =============================================================================

// -----------------------------------------------------------------------------
// Midasm hook: append our section id to the tab strip (end of sub_825A6708,
// guest 0x825A6B98, right after dword_82FA32C0 is finalized).
// -----------------------------------------------------------------------------
void pauseSection_InjectTab() {
  REX_PPC_MEMBASE_PTR(membase);
  uint32_t count = ReadGuestBE32(membase, kSectionCount);
  if (count < kGuestSectionMax) {
    WriteGuestBE32(membase, kSectionArray + count * 4, kSectionRenut);
    WriteGuestBE32(membase, kSectionCount, count + 1);
  }
}

// -----------------------------------------------------------------------------
// Override: tab icon setter (sub_825A8110)
//   a1 = r3, a2 = r4 (tab index), a3 = r5 (tab XUI object)
// For our section, temporarily borrow section 0's icon (the game-settings gear)
// so the original code loads a valid, already-preloaded icon.
// -----------------------------------------------------------------------------
REX_HOOK_RAW(sub_825A8110) {
  const uint32_t tabPos = ctx.r4.u32;
  const uint32_t idAddr = kSectionArray + tabPos * 4;
  if (ReadGuestBE32(base, idAddr) == kSectionRenut) {
    WriteGuestBE32(base, idAddr, kSectionIconBorrow);  // town-map icon (unused elsewhere)
    __imp__sub_825A8110(ctx, base);
    WriteGuestBE32(base, idAddr, kSectionRenut);       // restore
    return;
  }
  __imp__sub_825A8110(ctx, base);
}

// -----------------------------------------------------------------------------
// Override: section title text setter (sub_825A7CF0)
//   a1 = r3 (pause object), a2 = r4 (current tab index)
// For our section, set the title to "reNut Settings".
// -----------------------------------------------------------------------------
REX_HOOK_RAW(sub_825A7CF0) {
  const uint32_t pauseObj = ctx.r3.u32;
  const uint32_t tabPos = ctx.r4.u32;
  if (ReadGuestBE32(base, kSectionArray + tabPos * 4) == kSectionRenut) {
    const uint32_t titleEl = ReadGuestBE32(base, pauseObj + kSectionTitleElemOff);
    // Carve a small scratch buffer off the guest stack for the wide string.
    const uint32_t savedR1 = ctx.r1.u32;
    ctx.r1.u32 = (ctx.r1.u32 - 64) & ~15u;
    const uint32_t scratch = ctx.r1.u32;
    WriteWideBE(base, scratch, "reNut Settings");
    {
      rex::CallFrame frame{ctx};
      RenutSetTitleText(frame, base, titleEl, scratch);
    }
    ctx.r1.u32 = savedR1;
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_825A7CF0(ctx, base);
}

// -----------------------------------------------------------------------------
// Override: section switch / setMode (sub_825A7D90)
//   a1 = r3 (pause object), a2 = r4 (section id / mode)
// Selecting our section builds the cvar list as its content; leaving it clears
// cvar display so the real section builders run normally.
// -----------------------------------------------------------------------------
REX_HOOK_RAW(sub_825A7D90) {
  const uint32_t pauseObj = ctx.r3.u32;
  const uint32_t mode = ctx.r4.u32;

  if (mode == kSectionRenut) {
    SnapshotCvars();
    g_cvarMenuOpen = true;
    g_restoreSel = -1;
    __imp__sub_825A7D90(ctx, base);  // housekeeping (mode>7 falls through, stores mode)
    rex::CallFrame frame{ctx};
    RenutRebuildPauseMenu(frame, base, pauseObj);  // build the cvar list into the content list
    return;
  }

  g_cvarMenuOpen = false;  // a normal section owns the content list again
  __imp__sub_825A7D90(ctx, base);
}

// -----------------------------------------------------------------------------
// Override: pause input handler (sub_825A76E0)
//   a1 = r3 (pause object)
// Its "activate content item" dispatch is a jump table bounded to modes 0..7
// (mode 0 -> sub_825A8D68). In our section (mode 8) A-press is dropped, so cvars
// never toggle. While our section is active, present mode 0 for the duration of
// the call so "activate" routes to sub_825A8D68 (our toggle). We restore mode 8
// afterward unless the call itself switched sections (LEFT/RIGHT nav).
// -----------------------------------------------------------------------------
REX_HOOK_RAW(sub_825A76E0) {
  const uint32_t pauseObj = ctx.r3.u32;
  const uint32_t source = ctx.r4.u32;
  const uint32_t modeAddr = pauseObj + kPauseModeField;
  const bool inRenut = (ReadGuestBE32(base, modeAddr) == kSectionRenut);

  // Backstop for the frame tick: apply any deferred rebuild before handling this
  // input. We're at the start of a fresh scene message, so the prior list event
  // has fully unwound - safe to change the row count here.
  if (g_pendingRebuild) {
    g_pendingRebuild = false;
    if (g_cvarMenuOpen) {
      rex::CallFrame frame{ctx};
      RenutRebuildPauseMenu(frame, base, g_pendingRebuildObj);
    }
  }

  // Dpad left/right on a cyclable (non-bool) cvar row changes its value instead
  // of switching sections. Other rows fall through to normal tab navigation.
  if (inRenut && g_cvarMenuOpen) {
    const uint32_t leftMsg = ReadGuestBE32(base, pauseObj + kPauseLeftMsgOff);
    const uint32_t rightMsg = ReadGuestBE32(base, pauseObj + kPauseRightMsgOff);
    if (source == leftMsg || source == rightMsg) {
      const uint32_t list = ReadGuestBE32(base, pauseObj + kMenuListOff);
      uint32_t sel;
      {
        rex::CallFrame frame{ctx};
        sel = RenutGetCurSel(frame, base, list);
      }
      if (sel < g_rows.size() && g_rows[sel].kind == RowKind::Cvar) {
        const CvarRow& cv = g_cats[g_rows[sel].cat].cvars[g_rows[sel].cvar];
        if (IsCyclable(cv)) {
          ApplyCvarChange(cv, source == rightMsg);  // right = next, left = prev
          RenutSaveConfig();                         // persist to renut.toml
          if (!UpdateRowInPlace(base, sel)) {  // in place: cursor/scroll stay put
            g_restoreSel = static_cast<int>(sel);
            g_pendingRebuildObj = pauseObj;
            g_pendingRebuild = true;  // fallback: full rebuild
          }
          ctx.r3.u64 = 1;
          return;  // handled - do NOT switch section
        }
      }
    }
  }

  if (inRenut) {
    WriteGuestBE32(base, modeAddr, 0);  // route activate -> sub_825A8D68
    __imp__sub_825A76E0(ctx, base);
    if (ReadGuestBE32(base, modeAddr) == 0)  // call didn't switch sections
      WriteGuestBE32(base, modeAddr, kSectionRenut);
    return;
  }
  __imp__sub_825A76E0(ctx, base);
}
