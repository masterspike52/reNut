/**
 * @file    game/graphics_menu.cpp
 * @brief   GRAPHICS in N&B's Display Settings: a submenu of reNut's graphics options.
 *
 * N&B (IDA): Display Settings is one scene (26, class DisplaySettings) that
 * the pause screen (tab 5, page object at pause + 376) and the main menu
 * (state 5, + 348) both open. Its init (sub_8257D100) finds the brightness
 * and contrast sliders (+ 192, + 196; XuiSliders with the visual
 * list_pause_menu_slider_item, labelled by message 0x7E0 through
 * sub_825679E0), shows them disabled and focuses brightness; up and down move
 * between them through their NavUp / NavDown ids (sub_82567928). Pressing A
 * (pause sub_825A87B0, main sub_825BB698) edits the focused slider: it is
 * enabled and its navigation cleared until A again (sub_8257D290 puts the
 * navigation back). sub_8257D370 sets the A prompt for the focused slider.
 *
 * Game Information is scene 27 (class GameInformation; pause tab 4, main
 * state 2). Its init (sub_8257D908) fills its menu list (+ 200, class
 * GameInformationMenuList, one row per entry, row text from the list's
 * sub_8257F290) and leaves it focused in state 5 (+ 320); its other states
 * show a panel and disable the host's tab list (+ 220: the pause's tabs, the
 * main menu's list), which keeps the host's B and tab switching off, and B
 * (its backButton, + 228) returns to the menu list. Its presses come to
 * sub_8257DE50; the hidden left / right buttons (+ 232, + 236) are disabled
 * in state 5. + 260 is the host's subtitle text, + 328 a copy of what it
 * said, + 304 the prompt bar (sub_82571B30 / BA0 / C10 / C80: A, B, X, Y).
 * A list's rows are 3024-byte records (+ 8: column 0's text, wchar_t[128],
 * then + 268 = 0, + 272 its length), filled when shown while + 3016 is 0.
 *
 * Here Display Settings gets a third row under contrast, GRAPHICS: a button
 * made at init with Game Information's menu item visual (list_pause_menu_item,
 * its text set by XM_SET_TEXT 0x7E0), linked into the navigation, the three
 * rows respaced to fit the box. A on it opens Game Information the way the
 * host would (pause sub_825A7D90(4), main sub_825BA3C0(2)), and that page's
 * init turns it into the graphics menu: its menu list lists the options as
 * "NAME: VALUE", the host's tabs are disabled as in its other states, and the
 * left / right buttons are enabled. A or right moves the selected option to
 * its next value, left to its previous; each change applies at once and is
 * saved to the config. B goes back (pause sub_825A7D90(5), main
 * sub_825BA3C0(5)) with GRAPHICS focused. The host is switched from its own
 * code only (its press handler or its update), never from the page's: the
 * switch destroys the page. Display Settings' name, text key
 * pause_displaysettings (the pause tab's title and the main menu's entry,
 * looked up by sub_825EF418), reads Display/Graphics.
 */
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include "renut_engine/app/renut_logging.h"
#include "renut_engine/game/graphics_menu.h"

// Hooked.
REX_EXTERN(__imp__sub_8257D100);  // DisplaySettings init
REX_EXTERN(__imp__sub_8257D290);  // DisplaySettings: stop editing a slider
REX_EXTERN(__imp__sub_8257D370);  // DisplaySettings prompts
REX_EXTERN(__imp__sub_825A87B0);  // pause: press on the Display Settings tab
REX_EXTERN(__imp__sub_825BB698);  // main menu: press in Display Settings
REX_EXTERN(__imp__sub_8257D908);  // GameInformation init
REX_EXTERN(__imp__sub_8257DE50);  // GameInformation press
REX_EXTERN(__imp__sub_8257F290);  // GameInformationMenuList row text
REX_EXTERN(__imp__sub_825A76E0);  // pause press
REX_EXTERN(__imp__sub_825A7550);  // pause update
REX_EXTERN(__imp__sub_825BACD8);  // main menu press
REX_EXTERN(__imp__sub_825BA750);  // main menu update
REX_EXTERN(__imp__sub_825EF418);  // text lookup (table, key, wchar_t *out, out length, flag)
REX_EXTERN(__imp__sub_823E5728);  // a page's navigation (page, XUIMessageControlNavigate *, handled *)
REX_EXTERN(__imp__sub_829CA2A8);  // XuiElementSetBounds(h, float width, float height)

