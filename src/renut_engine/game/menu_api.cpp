#include "renut_engine/game/menu_api.h"

#include <rex/kernel/heap.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>
#include <rexfabric/game_api.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "renut_engine/game/guest_call.h"

// Hooked: the game's list class, under every XUI list its menus fill, and its scenes' presses.
REX_EXTERN(__imp__sub_823B2130);  // (list, count): inserts count rows at the top
REX_EXTERN(__imp__sub_823B1EC8);  // (list, message): fills the shown rows' text, each frame
REX_EXTERN(__imp__sub_821EED78);  // (list, {from, to} *, handled *): the selection moved
REX_EXTERN(__imp__sub_823B3240);  // (scene, source, u8 *user, handled *): XN_PRESS
REX_EXTERN(__imp__sub_823B32F8);  // (scene, source, notify *, handled *): the game's own press (A)
REX_EXTERN(__imp__sub_823AFCC0);  // (button, key, user, ...): a prompt's key down, which presses its scene (+ 52)

// Called.
REX_IMPORT(__imp__sub_82A07970, ElementData, u32(u32, u32));  // (handle, class): the element's data
REX_IMPORT(__imp__sub_82229C90, XuiElementGetParent, u32(u32, u32));
REX_IMPORT(__imp__sub_82263290, XuiElementGetUserFocus, u32(u32));
REX_IMPORT(__imp__sub_8255E5B0, XuiContainerGetCurSel, u32(u32));
REX_IMPORT(__imp__rex_uiSystemSfxPlayOperation, PlayUiSound, void(u32, u32));
REX_IMPORT(__imp__sub_823B0090, ButtonScene, u32(u32));  // (button): the scene object its presses go to

