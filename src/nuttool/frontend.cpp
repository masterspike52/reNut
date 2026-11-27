#include "nuttool/frontend.h"

#include <rex/ppc/hook.h>
#include <rex/types.h>
#include <atomic>
#include <string>

#include "nuttool/session.h"
#include "nuttool/studio.h"
#include "renut_engine/game/guest_call.h"
#include "renut_engine/game/menu_api.h"

REX_EXTERN(__imp__sub_825A0578);  // StartMenu_OnSelectionChanged(scene, source, {new, old} *)
REX_EXTERN(__imp__sub_825A06C0);  // StartMenu_HandleToolbarPress(scene, source, user)
REX_IMPORT(__imp__sub_8255E5B0, CurrentSelection, u32(u32));  // (list handle): its selected row

REX_IMPORT(__imp__rex_supportGameLoadNewScene, LoadNewScene, u32(u32, u32, u32, u32));  // (&scene aid, world, 0, 0)
REX_IMPORT(__imp__sub_82A25780, SetText, u32(u32, u32));                       // (text element, &UTF-16 text)
REX_IMPORT(__imp__sub_823B2178, ListFollowSelection, u32(u32, u32));          // (list, {new, old} *): scrolls to it
REX_IMPORT(__imp__rex_uiSystemSfxPlayOperation, PlayCue, u32(u32, u32));       // (cue table, index)
REX_IMPORT(__imp__sub_82447130, PlayAttachment, u32(u32, u32, u32, u32, u32, u32, u32));  // (house, prop, on, 1, ...)

namespace renut::nuttool {
namespace {

using renut::guest::Load32;
using renut::guest::Store16;
using renut::guest::Store32;

constexpr uint32_t kStartMenuVtable = 0x82158240;
// StartMenu: its icon bar (handle), the bar's list object, the name under it and Banjo's house behind it.
constexpr uint32_t kToolbar = 192;
constexpr uint32_t kAButton = 220;  // its A prompt (aButton), whose press opens the selected icon's page
constexpr uint32_t kToolbarList = 416;
constexpr uint32_t kLabel = 212;
constexpr uint32_t kHouse = 424;
// The house's prop playing for the selected icon, -1 for none.
constexpr uint32_t kHouseProp = 344;
constexpr uint32_t kCues = 0x82E19A70;
constexpr uint32_t kCueMove = 2;
// Set before a scene loads from the front end, so the world waits for the menu's work first.
constexpr uint32_t kFinishTasks = 0x82E0B5A0;
constexpr uint32_t kRunningWorld = 0x82E19AB4;

std::atomic<bool> g_open{false};

// From the NUTTOOL icon's press: the studio loads as the main menu's own choices load theirs.
void OpenStudio() {
  const renut::game::menu_api::Press *press = renut::game::menu_api::CurrentPress();
  if (!StudioReady() || !press || !press->scene || Load32(press->base, press->scene) != kStartMenuVtable)
    return;
  uint8_t *base = press->base;
  renut::guest::Guest g(*press->ctx, base);
  const uint32_t scene = StudioScene();
  const uint32_t aid = g.Push(&scene, 4);
  Store32(base, aid, scene);
  Store32(base, kFinishTasks, 1);
  g_open = true;
  LoadNewScene(g.ctx(), base, aid, Load32(base, kRunningWorld), 0, 0);
}

} // namespace

void Declare() {
  // Part of reNut: a row of its own, not a mod's, so no script or setting takes it away.
  renut::game::menu_api::AddBuiltinRow("StartMenu/toolBarList", "NUTTOOL", IconId(), [] { OpenStudio(); });
  StartSession();
}

bool Opened() { return g_open.load(); }

void SetOpened(bool open) { g_open = open; }

// The icon bar's selection on a mod's icon: its name under the bar, the move sound, the house's prop put away. The
// game's own part reads tables by the icon's index, which a mod's icon is past the end of.
bool SelectModIcon(PPCContext &ctx, uint8_t *base, uint32_t scene, uint32_t selection) {
  // XN_SELCHANGED's {new item, old item}.
  const uint32_t list = Load32(base, scene + kToolbarList);
  const int32_t to = int32_t(Load32(base, selection));
  std::u16string text;
  if (!list || int32_t(Load32(base, selection + 4)) == to || !renut::game::menu_api::ModRow(base, list, to, &text))
    return false;
  renut::guest::Guest g(ctx, base);
  ListFollowSelection(g.ctx(), base, list, selection);
  const uint32_t wide = g.Alloc(uint32_t(2 * (text.size() + 1)));
  for (size_t i = 0; i < text.size(); ++i)
    Store16(base, uint32_t(wide + 2 * i), uint16_t(text[i]));
  if (const uint32_t label = Load32(base, scene + kLabel))
    SetText(g.ctx(), base, label, wide);
  PlayCue(g.ctx(), base, kCues, kCueMove);
  if (const uint32_t house = Load32(base, scene + kHouse)) {
    const uint32_t prop = Load32(base, house + kHouseProp);
    if (prop != 0xFFFFFFFFu)
      PlayAttachment(g.ctx(), base, house, prop, 0, 1, 0, 0, 0);
    Store32(base, house + kHouseProp, 0xFFFFFFFFu);
  }
  return true;
}

} // namespace renut::nuttool

// The icon bar's buttons: A on a mod's icon is its press (the game's would open a page past its own seven).
REX_HOOK_RAW(sub_825A06C0) {
  using renut::guest::Load32;
  namespace n = renut::nuttool;
  const uint32_t scene = ctx.r3.u32, source = ctx.r4.u32, user = ctx.r5.u32;
  const auto r3 = ctx.r3, r4 = ctx.r4, r5 = ctx.r5;
  bool ours = false;
  if (scene && source && source == Load32(base, scene + n::kAButton)) {
    const uint32_t toolbar = Load32(base, scene + n::kToolbar);
    int32_t selected = -1;
    {
      renut::guest::Guest g(ctx, base);
      selected = int32_t(CurrentSelection(g.ctx(), base, toolbar));
    }
    ours = renut::game::menu_api::PressModRow(ctx, base, Load32(base, scene + n::kToolbarList), selected, user, scene);
  }
  if (ours) {
    ctx.r3.u64 = 1;
    return;
  }
  ctx.r3 = r3;
  ctx.r4 = r4;
  ctx.r5 = r5;
  __imp__sub_825A06C0(ctx, base);
}

// The main menu's selection moved: a mod's icon is handled above, then the game's handler runs without the move.
REX_HOOK_RAW(sub_825A0578) {
  using renut::guest::Load32;
  const uint32_t scene = ctx.r3.u32;
  const uint32_t selection = ctx.r5.u32;
  const auto r3 = ctx.r3, r4 = ctx.r4, r5 = ctx.r5;
  const bool ours = scene && selection && ctx.r4.u32 == Load32(base, scene + renut::nuttool::kToolbar) &&
                    renut::nuttool::SelectModIcon(ctx, base, scene, selection);
  ctx.r3 = r3;
  ctx.r4 = r4;
  ctx.r5 = r5;
  if (!ours) {
    __imp__sub_825A0578(ctx, base);
    return;
  }
  // The old item made the new one, so the handler sees no move.
  const uint32_t old = Load32(base, selection + 4);
  renut::guest::Store32(base, selection + 4, Load32(base, selection));
  __imp__sub_825A0578(ctx, base);
  renut::guest::Store32(base, selection + 4, old);
}