// Called.
REX_IMPORT(__imp__sub_829C27F8, XuiCreateObject, u32(u32, u32));
REX_IMPORT(__imp__sub_829C7F88, XuiElementAddChild, u32(u32, u32));
REX_IMPORT(__imp__sub_8225B210, XuiElementGetParent, u32(u32, u32));
REX_IMPORT(__imp__sub_8226E7E0, XuiElementGetPosition, u32(u32, u32));
REX_IMPORT(__imp__sub_82267268, XuiElementSetPosition, u32(u32, u32));
REX_IMPORT(__imp__sub_82279F88, XuiElementGetChildById, u32(u32, u32, u32));
REX_IMPORT(__imp__sub_82220068, XuiControlGetVisual, u32(u32, u32));
REX_IMPORT(__imp__sub_821FB9C8, XuiObjectGetPropertyId, u32(u32, u32, u32));
REX_IMPORT(__imp__sub_829BB868, XuiObjectSetProperty, u32(u32, u32, u32, u32));
REX_IMPORT(__imp__sub_822A2420, XuiObjectGetProperty, u32(u32, u32, u32, u32));
REX_IMPORT(__imp__sub_8225B168, XuiObjectFromHandle, u32(u32, u32));
REX_IMPORT(__imp__sub_8225B330, XuiSendMessage, u32(u32, u32));
REX_IMPORT(__imp__sub_829CB4A8, XuiElementSetFocus, u32(u32, u32));
REX_IMPORT(__imp__sub_82295738, XuiElementGetUserFocus, u32(u32));
REX_IMPORT(__imp__sub_829C87A0, XuiTextElementSetText, u32(u32, u32));
REX_IMPORT(__imp__sub_829D2E48, XuiListDeleteItems, u32(u32, u32, u32));
REX_IMPORT(__imp__sub_8258CC78, XuiContainerSetCurSel, u32(u32, u32));
REX_IMPORT(__imp__sub_8257AD00, XuiContainerGetCurSel, u32(u32));
REX_IMPORT(__imp__sub_823E3950, XuiContainerGetItemControl, u32(u32, u32));
REX_IMPORT(__imp__sub_829BFB08, XuiControlSetText, u32(u32, u32));
REX_IMPORT(__imp__sub_823E43D8, ListInsertItems, u32(u32, u32));          // (list object, count)
REX_IMPORT(__imp__sub_8251E748, ElementShow, u32(u32, u32));              // XM 0x12
REX_IMPORT(__imp__sub_823E4988, ElementEnable, u32(u32, u32));            // XM_SET_ENABLE
REX_IMPORT(__imp__sub_82567928, SetNavUpDown, void(u32, u32, u32));       // (h, NavUp id, NavDown id)
REX_IMPORT(__imp__sub_825679E0, MakeLabelMessage, void(u32, u32, u32, u32, u32, u32));  // (msg, data, 0, 0, 0, text)
REX_IMPORT(__imp__sub_829C8828, XuiImageElementSetImagePath, u32(u32, u32));
REX_IMPORT(__imp__sub_82567890, AddAssetTexture, u32(u32, u32, u32, u32));  // (texture asset, name, HXUITEX *, 0)
REX_IMPORT(__imp__gameAssetRefGetAid, GameAssetRefGetAid, u32(u32, u32, u32));
REX_IMPORT(__imp__assetManOpenParams, AssetManOpenParams, u32(u32, u32));
REX_IMPORT(__imp__sub_822C1228, AssetLoaded, u32(u32));  // (const u32 *aid): a loaded bundle has it
REX_IMPORT(__imp__sub_82571B30, PromptA, void(u32, u32));
REX_IMPORT(__imp__sub_82571BA0, PromptB, void(u32, u32));
REX_IMPORT(__imp__sub_82571C10, PromptX, void(u32, u32));
REX_IMPORT(__imp__sub_82571C80, PromptY, void(u32, u32));
REX_IMPORT(__imp__sub_825A7D90, PauseShowPage, void(u32, u32));
REX_IMPORT(__imp__sub_825BA3C0, MainShowPage, void(u32, u32));
REX_IMPORT(__imp__uiSystemSfxPlayOperation, PlayUiSound, void(u32, u32));