namespace renut::game::menu_api {
namespace {

using renut::guest::Load16;
using renut::guest::Load32;
using renut::guest::Store16;
using renut::guest::Store32;
using Error = std::runtime_error;
using Clock = std::chrono::steady_clock;

// Guest data.
constexpr uint32_t kElementClass = 0x82DA3488;  // XuiElement's class, as sub_82247F88 asks for it
constexpr uint32_t kElementId = 4;              // element data + 4: its Id (wide string)
constexpr uint32_t kSounds = 0x82E19A70;        // uiSystemSfxPlayOperation's sounds: 0 select
constexpr uint32_t kSoundSelect = 0;
// List objects: + 4 HXUIOBJ, + 8 the row vector (u16 row size, then begin, end).
constexpr uint32_t kListHandle = 4;
constexpr uint32_t kListRows = 8;
// A row record: column 0's text (128 wide chars) at + 8 as sub_823B2798 writes it; + 3016 set once the
// list's text callback (vtable + 40) filled it, which sub_823B1EC8 then skips.
constexpr uint32_t kRowText = 8;
constexpr uint32_t kRowTextImage = 268;
constexpr uint32_t kRowTextLength = 272;
constexpr uint32_t kRowFilled = 3016;
constexpr uint32_t kRowMinSize = kRowFilled + 4;
constexpr size_t kMaxText = 127;
constexpr int kMaxDepth = 16;
constexpr size_t kMaxPaths = 256;
constexpr const char kAPromptId[] = "abutton";
// A scene object's vtable: + 52 its press handler; sub_826B9BD0 is the game's "li r3, 1; blr" (taken).
constexpr uint32_t kScenePress = 52;
constexpr uint32_t kPressTaken = 0x826B9BD0;
constexpr uint32_t kVtableCopySize = 512;
// One A comes as the list's press and the page's A prompt's: the second within this is the same press.
constexpr auto kSamePress = std::chrono::milliseconds(300);

// A row a mod added: to the lists whose XUI Id path ends with 'list'.
struct Row {
  int64_t id;
  std::string key;
  std::string list;
  std::u16string text;
};

// A list showing rows of ours: its object, its own rows (before ours) and ours, in order.
struct Shown {
  uint32_t object = 0;
  uint32_t game_rows = 0;
  std::vector<int64_t> ids;
  bool dirty = false;  // a row's text changed: rewritten at the list's next fill
};

std::mutex g_mutex;
std::vector<Row> g_rows;
int64_t g_next_id = 1;
std::unordered_map<uint32_t, Shown> g_shown;  // by the list's handle
std::vector<std::string> g_paths;             // every list's path seen, for renut.menu.lists
Clock::time_point g_last_select;
std::atomic<bool> g_any_shown{false};
std::atomic<bool> g_any_dirty{false};
std::optional<rexfabric::Event> g_select;

bool Failed(uint32_t hr) { return int32_t(hr) < 0; }

// UTF-8 as the wide text a row holds, at most kMaxText long; what the font can't show is '?'.
std::u16string Wide(const std::string &text) {
  std::u16string out;
  for (size_t i = 0; i < text.size() && out.size() < kMaxText;) {
    const uint8_t c = uint8_t(text[i]);
    char16_t ch = u'?';
    size_t length = 1;
    if (c < 0x80) {
      ch = c;
    } else if ((c >> 5) == 0x6 && i + 1 < text.size()) {
      ch = char16_t(((c & 0x1F) << 6) | (text[i + 1] & 0x3F));
      length = 2;
    } else if ((c >> 4) == 0xE && i + 2 < text.size()) {
      ch = char16_t(((c & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F));
      length = 3;
    } else if ((c >> 3) == 0x1E) {
      length = 4;
    }
    out.push_back(ch);
    i += length;
  }
  return out;
}

const Row *FindRow(int64_t id) {
  for (const Row &row : g_rows)
    if (row.id == id)
      return &row;
  return nullptr;
}

// "singlePlayerList" matches ".../singlePlayerList"; "StartMenu/singlePlayerList" its end too.
bool Matches(const std::string &path, const std::string &list) {
  if (path == list)
    return true;
  return path.size() > list.size() && path.ends_with(list) && path[path.size() - list.size() - 1] == '/';
}

std::string ElementId(renut::guest::Guest &g, uint8_t *base, uint32_t handle) {
  std::string id;
  const uint32_t data = ElementData(g.ctx(), base, handle, Load32(base, kElementClass));
  const uint32_t text = data ? Load32(base, data + kElementId) : 0;
  for (uint32_t i = 0; text && i < 64; ++i) {
    const uint16_t c = Load16(base, text + 2 * i);
    if (!c)
      break;
    id.push_back(c < 0x80 ? char(c) : '?');
  }
  return id;
}

// The Ids from the scene down to the element, '/' between ("StartMenu/singlePlayerList"); no Id, no part.
std::string ElementPath(PPCContext &ctx, uint8_t *base, uint32_t handle) {
  renut::guest::Guest g(ctx, base);
  const uint32_t out = g.Alloc(4);
  std::vector<std::string> ids;
  for (int depth = 0; handle && depth < kMaxDepth; ++depth) {
    if (std::string id = ElementId(g, base, handle); !id.empty())
      ids.push_back(std::move(id));
    Store32(base, out, 0);
    if (Failed(XuiElementGetParent(g.ctx(), base, handle, out)))
      break;
    handle = Load32(base, out);
  }
  std::string path;
  for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
    if (!path.empty())
      path += '/';
    path += *it;
  }
  return path;
}

// Rows [first, first + texts) of the list: column 0's text, marked filled.
void WriteRows(uint8_t *base, uint32_t list, uint32_t first, const std::vector<std::u16string> &texts) {
  const uint32_t rows = Load32(base, list + kListRows);
  if (!rows)
    return;
  const uint32_t size = Load16(base, rows);
  const uint32_t begin = Load32(base, rows + 4);
  const uint32_t end = Load32(base, rows + 8);
  if (size < kRowMinSize || !begin || end < begin)
    return;
  const uint32_t count = (end - begin) / size;
  for (uint32_t i = 0; i < texts.size() && first + i < count; ++i) {
    const uint32_t row = begin + (first + i) * size;
    const std::u16string &text = texts[i];
    for (size_t c = 0; c < text.size(); ++c)
      Store16(base, uint32_t(row + kRowText + 2 * c), uint16_t(text[c]));
    Store16(base, uint32_t(row + kRowText + 2 * text.size()), 0);
    Store32(base, row + kRowTextImage, 0);
    Store32(base, row + kRowTextLength, uint32_t(text.size()));
    Store32(base, row + kRowFilled, 1);
  }
}

// The texts of a shown list's rows of ours; a removed row keeps its text until the menu fills the list again.
std::vector<std::u16string> Texts(uint8_t *base, uint32_t list, const Shown &shown) {
  std::vector<std::u16string> texts;
  const uint32_t rows = Load32(base, list + kListRows);
  const uint32_t size = rows ? Load16(base, rows) : 0;
  const uint32_t begin = rows ? Load32(base, rows + 4) : 0;
  for (size_t i = 0; i < shown.ids.size(); ++i) {
    if (const Row *row = FindRow(shown.ids[i])) {
      texts.push_back(row->text);
      continue;
    }
    std::u16string text;
    const uint32_t at = begin + (shown.game_rows + uint32_t(i)) * size + kRowText;
    for (uint32_t c = 0; size >= kRowMinSize && begin && c < kMaxText; ++c) {
      const uint16_t ch = Load16(base, at + 2 * c);
      if (!ch)
        break;
      text.push_back(char16_t(ch));
    }
    texts.push_back(std::move(text));
  }
  return texts;
}

// Our rows in the list: [first, last); none when it shows none.
std::pair<int32_t, int32_t> OurRows(const uint8_t *base, uint32_t list) {
  if (!list || !g_any_shown.load(std::memory_order_relaxed))
    return {0, 0};
  std::lock_guard lock(g_mutex);
  const auto it = g_shown.find(Load32(base, list + kListHandle));
  if (it == g_shown.end() || it->second.object != list)
    return {0, 0};
  return {int32_t(it->second.game_rows), int32_t(it->second.game_rows + it->second.ids.size())};
}

// The list a handle is, if it shows rows of ours and is still that list.
bool ShownList(const uint8_t *base, uint32_t handle) {
  const auto it = g_shown.find(handle);
  return it != g_shown.end() && Load32(base, it->second.object + kListHandle) == handle;
}

// The page's A prompt ("aButton"), which the game presses (on release) along with the focused list.
bool APrompt(renut::guest::Guest &g, uint8_t *base, uint32_t source) {
  std::string id = ElementId(g, base, source);
  for (char &c : id)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return id == kAPromptId;
}

// The press renut.menu_select's listeners run inside, while they do.
thread_local const Press *t_press = nullptr;

// Whether a press is A on a row of ours: from the list itself, or the page's A prompt while the list has
// the focus. Fires renut.menu_select once per press, with the press lent to its listeners.
bool PressOnOurRow(PPCContext &ctx, uint8_t *base, uint32_t source, uint32_t user, uint32_t scene) {
  if (!g_any_shown.load(std::memory_order_relaxed))
    return false;
  renut::guest::Guest g(ctx, base);
  uint32_t list = 0;
  {
    std::lock_guard lock(g_mutex);
    if (ShownList(base, source))
      list = source;
  }
  if (!list) {
    if (!APrompt(g, base, source))
      return false;
    const uint32_t out = g.Alloc(4);
    uint32_t handle = XuiElementGetUserFocus(g.ctx(), base, user);
    for (int depth = 0; handle && depth < 4; ++depth) {
      {
        std::lock_guard lock(g_mutex);
        if (ShownList(base, handle)) {
          list = handle;
          break;
        }
      }
      Store32(base, out, 0);
      if (Failed(XuiElementGetParent(g.ctx(), base, handle, out)))
        break;
      handle = Load32(base, out);
    }
    if (!list)
      return false;
  }
  const int32_t selected = int32_t(XuiContainerGetCurSel(g.ctx(), base, list));
  int64_t id = 0;
  {
    std::lock_guard lock(g_mutex);
    const auto it = g_shown.find(list);
    if (it == g_shown.end())
      return false;
    const int32_t first = int32_t(it->second.game_rows);
    if (selected < first || selected >= first + int32_t(it->second.ids.size()))
      return false;
    id = it->second.ids[size_t(selected - first)];
    const Clock::time_point now = Clock::now();
    if (now - g_last_select < kSamePress)
      return true;
    g_last_select = now;
  }
  PlayUiSound(g.ctx(), base, kSounds, kSoundSelect);
  if (g_select) {
    const Press press{&ctx, base, scene};
    const Press *const outer = std::exchange(t_press, &press);
    g_select->Fire(id, int64_t(user));
    t_press = outer;
  }
  return true;
}

// renut.menu.add{list, text, on_select}: a row of the mod's at the end of the game's list menus.
constexpr const char kLua[] = R"lua(
local menu = renut.menu
local Row = {}
Row.__index = Row
local rows = {}

-- A new Lua state (the game starting, scripts reloading): the rows are the mods' from now.
menu._reset()

local function text_of(text, what)
  assert(type(text) == "string", what .. ": the text is a string")
  return text
end

function menu.add(def)
  assert(type(def) == "table", "renut.menu.add: takes a table {list = ..., text = ..., on_select = ...}")
  assert(type(def.list) == "string" and #def.list > 0,
         "renut.menu.add: list is a menu list's XUI id, e.g. \"singlePlayerList\" (renut.menu.lists() shows them)")
  text_of(def.text, "renut.menu.add")
  assert(def.on_select == nil or type(def.on_select) == "function", "renut.menu.add: on_select is a function")
  assert(def.key == nil or type(def.key) == "string", "renut.menu.add: key is a string")
  local key = def.key or (def.list .. "\n" .. def.text)
  local id = menu._add(key, def.list, def.text)
  local row = rows[id] or setmetatable({id = id}, Row)
  row.key, row.list, row.text, row.on_select = key, def.list, def.text, def.on_select
  rows[id] = row
  return row
end

function Row:set_text(text)
  self.text = text_of(text, "row:set_text")
  menu._set_text(self.id, text)
end

function Row:remove()
  menu._remove(self.id)
  rows[self.id] = nil
end

function menu.lists()
  local lists = {}
  for path in menu._lists():gmatch("[^\n]+") do lists[#lists + 1] = path end
  return lists
end

rex.on("renut.menu_select", function(id, user)
  local row = rows[id]
  if row and row.on_select then row.on_select(row, user) end
end)
)lua";

} // namespace

const Press *CurrentPress() { return t_press; }

void Declare() {
  rexfabric::Namespace renut("renut");
  renut.Category("menu.", "Menus");  // the node editor's add menu
  for (const auto &[name, args] : std::initializer_list<std::pair<const char *, std::vector<std::string>>>{
           {"menu._add", {"key", "list", "text"}},
           {"menu._set_text", {"row", "text"}},
           {"menu._remove", {"row"}},
           {"menu._lists", {}},
           {"menu._reset", {}},
           {"menu_select", {"row", "user"}}})
    renut.Args(name, args);  // the node editor's pins

  renut.Function("menu._add", [](std::string key, std::string list, std::string text) -> int64_t {
    if (list.empty())
      throw Error("a row needs the list it goes in");
    std::lock_guard lock(g_mutex);
    for (Row &row : g_rows)
      if (row.key == key) {
        row.list = std::move(list);
        row.text = Wide(text);
        for (auto &[handle, shown] : g_shown)
          for (const int64_t id : shown.ids)
            if (id == row.id)
              shown.dirty = true;
        g_any_dirty.store(true, std::memory_order_relaxed);
        return row.id;
      }
    g_rows.push_back({g_next_id++, std::move(key), std::move(list), Wide(text)});
    return g_rows.back().id;
  });
  renut.Function("menu._set_text", [](int64_t id, std::string text) {
    std::lock_guard lock(g_mutex);
    for (Row &row : g_rows)
      if (row.id == id)
        row.text = Wide(text);
    for (auto &[handle, shown] : g_shown)
      for (const int64_t shown_id : shown.ids)
        if (shown_id == id)
          shown.dirty = true;
    g_any_dirty.store(true, std::memory_order_relaxed);
  });
  renut.Function("menu._remove", [](int64_t id) {
    std::lock_guard lock(g_mutex);
    std::erase_if(g_rows, [id](const Row &row) { return row.id == id; });
  });
  renut.Function("menu._lists", []() -> std::string {
    std::lock_guard lock(g_mutex);
    std::string lists;
    for (const std::string &path : g_paths)
      lists += path + "\n";
    return lists;
  });
  renut.Function("menu._reset", [] {
    std::lock_guard lock(g_mutex);
    g_rows.clear();
  });
  g_select = renut.Event("menu_select");
  renut.Lua(kLua);
}

} // namespace renut::game::menu_api

