/**
 * @file    game/coop_menu.cpp
 * @brief   Co-op in the start menu: Single Player's list gets "Co-op" items.
 *
 * N&B (IDA): the start menu's Single Player panel (sub_825BCDD8) fills its list
 * (scene + 196; object + 4 its HXUIOBJ, + 28 its count; class vtable
 * 0x8217AE20) with New Game, and Continue when there is a save (+ 369),
 * through sub_823E43D8(list, count). The list asks sub_82298B18(list, index,
 * item) for each item's text (sub_823E4A38(item, 0, text)) and icon
 * (gameAssetRefGetAid_82254D10, sub_823E4A90(item, 1, ref)). A press goes to
 * sub_825BC8D8(scene, control): the list or the A button (+ 220), by the
 * list's selection (sub_8257AD00); the pad is r6, as sub_825BACD8 passes it
 * on. Multiplayer, Online is sub_825BBD00(scene, pad, 1): with a save it
 * starts the game's online play, the player hosting their party.
 *
 * After the game's items come the co-op ones:
 * - "Co-op: Host": online play starts as from Multiplayer, Online, and the
 *   session is co-op (online/coop.h): its matches take place in Showdown Town.
 * - "Co-op: Join <name>", one per online friend hosting co-op: the game joins
 *   that friend's session as if it had accepted an invite.
 */
#include <rex/hook.h>
#include <rex/types.h>

#include <cstdint>
#include <string>
#include <vector>

#include "renut_engine/app/renut_logging.h"
#include "renut_engine/game/guest_call.h"
#include "renut_engine/online/coop.h"
#include "renut_engine/online/friends.h"
#include "renut_engine/online/online.h"

REX_EXTERN(__imp__sub_823E43D8);  // list: insert 'count' items
REX_EXTERN(__imp__sub_82298B18);  // Single Player list: an item's text and icon
REX_EXTERN(__imp__sub_825BC8D8);  // start menu, Single Player panel: a press
REX_IMPORT(__imp__sub_823E4A38, SetItemText, u32(u32, u32, u32));      // (item, column, wchar_t *)
REX_IMPORT(__imp__sub_823E4A90, SetItemImage, u32(u32, u32, u32));     // (item, column, asset ref)
REX_IMPORT(__imp__gameAssetRefGetAid, GameAssetRefGetAid, u32(u32, u32, u32));
REX_IMPORT(__imp__sub_8257AD00, XuiContainerGetCurSel, u32(u32));
REX_IMPORT(__imp__sub_825BBD00, StartOnlinePlay, void(u32, u32, u32));  // (scene, pad, online)
REX_IMPORT(__imp__uiSystemSfxPlayOperation, PlayUiSound, void(u32, u32));

namespace {

using namespace renut::online;
using renut::guest::Guest;
using renut::guest::Load32;
using renut::guest::Store32;

constexpr uint32_t kSinglePlayerListVtable = 0x8217AE20;
constexpr uint32_t kListCount = 28;
constexpr uint32_t kSceneList = 196;
constexpr uint32_t kSceneAButton = 220;
constexpr uint32_t kSounds = 0x82FAC768;  // 0 select, 5 can't
constexpr size_t kMaxJoinItems = 3;
constexpr const char kHostIcon[] = "startmenumultiplayer";
constexpr const char kJoinIcon[] = "startmenucontest";

enum class ItemKind { kHost, kJoin, kNoHost, kOffline };

struct Item {
  ItemKind kind;
  std::string label;
  const char *icon;
  std::string friend_id;
};

// The list's co-op items, after the game's 'g_base' items. Game thread (XUI).
uint32_t g_list = 0;
uint32_t g_base = 0;
std::vector<Item> g_items;

std::vector<Item> MakeItems() {
  std::vector<Item> items;
  if (!Enabled()) {
    items.push_back({ItemKind::kOffline, "Co-op (go online first, F5)", kHostIcon, {}});
    return items;
  }
  items.push_back({ItemKind::kHost, "Co-op: Host", kHostIcon, {}});
  size_t joins = 0;
  for (const friends::Friend &f : friends::Friends()) {
    if (joins >= kMaxJoinItems)
      break;
    if (f.online && f.session.Valid() && f.session.coop) {
      items.push_back({ItemKind::kJoin, "Co-op: Join " + (f.gamertag.empty() ? std::string("a friend") : f.gamertag),
                       kJoinIcon, f.id});
      ++joins;
    }
  }
  if (!joins)
    items.push_back({ItemKind::kNoHost, "Co-op: Join (no friend is hosting)", kJoinIcon, {}});
  return items;
}

// ASCII as the game's big-endian wide text, at most 120 characters.
std::vector<uint8_t> WideText(const std::string &text) {
  std::vector<uint8_t> out;
  for (char c : text) {
    if (out.size() >= 240)
      break;
    out.push_back(0);
    out.push_back(uint8_t(c));
  }
  out.push_back(0);
  out.push_back(0);
  return out;
}

}  // namespace