namespace {

// Guest data.
constexpr uint32_t kSounds = 0x82FAC768;    // uiSystemSfxPlayOperation's sounds: 0 select, 1 back, 2 move
constexpr uint32_t kKeySelect = 0x821697F4;  // "select"
constexpr uint32_t kKeyBack = 0x82135688;    // "back"
constexpr uint32_t kKeyNone = 0x8212ED2D;    // ""
constexpr uint32_t kWideEmpty = 0x82000B34;  // L""
constexpr uint32_t kTvIconAsset = 0x82178500;  // "pausemenudisplaysettings": the Display Settings tab's TV
constexpr uint32_t kNone = 0xFFFFFFFF;

// Scene objects (the page object of a scene: + 4 its HXUIOBJ, + 104 its user).
constexpr uint32_t kUser = 104;
constexpr uint32_t kPageLastFocus = 120;  // set by sub_823E5728 to where it navigated
// DisplaySettings.
constexpr uint32_t kDisplayBrightness = 192;
constexpr uint32_t kDisplayContrast = 196;
constexpr uint32_t kDisplayEditing = 208;
constexpr uint32_t kDisplayPrompts = 212;
// GameInformation.
constexpr uint32_t kInfoMenuList = 200;
constexpr uint32_t kInfoTabs = 220;
constexpr uint32_t kInfoA = 224;
constexpr uint32_t kInfoBack = 228;
constexpr uint32_t kInfoLeft = 232;
constexpr uint32_t kInfoRight = 236;
constexpr uint32_t kInfoSubtitle = 260;
constexpr uint32_t kInfoPrompts = 304;
constexpr uint32_t kInfoSubtitleCopy = 328;
// List objects: + 4 HXUIOBJ, + 8 the row vector (u16 row size, then begin,
// end), + 28 the row count it inserts.
constexpr uint32_t kListHandle = 4;
constexpr uint32_t kListRows = 8;
constexpr uint32_t kListCount = 28;
// Pause.
constexpr uint32_t kPauseA = 248;
constexpr uint32_t kPauseBack = 256;
constexpr uint32_t kPauseDisplayPage = 376;
constexpr uint32_t kPauseEditing = 448;
constexpr uint32_t kPausePageInfo = 4;
constexpr uint32_t kPausePageDisplay = 5;
constexpr uint32_t kPauseBackKey = 0x82178A7C;  // "resumegame": the B prompt it gives Game Information
// Main menu.
constexpr uint32_t kMainA = 220;
constexpr uint32_t kMainBack = 228;
constexpr uint32_t kMainEditing = 304;
constexpr uint32_t kMainDisplayPage = 348;
constexpr uint32_t kMainState = 388;
constexpr uint32_t kMainStateInfo = 2;
constexpr uint32_t kMainStateDisplay = 5;
constexpr uint32_t kMainBackKey = kKeyBack;

// Display Settings' name (the pause tab's title, the main menu's entry).
constexpr const char kDisplayNameKey[] = "pause_displaysettings";
constexpr const char kDisplayName[] = "Display/Graphics";

// The TV on GRAPHICS, by the row's height: as big and as far in as the
// sliders' icons. XuiImage SizeMode 0x10: stretched to fit, aspect kept,
// centred (ComputeBrushXFormForObject; 4 stretches, 8 fits from the corner).
constexpr float kIconSize = 0.9f;
constexpr float kIconLeft = 0.05f;
constexpr float kIconRaise = 0.1f;  // above the middle, level with the name
constexpr uint32_t kSizeModeFitCentred = 0x10;

constexpr const char kRowId[] = "renutGraphicsRow";
constexpr const char kRowVisual[] = "list_pause_menu_item";
constexpr const char kTitle[] = "GRAPHICS";

struct Option {
  const char *cvar;
  const char *label;
};

constexpr Option kOptions[] = {
    {"renut_resolution", "Resolution"},
    {"renut_field_of_view", "Field of View"},
    {"renut_shadow_resolution", "Shadow Resolution"},
    {"renut_anisotropic_filtering", "Anisotropic Filtering"},
    {"renut_ambient_occlusion", "Ambient Occlusion"},
    {"renut_lod_distance", "Detail Distance"},
    {"renut_grass_distance", "Grass Distance"},
    {"renut_depth_of_field", "Depth of Field"},
    {"renut_bloom", "Bloom"},
    {"renut_msaa", "MSAA"},
    {"renut_anti_aliasing", "Anti-Aliasing"},
    {"renut_vsync", "V-Sync"},
    {"frame_cap", "Frame Cap"},
};
constexpr uint32_t kOptionCount = uint32_t(std::size(kOptions));

enum class Host { kPause, kMain };

std::filesystem::path g_config_path;
// Display Settings' GRAPHICS row, and its page.
uint32_t g_display_page = 0;
uint32_t g_row = 0;
bool g_focus_row = false;  // focus GRAPHICS when Display Settings next opens
// The graphics menu: asked for, then open on this Game Information page.
bool g_open_pending = false;
Host g_host = Host::kPause;
uint32_t g_host_object = 0;
uint32_t g_info_page = 0;
uint32_t g_info_list = 0;
bool g_exit_requested = false;  // B on the page: the host goes back on its next update

uint32_t Load32(const uint8_t *base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return rex::byte_swap(v);
}

uint16_t Load16(const uint8_t *base, uint32_t va) {
  uint16_t v;
  std::memcpy(&v, base + va, 2);
  return rex::byte_swap(v);
}

void Store32(uint8_t *base, uint32_t va, uint32_t value) {
  value = rex::byte_swap(value);
  std::memcpy(base + va, &value, 4);
}

void Store16(uint8_t *base, uint32_t va, uint16_t value) {
  value = rex::byte_swap(value);
  std::memcpy(base + va, &value, 2);
}

float LoadFloat(const uint8_t *base, uint32_t va) {
  const uint32_t bits = Load32(base, va);
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}

void StoreFloat(uint8_t *base, uint32_t va, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  Store32(base, va, bits);
}

bool Failed(uint32_t hr) { return int32_t(hr) < 0; }

// Guest calls from a hook: an isolated context whose stack pointer sits under
// the scratch space handed out (out-parameters, structs), leaving a callee
// room to write its caller's parameter area.
class Guest {
 public:
  Guest(PPCContext &ctx, uint8_t *base) : frame_(ctx), base_(base), low_(ctx.r1.u32 - kGap) {
    frame_.ctx.r1.u32 = low_ - kCalleeArea;
  }