using renut::game::menu_api::g_any_dirty;
using renut::game::menu_api::g_any_shown;
using renut::game::menu_api::g_mutex;
using renut::game::menu_api::g_paths;
using renut::game::menu_api::g_rows;
using renut::game::menu_api::g_shown;
using renut::guest::Load32;
using renut::guest::Store32;

// ListInsertItems(list, count): the rows of ours for this list go after the menu's own.
REX_HOOK_RAW(sub_823B2130) {
  namespace m = renut::game::menu_api;
  const uint32_t list = ctx.r3.u32;
  const uint32_t count = ctx.r4.u32;
  const uint32_t handle = list ? Load32(base, list + m::kListHandle) : 0;
  std::vector<int64_t> ids;
  std::vector<std::u16string> texts;
  if (handle && int32_t(count) >= 0) {
    const std::string path = m::ElementPath(ctx, base, handle);
    std::lock_guard lock(g_mutex);
    if (!path.empty() && g_paths.size() < m::kMaxPaths &&
        std::find(g_paths.begin(), g_paths.end(), path) == g_paths.end())
      g_paths.push_back(path);
    for (const m::Row &row : g_rows)
      if (!path.empty() && m::Matches(path, row.list)) {
        ids.push_back(row.id);
        texts.push_back(row.text);
      }
    if (ids.empty())
      g_shown.erase(handle);
    else
      g_shown[handle] = {list, count, ids, false};
    g_any_shown.store(!g_shown.empty(), std::memory_order_relaxed);
  }
  ctx.r4.u64 = count + uint32_t(ids.size());
  __imp__sub_823B2130(ctx, base);
  if (!ids.empty())
    m::WriteRows(base, list, count, texts);
}