// HRESULT sub_823E43D8(list object, count): inserts a list's items; the
// Single Player list's get the co-op ones after them.
REX_HOOK_RAW(sub_823E43D8) {
  const uint32_t list = ctx.r3.u32;
  if (Load32(base, list) == kSinglePlayerListVtable) {
    // Back in the start menu outside online play: co-op is over.
    if (coop::CurrentRole() != coop::Role::kNone && !InSession())
      coop::Leave();
    g_list = list;
    g_base = ctx.r4.u32;
    g_items = MakeItems();
    const uint32_t count = g_base + uint32_t(g_items.size());
    Store32(base, list + kListCount, count);
    ctx.r4.u64 = count;
  }
  __imp__sub_823E43D8(ctx, base);
}

// int sub_82298B18(list object, index, item): a Single Player item's text and icon.
REX_HOOK_RAW(sub_82298B18) {
  const uint32_t list = ctx.r3.u32;
  const uint32_t index = ctx.r4.u32;
  if (list != g_list || index < g_base || index - g_base >= g_items.size()) {
    __imp__sub_82298B18(ctx, base);
    return;
  }
  const uint32_t item = ctx.r5.u32;
  const Item &entry = g_items[index - g_base];
  {
    Guest g(ctx, base);
    auto &f = g.frame();
    const std::vector<uint8_t> text = WideText(entry.label);
    SetItemText(f, base, item, 0, g.Push(text.data(), uint32_t(text.size())));
    const std::string icon = entry.icon;
    const uint32_t name = g.Push(icon.c_str(), uint32_t(icon.size() + 1));
    const uint32_t ref = g.Alloc(16);
    if (GameAssetRefGetAid(f, base, name, ref, 0) & 0xFF)
      SetItemImage(f, base, item, 1, ref);
  }
  ctx.r3.u64 = 1;
}

// int sub_825BC8D8(scene, control): a press in the Single Player panel (pad in r6).
REX_HOOK_RAW(sub_825BC8D8) {
  const uint32_t scene = ctx.r3.u32;
  const uint32_t control = ctx.r4.u32;
  const uint32_t pad = ctx.r6.u32;
  const uint32_t list = Load32(base, scene + kSceneList);
  if (g_items.empty() || (control != list && control != Load32(base, scene + kSceneAButton))) {
    __imp__sub_825BC8D8(ctx, base);
    return;
  }
  uint32_t selected;
  {
    Guest g(ctx, base);
    selected = XuiContainerGetCurSel(g.frame(), base, list);
  }
  if (selected < g_base || selected - g_base >= g_items.size()) {
    __imp__sub_825BC8D8(ctx, base);
    return;
  }
  const Item item = g_items[selected - g_base];
  // The town this host's matches use, for its progress (the joiners load it too).
  const uint32_t town = item.kind == ItemKind::kHost ? coop::TownScene(ctx, base) : 0;
  Guest g(ctx, base);
  auto &f = g.frame();
  switch (item.kind) {
  case ItemKind::kHost:
    coop::Host(town);
    StartOnlinePlay(f, base, scene, pad, 1);
    break;
  case ItemKind::kJoin:
    if (coop::Join(item.friend_id)) {
      PlayUiSound(f, base, kSounds, 0);
    } else {
      PlayUiSound(f, base, kSounds, 5);
    }
    break;
  case ItemKind::kNoHost:
  case ItemKind::kOffline:
    PlayUiSound(f, base, kSounds, 5);
    break;
  }
  ctx.r3.u64 = 1;
}