  // Zeroed scratch space for the calls made through this.
  uint32_t Alloc(uint32_t size) {
    low_ -= (size + 15) & ~15u;
    std::memset(base_ + low_, 0, size);
    frame_.ctx.r1.u32 = low_ - kCalleeArea;
    return low_;
  }

  rex::CallFrame &frame() { return frame_; }

 private:
  static constexpr uint32_t kGap = 0x100;
  static constexpr uint32_t kCalleeArea = 0x60;
  rex::CallFrame frame_;
  uint8_t *base_;
  uint32_t low_;
};

// A wide (UTF-16, big-endian) copy of an ASCII string in guest memory, made
// once and kept.
uint32_t Wide(uint8_t *base, const std::string &text) {
  static std::unordered_map<std::string, uint32_t> strings;
  if (auto it = strings.find(text); it != strings.end())
    return it->second;
  const uint32_t va = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(2 * (text.size() + 1)));
  if (!va)
    return kWideEmpty;
  for (size_t i = 0; i < text.size(); ++i)
    Store16(base, uint32_t(va + 2 * i), uint8_t(text[i]));
  Store16(base, uint32_t(va + 2 * text.size()), 0);
  strings.emplace(text, va);
  return va;
}

std::string Upper(std::string s) {
  for (char &c : s)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

const rex::cvar::FlagEntry *FindFlag(const char *name) {
  for (const auto &entry : rex::cvar::GetRegistry())
    if (entry.name == name)
      return &entry;
  return nullptr;
}

std::vector<std::string> Values(const rex::cvar::FlagEntry &entry) {
  if (entry.type == rex::cvar::FlagType::Boolean)
    return {"false", "true"};
  return entry.constraints.allowed_values;
}

// "RESOLUTION: 1080P".
std::string RowText(const Option &option) {
  const rex::cvar::FlagEntry *entry = FindFlag(option.cvar);
  if (!entry)
    return Upper(option.label);
  std::string value = rex::cvar::GetFlagByName(option.cvar);
  if (entry->type == rex::cvar::FlagType::Boolean)
    value = value == "true" || value == "1" ? "On" : "Off";
  return Upper(std::string(option.label) + ": " + value);
}

// Moves the option to its next (dir 1) or previous (dir -1) value, applies it
// and saves the config.
bool StepOption(const Option &option, int dir) {
  const rex::cvar::FlagEntry *entry = FindFlag(option.cvar);
  if (!entry)
    return false;
  const std::vector<std::string> values = Values(*entry);
  if (values.size() < 2)
    return false;
  std::string current = rex::cvar::GetFlagByName(option.cvar);
  if (entry->type == rex::cvar::FlagType::Boolean)
    current = current == "true" || current == "1" ? "true" : "false";
  const int count = int(values.size());
  const auto it = std::find(values.begin(), values.end(), current);
  const int index = it == values.end() ? 0 : int(it - values.begin());
  const std::string &next = values[size_t(((index + dir) % count + count) % count)];
  if (!rex::cvar::SetFlagByName(option.cvar, next))
    return false;
  if (!g_config_path.empty())
    rex::cvar::SaveConfig(g_config_path);
  return true;
}

// Column 0 of a list row record, as sub_823E4A38 writes it.
void WriteRowText(uint8_t *base, uint32_t row, const std::string &text) {
  const size_t length = std::min<size_t>(text.size(), 127);
  for (size_t i = 0; i < length; ++i)
    Store16(base, uint32_t(row + 8 + 2 * i), uint8_t(text[i]));
  Store16(base, uint32_t(row + 8 + 2 * length), 0);
  Store32(base, row + 268, 0);
  Store32(base, row + 272, uint32_t(length));
}

void SetStringProperty(Guest &g, uint8_t *base, uint32_t obj, const char *name, const char *value) {
  auto &f = g.frame();
  const uint32_t id = g.Alloc(4);
  const uint32_t val = g.Alloc(20);
  if (Failed(XuiObjectGetPropertyId(f, base, obj, Wide(base, name), id)))
    return;
  Store32(base, val, 5);  // XUI_EPT_STRING
  Store32(base, val + 4, Wide(base, value));
  XuiObjectSetProperty(f, base, obj, Load32(base, id), 0, val);
}

void SetUnsignedProperty(Guest &g, uint8_t *base, uint32_t obj, const char *name, uint32_t value) {
  auto &f = g.frame();
  const uint32_t id = g.Alloc(4);
  const uint32_t val = g.Alloc(20);
  if (Failed(XuiObjectGetPropertyId(f, base, obj, Wide(base, name), id)))
    return;
  Store32(base, val, 3);  // XUI_EPT_UNSIGNED
  Store32(base, val + 4, value);
  XuiObjectSetProperty(f, base, obj, Load32(base, id), 0, val);
}

float FloatProperty(Guest &g, uint8_t *base, uint32_t obj, const char *name) {
  auto &f = g.frame();
  const uint32_t id = g.Alloc(4);
  const uint32_t val = g.Alloc(20);
  if (Failed(XuiObjectGetPropertyId(f, base, obj, Wide(base, name), id)) ||
      Failed(XuiObjectGetProperty(f, base, obj, Load32(base, id), 0, val)) ||
      Load32(base, val) != 4)  // XUI_EPT_FLOAT
    return 0.0f;
  return LoadFloat(base, val + 4);
}

uint32_t UserFocus(Guest &g, uint8_t *base, uint32_t page) {
  return XuiElementGetUserFocus(g.frame(), base, base[page + kUser]);
}

// The Display Settings tab's TV as a named XUI texture, the way sub_82567AA8
// gives the sliders their icons (the asset kept open, like theirs); made once,
// once its bundle is loaded (opening a missing asset stops the game with the
// dirty disc error).
uint32_t GraphicsIcon(Guest &g, uint8_t *base) {
  static bool made = false;
  static uint32_t name = 0;
  if (made)
    return name;
  auto &f = g.frame();
  const uint32_t aid = g.Alloc(16);
  const uint32_t texture = g.Alloc(4);
  if (!(GameAssetRefGetAid(f, base, kTvIconAsset, aid, 0) & 0xFF)) {
    made = true;
    RNUT_WARN("graphics menu: no asset named pausemenudisplaysettings for the TV icon");
    return 0;
  }
  if (!(AssetLoaded(f, base, aid) & 0xFF)) {
    RNUT_INFO("graphics menu: the TV icon isn't loaded yet (aid {:08X})", Load32(base, aid));
    return 0;
  }
  made = true;
  const uint32_t asset = AssetManOpenParams(f, base, aid, 0);
  const uint32_t icon = Wide(base, "tex://renutGraphicsIcon");
  if (!asset || Failed(AddAssetTexture(f, base, asset, icon, texture, 0))) {
    RNUT_WARN("graphics menu: couldn't make the TV icon");
    return 0;
  }
  RNUT_INFO("graphics menu: TV icon ready");
  name = icon;
  return name;
}

// GRAPHICS, under contrast: a button with Game Information's menu item visual
// (its rows' look: name only), the three rows spaced to fit the box.
void AddGraphicsRow(PPCContext &ctx, uint8_t *base, uint32_t page) {
  g_display_page = page;
  g_row = 0;
  Guest g(ctx, base);
  auto &f = g.frame();
  const uint32_t contrast = Load32(base, page + kDisplayContrast);
  const uint32_t brightness = Load32(base, page + kDisplayBrightness);
  const uint32_t out = g.Alloc(4);
  if (!contrast || !brightness || Failed(XuiElementGetParent(f, base, contrast, out)))
    return;
  const uint32_t parent = Load32(base, out);
  Store32(base, out, 0);
  XuiCreateObject(f, base, Wide(base, "XuiButton"), out);
  const uint32_t row = Load32(base, out);
  if (!row) {
    RNUT_WARN("graphics menu: couldn't make the GRAPHICS row");
    return;
  }
  XuiElementAddChild(f, base, parent, row);
  SetStringProperty(g, base, row, "Id", kRowId);
  SetStringProperty(g, base, row, "Visual", kRowVisual);

  // The box only fits two rows at the game's spacing: the three share the
  // span the two took, the first half a row higher, a row and the new gap
  // apart. GRAPHICS is as big as the sliders' rows.
  const float width = FloatProperty(g, base, contrast, "Width");
  const float height = FloatProperty(g, base, contrast, "Height");
  const uint32_t first = g.Alloc(12);
  const uint32_t position = g.Alloc(12);
  XuiElementGetPosition(f, base, brightness, first);
  XuiElementGetPosition(f, base, contrast, position);
  const float top = LoadFloat(base, first + 4);
  const float gap = LoadFloat(base, position + 4) - top;
  const float new_top = height > 0.0f ? top - height * 0.5f : top;
  const float spacing = height > 0.0f ? (gap + height) * 0.5f : gap * 0.5f;
  StoreFloat(base, first + 4, new_top);
  XuiElementSetPosition(f, base, brightness, first);
  StoreFloat(base, position + 4, new_top + spacing);
  XuiElementSetPosition(f, base, contrast, position);
  StoreFloat(base, position + 4, new_top + 2.0f * spacing);
  XuiElementSetPosition(f, base, row, position);
  if (width > 0.0f && height > 0.0f) {
    f.ctx.r3.u64 = row;
    f.ctx.f1.f64 = width;
    f.ctx.f2.f64 = height;
    __imp__sub_829CA2A8(f.ctx, base);
  }

  // XuiControlSetText's message.
  const uint32_t message = g.Alloc(0x20);
  const uint32_t data = g.Alloc(0x30);
  MakeLabelMessage(f, base, message, data, 0, 0, 0, Wide(base, kTitle));
  XuiSendMessage(f, base, row, message);
  // The visual takes its icon from a list's second column, which a button
  // doesn't have: the TV is an image of its own on the button, where the
  // sliders' icons are, scaled to fit.
  const uint32_t icon = GraphicsIcon(g, base);
  Store32(base, out, 0);
  if (icon && height > 0.0f) {
    XuiCreateObject(f, base, Wide(base, "XuiImage"), out);
  }
  if (const uint32_t image = Load32(base, out)) {
    XuiElementAddChild(f, base, row, image);
    const float side = height * kIconSize;
    StoreFloat(base, position, height * kIconLeft);
    StoreFloat(base, position + 4, (height - side) * 0.5f - height * kIconRaise);
    StoreFloat(base, position + 8, 0.0f);
    XuiElementSetPosition(f, base, image, position);
    f.ctx.r3.u64 = image;
    f.ctx.f1.f64 = side;
    f.ctx.f2.f64 = side;
    __imp__sub_829CA2A8(f.ctx, base);
    SetUnsignedProperty(g, base, image, "SizeMode", kSizeModeFitCentred);
    XuiImageElementSetImagePath(f, base, image, icon);
  }

  SetNavUpDown(f, base, row, Wide(base, "displayContrastSlider"), kWideEmpty);
  SetNavUpDown(f, base, contrast, Wide(base, "displayBrightnessSlider"), Wide(base, kRowId));
  g_row = row;

  if (g_focus_row) {
    g_focus_row = false;
    XuiElementSetFocus(f, base, row, base[page + kUser]);
  }
}

bool RowFocused(PPCContext &ctx, uint8_t *base, uint32_t page) {
  if (!g_row || !page || page != g_display_page)
    return false;
  Guest g(ctx, base);
  return UserFocus(g, base, page) == g_row;
}

// The menu list's rows: one per option, the selection on 'selected'. The rows
// are made anew (the game's way, sub_8257F610), so the list is focused again
// as the init does: the focused row went with the old ones.
void FillList(Guest &g, uint8_t *base, uint32_t list, uint32_t selected, uint32_t user) {
  auto &f = g.frame();
  const uint32_t handle = Load32(base, list + kListHandle);
  const uint32_t rows = Load32(base, list + kListRows);
  const uint32_t row_size = Load16(base, rows);
  const int32_t count = row_size ? int32_t(Load32(base, rows + 8) - Load32(base, rows + 4)) / int32_t(row_size) : 0;
  if (count > 0)
    XuiListDeleteItems(f, base, handle, 0, uint32_t(count));
  Store32(base, list + kListCount, kOptionCount);
  ListInsertItems(f, base, list, kOptionCount);
  XuiContainerSetCurSel(f, base, handle, selected);
  XuiElementSetFocus(f, base, handle, user);
}

void SetInfoPrompts(Guest &g, uint8_t *base, uint32_t page) {
  auto &f = g.frame();
  const uint32_t prompts = Load32(base, page + kInfoPrompts);
  if (prompts == kNone)
    return;
  PromptA(f, base, prompts, kKeySelect);
  PromptB(f, base, prompts, kKeyBack);
  PromptX(f, base, prompts, kKeyNone);
  PromptY(f, base, prompts, kKeyNone);
}

// Game Information as the graphics menu.
void SetUpGraphicsPage(PPCContext &ctx, uint8_t *base, uint32_t page) {
  Guest g(ctx, base);
  auto &f = g.frame();
  const uint32_t out = g.Alloc(4);
  XuiObjectFromHandle(f, base, Load32(base, page + kInfoMenuList), out);
  const uint32_t list = Load32(base, out);
  if (!list) {
    RNUT_WARN("graphics menu: Game Information has no menu list");
    return;
  }
  g_info_page = page;
  g_info_list = list;
  g_exit_requested = false;
  FillList(g, base, list, 0, base[page + kUser]);
  ElementEnable(f, base, Load32(base, page + kInfoTabs), 0);
  ElementEnable(f, base, Load32(base, page + kInfoLeft), 1);
  ElementEnable(f, base, Load32(base, page + kInfoRight), 1);
  XuiTextElementSetText(f, base, Load32(base, page + kInfoSubtitle), Wide(base, kTitle));
  SetInfoPrompts(g, base, page);
}

void OpenGraphics(PPCContext &ctx, uint8_t *base, Host host, uint32_t host_object) {
  g_open_pending = true;
  g_host = host;
  g_host_object = host_object;
  g_row = 0;
  g_display_page = 0;
  Guest g(ctx, base);
  auto &f = g.frame();
  if (host == Host::kPause) {
    PlayUiSound(f, base, kSounds, 0);
    PauseShowPage(f, base, host_object, kPausePageInfo);  // the page opens now
    g_open_pending = false;
    // Its builder sets the pause's prompts after the page's init.
    if (g_info_page)
      SetInfoPrompts(g, base, g_info_page);
  } else {
    MainShowPage(f, base, host_object, kMainStateInfo);  // the page opens on the next update
  }
}

// Back to Display Settings, from the host's own code.
void ExitGraphics(PPCContext &ctx, uint8_t *base) {
  const uint32_t page = g_info_page;
  if (!page)
    return;
  g_info_page = 0;
  g_info_list = 0;
  g_exit_requested = false;
  g_focus_row = true;
  Guest g(ctx, base);
  auto &f = g.frame();
  ElementEnable(f, base, Load32(base, page + kInfoTabs), 1);
  XuiTextElementSetText(f, base, Load32(base, page + kInfoSubtitle), page + kInfoSubtitleCopy);
  if (g_host == Host::kPause) {
    PlayUiSound(f, base, kSounds, 1);
    PauseShowPage(f, base, g_host_object, kPausePageDisplay);
  } else {
    MainShowPage(f, base, g_host_object, kMainStateDisplay);
  }
}

void ChangeOption(PPCContext &ctx, uint8_t *base, uint32_t page, int dir) {
  Guest g(ctx, base);
  auto &f = g.frame();
  const uint32_t selected = XuiContainerGetCurSel(f, base, Load32(base, page + kInfoMenuList));
  if (selected >= kOptionCount || !StepOption(kOptions[selected], dir))
    return;
  PlayUiSound(f, base, kSounds, 2);
  // Only this row changes, in place: making the rows anew loses the list's
  // scrolling (the rows below the first screen went blank). Its record (what
  // the list reads when the row comes into view) and, when it's on screen,
  // its control.
  const uint32_t handle = Load32(base, g_info_list + kListHandle);
  const uint32_t rows = Load32(base, g_info_list + kListRows);
  const uint32_t row = Load32(base, rows + 4) + Load16(base, rows) * selected;
  WriteRowText(base, row, RowText(kOptions[selected]));
  if (const uint32_t control = XuiContainerGetItemControl(f, base, handle, selected))
    XuiControlSetText(f, base, control, row + 8);
}

}  // namespace