// The list's per-frame fill: a row of ours whose text changed is rewritten first.
REX_HOOK_RAW(sub_823B1EC8) {
  namespace m = renut::game::menu_api;
  const uint32_t list = ctx.r3.u32;
  if (list && g_any_dirty.load(std::memory_order_relaxed)) {
    std::lock_guard lock(g_mutex);
    const auto it = g_shown.find(Load32(base, list + m::kListHandle));
    if (it != g_shown.end() && it->second.object == list && it->second.dirty) {
      m::WriteRows(base, list, it->second.game_rows, m::Texts(base, list, it->second));
      it->second.dirty = false;
    }
    bool dirty = false;
    for (const auto &[handle, shown] : g_shown)
      dirty = dirty || shown.dirty;
    g_any_dirty.store(dirty, std::memory_order_relaxed);
  }
  __imp__sub_823B1EC8(ctx, base);
}

// The selection moved: the list's own callback (vtable + 56) never gets a row of ours, which its tables lack.
REX_HOOK_RAW(sub_821EED78) {
  namespace m = renut::game::menu_api;
  const uint32_t list = ctx.r3.u32;
  const uint32_t indexes = ctx.r4.u32;
  const auto [first, last] = m::OurRows(base, list);
  if (indexes && first < last) {
    const uint32_t from = Load32(base, indexes);
    const uint32_t to = Load32(base, indexes + 4);
    const auto ours = [&](uint32_t index) { return int32_t(index) >= first && int32_t(index) < last; };
    if (ours(to)) {
      ctx.r3.u64 = 0;
      return;
    }
    if (ours(from)) {
      Store32(base, indexes, to);
      __imp__sub_821EED78(ctx, base);
      Store32(base, indexes, from);
      return;
    }
  }
  __imp__sub_821EED78(ctx, base);
}