namespace renut::graphics_menu {

void SetConfigPath(const std::filesystem::path &path) { g_config_path = path; }

}  // namespace renut::graphics_menu

REX_HOOK_RAW(sub_8257D100) {
  const uint32_t page = ctx.r3.u32;
  __imp__sub_8257D100(ctx, base);
  AddGraphicsRow(ctx, base, page);
}

// Leaving a slider's editing puts back its navigation: contrast's down is GRAPHICS.
REX_HOOK_RAW(sub_8257D290) {
  const uint32_t page = ctx.r3.u32;
  const uint32_t editing = ctx.r4.u32;
  __imp__sub_8257D290(ctx, base);
  if (editing || !g_row || page != g_display_page)
    return;
  Guest g(ctx, base);
  SetNavUpDown(g.frame(), base, Load32(base, page + kDisplayContrast), Wide(base, "displayBrightnessSlider"),
               Wide(base, kRowId));
}

REX_HOOK_RAW(sub_8257D370) {
  const uint32_t page = ctx.r3.u32;
  __imp__sub_8257D370(ctx, base);
  if (!g_row || page != g_display_page || Load32(base, page + kDisplayEditing))
    return;
  const uint32_t prompts = Load32(base, page + kDisplayPrompts);
  if (prompts == kNone)
    return;
  Guest g(ctx, base);
  if (UserFocus(g, base, page) == g_row)
    PromptA(g.frame(), base, prompts, kKeySelect);
}