// A scene's XN_PRESS: A on a row of ours is the mod's, and the scene never sees it.
REX_HOOK_RAW(sub_823B3240) {
  const uint32_t user = ctx.r5.u32 ? base[ctx.r5.u32] : 0;
  if (const uint32_t handled = ctx.r6.u32;
      handled && renut::game::menu_api::PressOnOurRow(ctx, base, ctx.r4.u32, user, ctx.r3.u32)) {
    Store32(base, handled, 1);
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_823B3240(ctx, base);
}

// A prompt button's key down (button + 4 its handle): A on the page's A prompt, the list on a row of ours, is
// the mod's. The button still goes down and up as for a taken press, its scene's handler answering "taken".
REX_HOOK_RAW(sub_823AFCC0) {
  namespace m = renut::game::menu_api;
  constexpr uint32_t kKeyPadA = 0x5800;  // VK_PAD_A
  const uint32_t button = ctx.r3.u32;
  if (!button || ctx.r4.u32 != kKeyPadA) {
    __imp__sub_823AFCC0(ctx, base);
    return;
  }
  uint32_t scene = 0;
  {
    renut::guest::Guest g(ctx, base);
    scene = ButtonScene(g.ctx(), base, button);
  }
  if (!m::PressOnOurRow(ctx, base, Load32(base, button + 4), ctx.r5.u32, scene)) {
    __imp__sub_823AFCC0(ctx, base);
    return;
  }
  static uint32_t copy = REX_RUNTIME()->GetGuestHeap().SystemHeapAlloc(m::kVtableCopySize);
  const uint32_t vtable = scene ? Load32(base, scene) : 0;
  if (!copy || !vtable) {
    ctx.r3.u64 = 1;
    return;
  }
  std::memcpy(base + copy, base + vtable, m::kVtableCopySize);
  Store32(base, copy + m::kScenePress, m::kPressTaken);
  Store32(base, scene, copy);
  __imp__sub_823AFCC0(ctx, base);
  Store32(base, scene, vtable);
}

// The game's own press (its button system's A, notify + 8 the user): as XN_PRESS.
REX_HOOK_RAW(sub_823B32F8) {
  const uint32_t user = ctx.r5.u32 ? base[ctx.r5.u32 + 8] : 0;
  if (const uint32_t handled = ctx.r6.u32;
      handled && renut::game::menu_api::PressOnOurRow(ctx, base, ctx.r4.u32, user, ctx.r3.u32)) {
    Store32(base, handled, 1);
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_823B32F8(ctx, base);
}