REX_HOOK_RAW(sub_825A87B0) {
  const uint32_t pause = ctx.r3.u32;
  const uint32_t pressed = ctx.r4.u32;
  if ((pressed == Load32(base, pause + kPauseA) || (g_row && pressed == g_row)) &&
      !Load32(base, pause + kPauseEditing) &&
      RowFocused(ctx, base, Load32(base, pause + kPauseDisplayPage))) {
    OpenGraphics(ctx, base, Host::kPause, pause);
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_825A87B0(ctx, base);
}

REX_HOOK_RAW(sub_825BB698) {
  const uint32_t menu = ctx.r3.u32;
  const uint32_t pressed = ctx.r4.u32;
  if ((pressed == Load32(base, menu + kMainA) || (g_row && pressed == g_row)) &&
      !Load32(base, menu + kMainEditing) &&
      RowFocused(ctx, base, Load32(base, menu + kMainDisplayPage))) {
    OpenGraphics(ctx, base, Host::kMain, menu);
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_825BB698(ctx, base);
}

REX_HOOK_RAW(sub_8257D908) {
  const uint32_t page = ctx.r3.u32;
  const uint32_t args = ctx.r4.u32;
  __imp__sub_8257D908(ctx, base);
  g_info_page = 0;
  g_info_list = 0;
  // The page the GRAPHICS row asked for: its host gives it that host's B prompt.
  const uint32_t back_key = g_host == Host::kPause ? kPauseBackKey : kMainBackKey;
  if (!g_open_pending || Load32(base, args + 4) != back_key)
    return;
  g_open_pending = false;
  SetUpGraphicsPage(ctx, base, page);
}

REX_HOOK_RAW(sub_8257DE50) {
  const uint32_t page = ctx.r3.u32;
  if (!page || page != g_info_page) {
    __imp__sub_8257DE50(ctx, base);
    return;
  }
  const uint32_t pressed = ctx.r4.u32;
  ctx.r3.u64 = 1;
  if (pressed == Load32(base, page + kInfoBack)) {
    g_exit_requested = true;
  } else if (pressed == Load32(base, page + kInfoLeft)) {
    ChangeOption(ctx, base, page, -1);
  } else if (pressed == Load32(base, page + kInfoMenuList) || pressed == Load32(base, page + kInfoA) ||
             pressed == Load32(base, page + kInfoRight)) {
    ChangeOption(ctx, base, page, 1);
  }
}

REX_HOOK_RAW(sub_8257F290) {
  if (!g_info_list || ctx.r3.u32 != g_info_list) {
    __imp__sub_8257F290(ctx, base);
    return;
  }
  const uint32_t index = ctx.r4.u32;
  if (index < kOptionCount)
    WriteRowText(base, ctx.r5.u32, RowText(kOptions[index]));
  ctx.r3.u64 = 1;
}

REX_HOOK_RAW(sub_825A76E0) {
  const uint32_t pause = ctx.r3.u32;
  if (g_info_page && g_host == Host::kPause && g_host_object == pause &&
      ctx.r4.u32 == Load32(base, pause + kPauseBack)) {
    ExitGraphics(ctx, base);
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_825A76E0(ctx, base);
}

REX_HOOK_RAW(sub_825A7550) {
  if (g_exit_requested && g_info_page && g_host == Host::kPause && g_host_object == ctx.r3.u32)
    ExitGraphics(ctx, base);
  __imp__sub_825A7550(ctx, base);
}

REX_HOOK_RAW(sub_825BACD8) {
  const uint32_t menu = ctx.r3.u32;
  if (g_info_page && g_host == Host::kMain && g_host_object == menu &&
      Load32(base, menu + kMainState) == kMainStateInfo && ctx.r4.u32 == Load32(base, menu + kMainBack)) {
    ExitGraphics(ctx, base);
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_825BACD8(ctx, base);
}

// A page's navigation (XUIMessageControlNavigate: + 4 from, + 8 to) skips
// disabled controls, and the sliders are disabled until edited: down from
// brightness would pass contrast for GRAPHICS. It goes to contrast.
REX_HOOK_RAW(sub_823E5728) {
  const uint32_t page = ctx.r3.u32;
  const uint32_t navigate = ctx.r4.u32;
  __imp__sub_823E5728(ctx, base);
  if (!g_row || page != g_display_page || Load32(base, navigate + 8) != g_row ||
      Load32(base, navigate + 4) != Load32(base, page + kDisplayBrightness))
    return;
  const uint32_t contrast = Load32(base, page + kDisplayContrast);
  Store32(base, navigate + 8, contrast);
  if (Load32(base, page + kPageLastFocus) == g_row)
    Store32(base, page + kPageLastFocus, contrast);
}

// Display Settings is called Display/Graphics.
REX_HOOK_RAW(sub_825EF418) {
  const uint32_t key = ctx.r4.u32;
  const uint32_t out = ctx.r5.u32;
  const int32_t out_length = int32_t(ctx.r6.u32);
  __imp__sub_825EF418(ctx, base);
  if (!key || !out || out_length <= 0 ||
      std::strncmp(reinterpret_cast<const char *>(base + key), kDisplayNameKey, sizeof(kDisplayNameKey)) != 0)
    return;
  const size_t length = std::min<size_t>(std::size(kDisplayName) - 1, size_t(out_length - 1));
  for (size_t i = 0; i < length; ++i)
    Store16(base, uint32_t(out + 2 * i), uint8_t(kDisplayName[i]));
  Store16(base, uint32_t(out + 2 * length), 0);
}

REX_HOOK_RAW(sub_825BA750) {
  const uint32_t menu = ctx.r3.u32;
  if (g_host == Host::kMain && g_host_object == menu) {
    if (g_exit_requested && g_info_page)
      ExitGraphics(ctx, base);
    else if (g_open_pending && Load32(base, menu + kMainState) != kMainStateInfo)
      g_open_pending = false;  // the menu went elsewhere before the page opened
  }
  __imp__sub_825BA750(ctx, base);
}
