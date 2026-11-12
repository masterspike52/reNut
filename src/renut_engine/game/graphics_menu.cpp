#include <rex/console_command.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/result.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/types.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "renut_engine/app/renut_settings.h"
#include "renut_engine/game/graphics_menu.h"
#include "renut_engine/game/guest_call.h"

// Hooked.
REX_EXTERN(__imp__sub_82560958);  // DisplaySettings init
REX_EXTERN(__imp__sub_82560C58);  // DisplaySettings: stop editing a row
REX_EXTERN(__imp__sub_82560DF8);  // DisplaySettings prompts
REX_EXTERN(__imp__sub_8258DA40);  // pause: press on the Display Settings tab
REX_EXTERN(__imp__sub_825A0F98);  // main menu: press in Display Settings
REX_EXTERN(__imp__sub_825613A8);  // GameInformation init
REX_EXTERN(__imp__sub_82561960);  // GameInformation press
REX_EXTERN(__imp__sub_82562CE8);  // GameInformationMenuList row text
REX_EXTERN(__imp__sub_8258C948);  // pause press
REX_EXTERN(__imp__sub_8258C7C0);  // pause update
REX_EXTERN(__imp__sub_8258B948);  // pause constructor (its visible tabs)
REX_EXTERN(__imp__sub_825A0458);  // main menu press
REX_EXTERN(__imp__sub_8259FDA8);  // main menu update
REX_EXTERN(__imp__sub_825D7F28);  // text lookup (table, key, wchar_t *out, out length, flag)
REX_EXTERN(__imp__sub_823B33E8);  // a page's navigation (page, XUIMessageControlNavigate *, handled *)

// Called.
REX_IMPORT(__imp__sub_82A27180, XuiElementSetBounds, u32(u32, f64, f64));  // (h, float width, float height)
REX_IMPORT(__imp__sub_82A1F6B8, XuiCreateObject, u32(u32, u32));
REX_IMPORT(__imp__sub_82A24F68, XuiElementAddChild, u32(u32, u32));
REX_IMPORT(__imp__sub_82229C90, XuiElementGetParent, u32(u32, u32));
REX_IMPORT(__imp__sub_8223D020, XuiElementGetPosition, u32(u32, u32));
REX_IMPORT(__imp__sub_82235C88, XuiElementSetPosition, u32(u32, u32));
REX_IMPORT(__imp__sub_82247F88, XuiElementGetChildById, u32(u32, u32, u32));
REX_IMPORT(__imp__sub_821EF288, XuiControlGetVisual, u32(u32, u32));
REX_IMPORT(__imp__sub_821CB260, XuiObjectGetPropertyId, u32(u32, u32, u32));
REX_IMPORT(__imp__sub_82A18700, XuiObjectSetProperty, u32(u32, u32, u32, u32));
REX_IMPORT(__imp__sub_82270150, XuiObjectGetProperty, u32(u32, u32, u32, u32));
REX_IMPORT(__imp__sub_82229BE8, XuiObjectFromHandle, u32(u32, u32));
REX_IMPORT(__imp__sub_82229DB0, XuiSendMessage, u32(u32, u32));
REX_IMPORT(__imp__sub_82A28368, XuiElementSetFocus, u32(u32, u32));
REX_IMPORT(__imp__sub_82263290, XuiElementGetUserFocus, u32(u32));
REX_IMPORT(__imp__sub_82A25780, XuiTextElementSetText, u32(u32, u32));
REX_IMPORT(__imp__sub_823B1758, XuiListDeleteItems, u32(u32, u32, u32));
REX_IMPORT(__imp__sub_82570CA0, XuiContainerSetCurSel, u32(u32, u32));
REX_IMPORT(__imp__sub_823B16C8, XuiListSetTopItem, u32(u32, u32));
REX_IMPORT(__imp__sub_8255E5B0, XuiContainerGetCurSel, u32(u32));
REX_IMPORT(__imp__sub_82A2FBA0, XuiContainerGetItemControl, u32(u32, u32));
REX_IMPORT(__imp__sub_82A1C9D0, XuiControlSetText, u32(u32, u32));
REX_IMPORT(__imp__sub_823B2130, ListInsertItems, u32(u32, u32));          // (list object, count)
REX_IMPORT(__imp__sub_824FC1B8, ElementShow, u32(u32, u32));              // XM 0x12
REX_IMPORT(__imp__sub_823B26F0, ElementEnable, u32(u32, u32));            // XM_SET_ENABLE
REX_IMPORT(__imp__sub_8254B0F0, SetNavUpDown, void(u32, u32, u32));       // (h, NavUp id, NavDown id)
REX_IMPORT(__imp__sub_8254B1A8, MakeLabelMessage, void(u32, u32, u32));  // (msg, data, text)
REX_IMPORT(__imp__sub_82A25808, XuiImageElementSetImagePath, u32(u32, u32));
REX_IMPORT(__imp__sub_8254B058, AddAssetTexture, u32(u32, u32, u32, u32));  // (texture asset, name, HXUITEX *, 0)
REX_IMPORT(__imp__rex_gameAssetRefGetAid, GameAssetRefGetAid, u32(u32, u32, u32));
REX_IMPORT(__imp__rex_assetManOpenParams, AssetManOpenParams, u32(u32, u32));
REX_IMPORT(__imp__sub_821DDBF0, AssetQuery, u32(u32, u32));  // ({vtable, aid} *, provider): a loaded bundle has it
REX_IMPORT(__imp__sub_82555490, PromptA, void(u32, u32));
REX_IMPORT(__imp__sub_82555500, PromptB, void(u32, u32));
REX_IMPORT(__imp__sub_82555570, PromptX, void(u32, u32));
REX_IMPORT(__imp__sub_825555E0, PromptY, void(u32, u32));
REX_IMPORT(__imp__sub_8258D038, PauseShowPage, void(u32, u32));
REX_IMPORT(__imp__sub_8259F940, MainShowPage, void(u32, u32));
REX_IMPORT(__imp__rex_uiSystemSfxPlayOperation, PlayUiSound, void(u32, u32));

namespace {

// Guest data.
constexpr uint32_t kSounds = 0x82E19A70;    // uiSystemSfxPlayOperation's sounds: 0 select, 1 back, 2 move
constexpr uint32_t kKeySelect = 0x82146174;  // "select"
constexpr uint32_t kKeyBack = 0x82111110;    // "back"
constexpr uint32_t kKeyNone = 0x8210A5E1;    // ""
constexpr uint32_t kWideEmpty = 0x82000B08;  // L""
constexpr uint32_t kTvIconAsset = 0x82155650;  // "pausemenudisplaysettings": the Display Settings tab's TV
constexpr uint32_t kAssetQueryVtable = 0x82001AF0;  // an asset query {vtable, aid}
constexpr uint32_t kAnyProvider = 0xFF;
constexpr uint32_t kNone = 0xFFFFFFFF;

// Scene objects (the page object of a scene: + 4 its HXUIOBJ, + 104 its user).
constexpr uint32_t kUser = 104;
constexpr uint32_t kPageLastFocus = 120;  // set by sub_823B33E8 to where it navigated
// DisplaySettings.
constexpr uint32_t kDisplayBrightness = 192;
constexpr uint32_t kDisplayContrast = 196;
constexpr uint32_t kDisplayBigFont = 204;      // useBigFontButton
constexpr uint32_t kDisplayBigFontTick = 208;  // its tick and tick box
constexpr uint32_t kDisplayBigFontTickBox = 212;
constexpr uint32_t kDisplayEditing = 220;
constexpr uint32_t kDisplayPrompts = 224;
constexpr uint32_t kDisplayNoBigFont = 232;  // the init's args[2]: the big font row hidden
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
constexpr uint32_t kPauseBackKey = 0x82155BCC;  // "resumegame": the B prompt it gives Game Information
constexpr uint32_t kPauseTabs = 0x82E1045C;     // the visible tabs' types
constexpr uint32_t kPauseTabCount = 0x82E10458;
constexpr uint32_t kPauseTabSlots = 8;
constexpr uint32_t kPauseTabRenut = 3;  // the unused town map tab
constexpr uint32_t kPauseBusy = 456;    // nonzero: sub_8258D038 shows no page
constexpr uint32_t kPauseToolBar = 192;  // toolBarList: the tab bar
// The game's button being pressed (set by sub_823AFCC0, cleared at release by
// sub_823B0110); while set no button takes a key.
constexpr uint32_t kButtonPressed = 0x82E19A34;
// XUIMessageControlNavigate: + 0 direction (0 up, 1 down, 2 left, 3 right,
// 4 / 5 tab forward / back), + 4 from, + 8 to, + 12 stay.
constexpr uint32_t kNavigateLeft = 2;
constexpr uint32_t kNavigateRight = 3;
// Main menu.
constexpr uint32_t kMainA = 220;
constexpr uint32_t kMainBack = 228;
constexpr uint32_t kMainEditing = 304;
constexpr uint32_t kMainDisplayPage = 352;
constexpr uint32_t kMainState = 420;
constexpr uint32_t kMainStateInfo = 2;
constexpr uint32_t kMainStateDisplay = 5;
constexpr uint32_t kMainBackKey = kKeyBack;

// Text the lookup answers differently: Display Settings' name (the pause
// tab's title, the main menu's entry) and the reNut tab's title.
struct Text {
  const char *key;
  const char *text;
};
constexpr Text kTexts[] = {
    {"pause_displaysettings", "DISPLAY/GRAPHICS"},
    {"pause_map", "RENUT SETTINGS"},
};
constexpr const char kRenutTitle[] = "RENUT SETTINGS";
// reNut's own settings' categories; Graphics has its own menu.
constexpr std::string_view kCategoryPrefix = "Nuts&Bolts/";
constexpr std::string_view kGraphicsCategory = "Nuts&Bolts/Graphics";
constexpr std::string_view kFirstCategory = "Nuts&Bolts/Cheats";

// The TV icon on GRAPHICS, sized by the row's height like the sliders' icons;
// XuiImage SizeMode 0x10 fits it centred with its aspect kept.
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
    {"renut_time_of_day", "Time of Day"},
    {"renut_shadow_resolution", "Shadow Resolution"},
    {"renut_shadow_softness", "Shadow Softness"},
    {"renut_anisotropic_filtering", "Anisotropic Filtering"},
    {"renut_ssao", "Ambient Occlusion"},
    {"renut_reflection_resolution", "Reflections"},
    {"renut_lod_distance", "Detail Distance"},
    {"renut_grass_distance", "Grass Distance"},
    {"renut_depth_of_field", "Depth of Field"},
    {"renut_bloom", "Bloom"},
    {"renut_msaa", "MSAA"},
    {"renut_alpha_to_coverage", "Smooth Cut-Outs"},
    {"renut_anti_aliasing", "Anti-Aliasing"},
    {"renut_vsync", "V-Sync"},
    {"frame_cap", "Frame Cap"},
};
enum class Host { kPause, kMain };
// What the Game Information page is made into: GRAPHICS (from Display
// Settings) or the reNut tab.
enum class Menu { kGraphics, kRenut };

// A row of the menu list.
struct Row {
  enum class Kind { kOption, kCommand, kCategory } kind;
  std::string name;   // the cvar, or the category ("Nuts&Bolts/Cheats")
  std::string label;  // as shown, before the value
};

std::filesystem::path g_config_path;
// Display Settings' GRAPHICS row, and its page.
uint32_t g_display_page = 0;
uint32_t g_row = 0;
bool g_focus_row = false;  // focus GRAPHICS when Display Settings next opens
// The graphics menu: asked for, then open on this Game Information page.
bool g_open_pending = false;
Menu g_menu = Menu::kGraphics;
Host g_host = Host::kPause;
uint32_t g_host_object = 0;
uint32_t g_info_page = 0;
uint32_t g_info_list = 0;
bool g_exit_requested = false;  // B on the page: the host goes back on its next update
std::vector<Row> g_rows;
// The reNut tab: the category whose settings the page shows ("" for the
// categories), and the category to select when the categories show again.
std::string g_category;
std::string g_category_return;
// B on a category's page (its own back button): the pause's update goes back.
bool g_back_requested = false;
// A null user focus (sub_82263290) leaves the pause dead but for Start, so
// while the reNut page is up the pause's update gives its list the focus back.

// Left / right on the reNut tab's list: the tab to step to (-1, 1), which
// the pause's update shows.
int g_tab_step = 0;
// The pause's updates so far, and the one A last changed a row in: one A
// comes as the list's press and as the page's aButton's, and counts once.
uint32_t g_pause_updates = 0;
uint32_t g_a_update = ~0u;

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

// Guest calls from a hook, on its own context: scratch space under its stack,
// each call opening its frame below that.
using Guest = renut::guest::Guest;

// A wide (UTF-16, big-endian) copy of an ASCII string in guest memory, made
// once and kept.
uint32_t Wide(uint8_t *base, const std::string &text) {
  static std::unordered_map<std::string, uint32_t> strings;
  if (auto it = strings.find(text); it != strings.end())
    return it->second;
  const uint32_t va = REX_RUNTIME()->GetGuestHeap().SystemHeapAlloc(uint32_t(2 * (text.size() + 1)));
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

// reNut's settings and commands by their old categories, which settings no longer
// carry; each category's entries in its menu order.
constexpr renut::graphics_menu::CategorizedSetting kCategorized[] = {
    {"change_vehicles_anywhere", "Nuts&Bolts/Cheats"},
    {"no_notes_spent", "Nuts&Bolts/Cheats"},
    {"infinite_fuel_and_ammo", "Nuts&Bolts/Cheats"},
    {"infinite_health", "Nuts&Bolts/Cheats"},
    {"infinite_parts", "Nuts&Bolts/Cheats"},
    {"no_out_of_bounds_reset", "Nuts&Bolts/Cheats"},
    {"unlock_all_parts", "Nuts&Bolts/Cheats"},
    {"max_acquired_parts_access", "Nuts&Bolts/Cheats"},
    {"notes_amount", "Nuts&Bolts/Cheats"},
    {"set_notes", "Nuts&Bolts/Cheats"},
    {"unlimited_vehicle_building", "Nuts&Bolts/Cheats"},
    {"walk_up_slopes", "Nuts&Bolts/Cheats"},
    {"debug_main_menu", "Nuts&Bolts/Debug"},
    {"debug_player_position", "Nuts&Bolts/Debug"},
    {"renut_pipeline_capture", "Nuts&Bolts/Debug"},
    {"StopNSwap", "Nuts&Bolts/Events"},
    {"dial_fill_color_power", "Nuts&Bolts/Garage"},
    {"dial_fill_color_ammo", "Nuts&Bolts/Garage"},
    {"dial_fill_color_fuel", "Nuts&Bolts/Garage"},
    {"dial_fill_color_weight", "Nuts&Bolts/Garage"},
    {"dial_fill_color_parts", "Nuts&Bolts/Garage"},
    {"dial_fill_color_strength", "Nuts&Bolts/Garage"},
    {"dial_fill_color_speed", "Nuts&Bolts/Garage"},
    {"renut_resolution", "Nuts&Bolts/Graphics"},
    {"renut_aspect_ratio", "Nuts&Bolts/Graphics"},
    {"renut_field_of_view", "Nuts&Bolts/Graphics"},
    {"renut_time_of_day", "Nuts&Bolts/Graphics"},
    {"renut_shadow_resolution", "Nuts&Bolts/Graphics"},
    {"renut_shadow_softness", "Nuts&Bolts/Graphics"},
    {"renut_anisotropic_filtering", "Nuts&Bolts/Graphics"},
    {"renut_ambient_occlusion", "Nuts&Bolts/Graphics"},
    {"renut_ssao", "Nuts&Bolts/Graphics"},
    {"renut_reflection_resolution", "Nuts&Bolts/Graphics"},
    {"renut_lod_distance", "Nuts&Bolts/Graphics"},
    {"renut_grass_distance", "Nuts&Bolts/Graphics"},
    {"disable_lod", "Nuts&Bolts/Graphics"},
    {"renut_depth_of_field", "Nuts&Bolts/Graphics"},
    {"renut_bloom", "Nuts&Bolts/Graphics"},
    {"renut_post_process_precision", "Nuts&Bolts/Graphics"},
    {"renut_msaa", "Nuts&Bolts/Graphics"},
    {"renut_alpha_to_coverage", "Nuts&Bolts/Graphics"},
    {"renut_anti_aliasing", "Nuts&Bolts/Graphics"},
    {"renut_vsync", "Nuts&Bolts/Graphics"},
    {"renut_gpu_backend", "Nuts&Bolts/Graphics"},
    {"renut_gpu_dred", "Nuts&Bolts/Graphics"},
    {"renut_vulkan_debug_layers", "Nuts&Bolts/Graphics"},
    {"renut_threaded_recording", "Nuts&Bolts/Graphics"},
    {"renut_shader_capture_dir", "Nuts&Bolts/Graphics"},
    {"renut_keyboard", "Nuts&Bolts/Input"},
    {"renut_key_forward", "Nuts&Bolts/Input"},
    {"renut_key_back", "Nuts&Bolts/Input"},
    {"renut_key_left", "Nuts&Bolts/Input"},
    {"renut_key_right", "Nuts&Bolts/Input"},
    {"renut_key_walk", "Nuts&Bolts/Input"},
    {"renut_key_a", "Nuts&Bolts/Input"},
    {"renut_key_b", "Nuts&Bolts/Input"},
    {"renut_key_x", "Nuts&Bolts/Input"},
    {"renut_key_y", "Nuts&Bolts/Input"},
    {"renut_key_lb", "Nuts&Bolts/Input"},
    {"renut_key_rb", "Nuts&Bolts/Input"},
    {"renut_key_lt", "Nuts&Bolts/Input"},
    {"renut_key_rt", "Nuts&Bolts/Input"},
    {"renut_key_start", "Nuts&Bolts/Input"},
    {"renut_key_back_button", "Nuts&Bolts/Input"},
    {"renut_key_ls", "Nuts&Bolts/Input"},
    {"renut_key_rs", "Nuts&Bolts/Input"},
    {"renut_key_dpad_up", "Nuts&Bolts/Input"},
    {"renut_key_dpad_down", "Nuts&Bolts/Input"},
    {"renut_key_dpad_left", "Nuts&Bolts/Input"},
    {"renut_key_dpad_right", "Nuts&Bolts/Input"},
    {"renut_key_guide", "Nuts&Bolts/Input"},
    {"renut_mouse_camera", "Nuts&Bolts/Input"},
    {"renut_mouse_sensitivity", "Nuts&Bolts/Input"},
    {"renut_mouse_invert_y", "Nuts&Bolts/Input"},
    {"renut_disable_extra_controllers", "Nuts&Bolts/Input"},
    {"menu_background_color", "Nuts&Bolts/Menus"},
    {"renut_title_logo", "Nuts&Bolts/Menus"},
    {"renut_mods", "Nuts&Bolts/Mods"},
    {"renut_mods_folder", "Nuts&Bolts/Mods"},
    {"frame_cap", "Nuts&Bolts/Performance"},
    {"renut_physics_whole_steps", "Nuts&Bolts/Performance"},
    {"renut_physics_substeps", "Nuts&Bolts/Performance"},
    {"fps_overlay", "Nuts&Bolts/Performance"},
    {"fps_overlay_size", "Nuts&Bolts/Performance"},
    {"banjo_skin", "Nuts&Bolts/Skins"},
};

// A name in reNut's scope ("renut.<old cvar name>").
std::string FullName(const std::string &name) {
  return std::string(renut::settings::kScope) + "." + name;
}

rex::SettingBase *FindSetting(const std::string &name) {
  auto found = REX_SETTINGS().Find(FullName(name));
  return found ? *found : nullptr;
}

bool IsCommand(const std::string &name) {
  const std::string full = FullName(name);
  for (const rex::ConsoleCommand *command : REX_SETTINGS().EnumerateCommands(renut::settings::kScope))
    if (command->GetName() == full)
      return true;
  return false;
}

std::vector<std::string> Values(const rex::SettingBase &setting) {
  if (setting.GetType() == rex::SettingType::kBool)
    return {"false", "true"};
  return setting.GetConstraints().allowed;
}

bool IsNumber(const rex::SettingBase &setting) {
  using rex::SettingType;
  const SettingType type = setting.GetType();
  return type == SettingType::kI32 || type == SettingType::kI64 || type == SettingType::kU32 ||
         type == SettingType::kU64 || type == SettingType::kF64;
}

// A number with a range and no list of values: stepped through the range.
bool IsRanged(const rex::SettingBase &setting) {
  const rex::SettingConstraints &c = setting.GetConstraints();
  return IsNumber(setting) && c.allowed.empty() && c.min && c.max && *c.max > *c.min;
}

// Whether the pad can change it here: an on/off switch, a list of values or a
// ranged number (the rest is left to the settings window, F4). Commands always can.
bool PadEditable(const rex::SettingBase &setting) {
  if (setting.GetConstraints().life == rex::SettingLife::kStartupOnly)
    return false;
  return setting.GetType() == rex::SettingType::kBool || setting.GetConstraints().allowed.size() >= 2 ||
         IsRanged(setting);
}

// "renut_host_cheats" as "HOST CHEATS".
std::string CvarLabel(const std::string &name) {
  std::string label = name.rfind("renut_", 0) == 0 ? name.substr(6) : name;
  std::replace(label.begin(), label.end(), '_', ' ');
  return Upper(label);
}

std::vector<Row> GraphicsRows() {
  std::vector<Row> rows;
  for (const Option &option : kOptions)
    rows.push_back({Row::Kind::kOption, option.cvar, option.label});
  return rows;
}

// The reNut tab's categories (Cheats first, then by name), or one category's
// settings in kCategorized's order. Only registered ones show.
std::vector<Row> RenutRows(const std::string &category) {
  std::vector<Row> rows;
  for (const renut::graphics_menu::CategorizedSetting &entry : kCategorized) {
    const std::string name = entry.name;
    const std::string entry_category = entry.category;
    if (entry_category.rfind(kCategoryPrefix, 0) != 0 || entry_category == kGraphicsCategory)
      continue;
    const bool command = IsCommand(name);
    if (!command) {
      const rex::SettingBase *setting = FindSetting(name);
      if (!setting || !PadEditable(*setting))
        continue;
    }
    if (!category.empty()) {
      if (entry_category == category)
        rows.push_back({command ? Row::Kind::kCommand : Row::Kind::kOption, name, CvarLabel(name)});
      continue;
    }
    if (std::none_of(rows.begin(), rows.end(), [&](const Row &row) { return row.name == entry_category; }))
      rows.push_back({Row::Kind::kCategory, entry_category, Upper(entry_category.substr(kCategoryPrefix.size()))});
  }
  if (category.empty())
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
      if ((a.name == kFirstCategory) != (b.name == kFirstCategory))
        return a.name == kFirstCategory;
      return a.label < b.label;
    });
  return rows;
}

// "RESOLUTION: 1080P", "CHEATS", "STOPNSWAP".
std::string RowText(const Row &row) {
  if (row.kind != Row::Kind::kOption)
    return Upper(row.label);
  const rex::SettingBase *setting = FindSetting(row.name);
  if (!setting)
    return Upper(row.label);
  std::string value = setting->GetText();
  if (setting->GetType() == rex::SettingType::kBool)
    value = value == "true" || value == "1" ? "On" : "Off";
  std::string text = row.label + ": " + value;
  if (setting->GetConstraints().life == rex::SettingLife::kRestart)
    text += " (restart)";
  return Upper(text);
}

// A ranged number's next value: a twentieth of its range a step (whole
// numbers at least 1), kept in the range.
std::string StepNumber(const rex::SettingBase &setting, const std::string &current, int dir) {
  const double lo = *setting.GetConstraints().min;
  const double hi = *setting.GetConstraints().max;
  const bool whole = setting.GetType() != rex::SettingType::kF64;
  double step = (hi - lo) / 20.0;
  if (whole)
    step = std::max(1.0, std::round(step));
  const double value = std::clamp(std::atof(current.c_str()) + dir * step, lo, hi);
  if (whole)
    return std::to_string(int64_t(std::llround(value)));
  char text[32];
  std::snprintf(text, sizeof(text), "%g", value);
  return text;
}

// Moves the option to its next (dir 1) or previous (dir -1) value, applies it
// and saves the config.
bool StepOption(const std::string &cvar, int dir) {
  const rex::SettingBase *setting = FindSetting(cvar);
  if (!setting)
    return false;
  const std::string current_text = setting->GetText();
  std::string next;
  if (IsRanged(*setting)) {
    next = StepNumber(*setting, current_text, dir);
    if (next == current_text)
      return false;
  } else {
    const std::vector<std::string> values = Values(*setting);
    if (values.size() < 2)
      return false;
    std::string current = current_text;
    if (setting->GetType() == rex::SettingType::kBool)
      current = current == "true" || current == "1" ? "true" : "false";
    const int count = int(values.size());
    const auto it = std::find(values.begin(), values.end(), current);
    const int index = it == values.end() ? 0 : int(it - values.begin());
    next = values[size_t(((index + dir) % count + count) % count)];
  }
  if (!REX_SETTINGS().SetText(FullName(cvar), next))
    return false;
  if (!g_config_path.empty())
    (void)REX_SETTINGS().Save(g_config_path);
  return true;
}

// A command row: A (or right) runs it.
bool RunCommand(const std::string &cvar) {
  const rex::Result<void> ran = REX_SETTINGS().Run(FullName(cvar), "");
  return ran || ran.error().category != rex::ErrorCategory::kNotFound;
}

// Column 0 of a list row record, as sub_823B2798 writes it.
void WriteRowText(uint8_t *base, uint32_t row, const std::string &text) {
  const size_t length = std::min<size_t>(text.size(), 127);
  for (size_t i = 0; i < length; ++i)
    Store16(base, uint32_t(row + 8 + 2 * i), uint8_t(text[i]));
  Store16(base, uint32_t(row + 8 + 2 * length), 0);
  Store32(base, row + 268, 0);
  Store32(base, row + 272, uint32_t(length));
}

void SetStringProperty(Guest &g, uint8_t *base, uint32_t obj, const char *name, const char *value) {
  auto &f = g.ctx();
  const uint32_t id = g.Alloc(4);
  const uint32_t val = g.Alloc(20);
  if (Failed(XuiObjectGetPropertyId(f, base, obj, Wide(base, name), id)))
    return;
  Store32(base, val, 5);  // XUI_EPT_STRING
  Store32(base, val + 4, Wide(base, value));
  XuiObjectSetProperty(f, base, obj, Load32(base, id), 0, val);
}

void SetUnsignedProperty(Guest &g, uint8_t *base, uint32_t obj, const char *name, uint32_t value) {
  auto &f = g.ctx();
  const uint32_t id = g.Alloc(4);
  const uint32_t val = g.Alloc(20);
  if (Failed(XuiObjectGetPropertyId(f, base, obj, Wide(base, name), id)))
    return;
  Store32(base, val, 3);  // XUI_EPT_UNSIGNED
  Store32(base, val + 4, value);
  XuiObjectSetProperty(f, base, obj, Load32(base, id), 0, val);
}

float FloatProperty(Guest &g, uint8_t *base, uint32_t obj, const char *name) {
  auto &f = g.ctx();
  const uint32_t id = g.Alloc(4);
  const uint32_t val = g.Alloc(20);
  if (Failed(XuiObjectGetPropertyId(f, base, obj, Wide(base, name), id)) ||
      Failed(XuiObjectGetProperty(f, base, obj, Load32(base, id), 0, val)) ||
      Load32(base, val) != 4)  // XUI_EPT_FLOAT
    return 0.0f;
  return LoadFloat(base, val + 4);
}

uint32_t UserFocus(Guest &g, uint8_t *base, uint32_t page) {
  return XuiElementGetUserFocus(g.ctx(), base, base[page + kUser]);
}


// The Display Settings tab's TV as a named XUI texture (as sub_8254B270 does
// for the sliders' icons), made once its bundle is loaded.
uint32_t GraphicsIcon(Guest &g, uint8_t *base) {
  static bool made = false;
  static uint32_t name = 0;
  if (made)
    return name;
  auto &f = g.ctx();
  const uint32_t aid = g.Alloc(16);
  const uint32_t texture = g.Alloc(4);
  // No asset named pausemenudisplaysettings: no icon.
  if (!(GameAssetRefGetAid(f, base, kTvIconAsset, aid, 0) & 0xFF)) {
    made = true;
    return 0;
  }
  // Not loaded yet: asked again later.
  const uint32_t query = g.Alloc(8);
  Store32(base, query, kAssetQueryVtable);
  Store32(base, query + 4, Load32(base, aid));
  if (!(AssetQuery(f, base, query, kAnyProvider) & 0xFF))
    return 0;
  made = true;
  const uint32_t asset = AssetManOpenParams(f, base, aid, 0);
  const uint32_t icon = Wide(base, "tex://renutGraphicsIcon");
  if (!asset || Failed(AddAssetTexture(f, base, asset, icon, texture, 0)))
    return 0;
  name = icon;
  return name;
}

// Whether Display Settings shows its big font row (its init's args say).
bool BigFontShown(const uint8_t *base, uint32_t page) {
  return !Load32(base, page + kDisplayNoBigFont) && Load32(base, page + kDisplayBigFont);
}

// Down from the row above GRAPHICS (the big font row when it shows, else
// contrast) to GRAPHICS.
void LinkLastRow(Guest &g, uint8_t *base, uint32_t page) {
  auto &f = g.ctx();
  if (BigFontShown(base, page))
    SetNavUpDown(f, base, Load32(base, page + kDisplayBigFont), Wide(base, "displayContrastSlider"),
                 Wide(base, kRowId));
  else
    SetNavUpDown(f, base, Load32(base, page + kDisplayContrast), Wide(base, "displayBrightnessSlider"),
                 Wide(base, kRowId));
}

// GRAPHICS, under the last row: a button with Game Information's menu item
// visual (its rows' look: name only), the rows spaced to fit the box.
void AddGraphicsRow(PPCContext &ctx, uint8_t *base, uint32_t page) {
  g_display_page = page;
  g_row = 0;
  Guest g(ctx, base);
  auto &f = g.ctx();
  const uint32_t contrast = Load32(base, page + kDisplayContrast);
  const uint32_t brightness = Load32(base, page + kDisplayBrightness);
  const bool big_font = BigFontShown(base, page);
  const uint32_t big_font_row = big_font ? Load32(base, page + kDisplayBigFont) : 0;
  const uint32_t out = g.Alloc(4);
  if (!contrast || !brightness || Failed(XuiElementGetParent(f, base, contrast, out)))
    return;
  const uint32_t parent = Load32(base, out);
  Store32(base, out, 0);
  XuiCreateObject(f, base, Wide(base, "XuiButton"), out);
  const uint32_t row = Load32(base, out);
  if (!row)
    return;
  XuiElementAddChild(f, base, parent, row);
  SetStringProperty(g, base, row, "Id", kRowId);
  SetStringProperty(g, base, row, "Visual", kRowVisual);

  // Respaces the game's rows and GRAPHICS to share the span the game's took,
  // starting half a row higher; GRAPHICS is as big as the sliders' rows.
  const float width = FloatProperty(g, base, contrast, "Width");
  const float height = FloatProperty(g, base, contrast, "Height");
  std::vector<uint32_t> rows = {brightness, contrast};
  if (big_font_row)
    rows.push_back(big_font_row);
  const float n = float(rows.size());
  const uint32_t first = g.Alloc(12);
  const uint32_t position = g.Alloc(12);
  XuiElementGetPosition(f, base, brightness, first);
  XuiElementGetPosition(f, base, rows.back(), position);
  const float top = LoadFloat(base, first + 4);
  const float gap = (LoadFloat(base, position + 4) - top) / (n - 1.0f);
  const float new_top = height > 0.0f ? top - height * 0.5f : top;
  const float spacing = height > 0.0f ? ((n - 1.0f) * gap + height) / n : gap * (n - 1.0f) / n;
  for (size_t i = 0; i < rows.size(); ++i) {
    XuiElementGetPosition(f, base, rows[i], position);
    const float y = new_top + float(i) * spacing;
    const float moved = y - LoadFloat(base, position + 4);
    StoreFloat(base, position + 4, y);
    XuiElementSetPosition(f, base, rows[i], position);
    if (rows[i] != big_font_row)
      continue;
    // The big font row's tick and tick box go with it (unless they are its
    // children already).
    for (const uint32_t offset : {kDisplayBigFontTick, kDisplayBigFontTickBox}) {
      const uint32_t part = Load32(base, page + offset);
      Store32(base, out, 0);
      if (!part || Failed(XuiElementGetParent(f, base, part, out)) || Load32(base, out) == big_font_row)
        continue;
      XuiElementGetPosition(f, base, part, first);
      StoreFloat(base, first + 4, LoadFloat(base, first + 4) + moved);
      XuiElementSetPosition(f, base, part, first);
    }
  }
  XuiElementGetPosition(f, base, contrast, position);
  StoreFloat(base, position + 4, new_top + n * spacing);
  XuiElementSetPosition(f, base, row, position);
  if (width > 0.0f && height > 0.0f) {
    XuiElementSetBounds(f, base, row, double(width), double(height));
  }

  // XuiControlSetText's message.
  const uint32_t message = g.Alloc(0x20);
  const uint32_t data = g.Alloc(0x30);
  MakeLabelMessage(f, base, message, data, Wide(base, kTitle));
  XuiSendMessage(f, base, row, message);
  // A button has no icon column, so the TV is an image of its own on the
  // button, where the sliders' icons are.
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
    XuiElementSetBounds(f, base, image, double(side), double(side));
    SetUnsignedProperty(g, base, image, "SizeMode", kSizeModeFitCentred);
    XuiImageElementSetImagePath(f, base, image, icon);
  }

  SetNavUpDown(f, base, row, Wide(base, big_font_row ? "useBigFontButton" : "displayContrastSlider"), kWideEmpty);
  LinkLastRow(g, base, page);
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

// Remakes the menu list's rows (as sub_82563058 does), one per option, with
// 'selected' selected and the list focused again.
void FillList(Guest &g, uint8_t *base, uint32_t list, uint32_t selected, uint32_t user) {
  auto &f = g.ctx();
  const uint32_t handle = Load32(base, list + kListHandle);
  const uint32_t rows = Load32(base, list + kListRows);
  const uint32_t row_size = Load16(base, rows);
  const int32_t count = row_size ? int32_t(Load32(base, rows + 8) - Load32(base, rows + 4)) / int32_t(row_size) : 0;
  if (count > 0)
    XuiListDeleteItems(f, base, handle, 0, uint32_t(count));
  const uint32_t rows_count = uint32_t(g_rows.size());
  Store32(base, list + kListCount, rows_count);
  ListInsertItems(f, base, list, rows_count);
  selected = std::min(selected, rows_count ? rows_count - 1 : 0);
  // Setting the selection doesn't scroll, and an off-screen row has no item to
  // focus, so the list is scrolled to the selected row first.
  if (selected)
    XuiListSetTopItem(f, base, handle, selected);
  XuiContainerSetCurSel(f, base, handle, selected);
  XuiElementSetFocus(f, base, handle, user);
}

// The page's prompts: A select, B 'back_key' (back, or the reNut tab's
// categories: resume game, as the pause's tabs say).
void SetInfoPrompts(Guest &g, uint8_t *base, uint32_t page, uint32_t back_key = kKeyBack) {
  auto &f = g.ctx();
  const uint32_t prompts = Load32(base, page + kInfoPrompts);
  if (prompts == kNone)
    return;
  PromptA(f, base, prompts, kKeySelect);
  PromptB(f, base, prompts, back_key);
  PromptX(f, base, prompts, kKeyNone);
  PromptY(f, base, prompts, kKeyNone);
}

// Game Information as the graphics menu or the reNut tab.
void SetUpPage(PPCContext &ctx, uint8_t *base, uint32_t page) {
  Guest g(ctx, base);
  auto &f = g.ctx();
  const uint32_t out = g.Alloc(4);
  XuiObjectFromHandle(f, base, Load32(base, page + kInfoMenuList), out);
  const uint32_t list = Load32(base, out);
  if (!list)
    return;
  g_info_page = page;
  g_info_list = list;
  g_exit_requested = false;
  g_back_requested = false;
  g_tab_step = 0;
  const bool renut = g_menu == Menu::kRenut;
  const bool categories = renut && g_category.empty();
  g_rows = renut ? RenutRows(g_category) : GraphicsRows();
  // The categories select the one last shown (back from it), a category's
  // page its first setting.
  uint32_t selected = 0;
  for (uint32_t i = 0; categories && i < g_rows.size(); ++i)
    if (g_rows[i].name == g_category_return)
      selected = i;
  g_category_return.clear();
  FillList(g, base, list, selected, base[page + kUser]);
  if (categories) {
    // The categories keep the tabs; A is the pause's (it opens a category), so
    // the page's aButton is off. Left / right change tab (sub_823B33E8 hook).
    ElementEnable(f, base, Load32(base, page + kInfoA), 0);
  } else {
    // Tabs off. GRAPHICS enables the hidden left / right buttons; a category's
    // page disables its own back button so B is the pause's.
    ElementEnable(f, base, Load32(base, page + kInfoTabs), 0);
    if (!renut) {
      ElementEnable(f, base, Load32(base, page + kInfoLeft), 1);
      ElementEnable(f, base, Load32(base, page + kInfoRight), 1);
    } else {
      ElementEnable(f, base, Load32(base, page + kInfoBack), 0);
    }
  }
  std::string title = kTitle;
  if (renut)
    title = categories ? kRenutTitle : Upper(g_category.substr(kCategoryPrefix.size()));
  XuiTextElementSetText(f, base, Load32(base, page + kInfoSubtitle), Wide(base, title));
  SetInfoPrompts(g, base, page, categories ? kPauseBackKey : kKeyBack);
}

// The reNut tab's page went (another tab, or the pause closing).
void ForgetRenutPage() {
  if (g_menu != Menu::kRenut)
    return;
  g_info_page = 0;
  g_info_list = 0;
  g_category.clear();
  g_category_return.clear();
  g_back_requested = false;
  g_tab_step = 0;
}

// The reNut tab shows a category's page, or with "" the categories again with
// the last one selected; called from the pause's own press or update.
void ShowRenutPage(PPCContext &ctx, uint8_t *base, const std::string &category) {
  if (!g_info_page || g_menu != Menu::kRenut || g_host != Host::kPause)
    return;
  const uint32_t page = g_info_page;
  g_category_return = category.empty() ? g_category : std::string();
  g_category = category;
  g_info_page = 0;
  g_info_list = 0;
  g_open_pending = true;
  Guest g(ctx, base);
  auto &f = g.ctx();
  // The tabs come back with the categories (a category's page had them off).
  if (category.empty())
    ElementEnable(f, base, Load32(base, page + kInfoTabs), 1);
  PlayUiSound(f, base, kSounds, category.empty() ? 1 : 0);
  PauseShowPage(f, base, g_host_object, kPausePageInfo);  // the page opens now
  g_open_pending = false;
  // Its builder sets the pause's prompts after the page's init.
  if (g_info_page)
    SetInfoPrompts(g, base, g_info_page, category.empty() ? kPauseBackKey : kKeyBack);
}

// From the reNut tab, shows the tab 'step' (-1, 1) along the tab bar, as the
// pause's left / right do (sub_8258C948). From the pause's update.
void StepRenutTab(PPCContext &ctx, uint8_t *base, uint32_t pause, int step) {
  Guest g(ctx, base);
  auto &f = g.ctx();
  const uint32_t bar = Load32(base, pause + kPauseToolBar);
  const int32_t count = int32_t(Load32(base, kPauseTabCount));
  const int32_t from = int32_t(XuiContainerGetCurSel(f, base, bar));
  const int32_t to = from + step;
  if (!bar || to < 0 || to >= count || count > int32_t(kPauseTabSlots))
    return;
  const uint32_t type = Load32(base, kPauseTabs + 4 * uint32_t(to));
  XuiContainerSetCurSel(f, base, bar, uint32_t(to));
  ForgetRenutPage();
  PauseShowPage(f, base, pause, type);
}

void OpenGraphics(PPCContext &ctx, uint8_t *base, Host host, uint32_t host_object) {
  g_open_pending = true;
  g_menu = Menu::kGraphics;
  g_host = host;
  g_host_object = host_object;
  g_row = 0;
  g_display_page = 0;
  Guest g(ctx, base);
  auto &f = g.ctx();
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
  auto &f = g.ctx();
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
  auto &f = g.ctx();
  const uint32_t selected = XuiContainerGetCurSel(f, base, Load32(base, page + kInfoMenuList));
  if (selected >= g_rows.size())
    return;
  const Row &target = g_rows[selected];
  if (target.kind == Row::Kind::kCategory)
    return;  // the pause's A opens it (the sub_8258C948 hook)
  if (target.kind == Row::Kind::kCommand) {
    if (dir > 0 && RunCommand(target.name))
      PlayUiSound(f, base, kSounds, 0);
    return;
  }
  if (!StepOption(target.name, dir))
    return;
  PlayUiSound(f, base, kSounds, 2);
  // Updates only this row in place (remaking the rows loses the scrolling): its
  // record and, when on screen, its control.
  const uint32_t handle = Load32(base, g_info_list + kListHandle);
  const uint32_t rows = Load32(base, g_info_list + kListRows);
  const uint32_t row = Load32(base, rows + 4) + Load16(base, rows) * selected;
  WriteRowText(base, row, RowText(target));
  if (const uint32_t control = XuiContainerGetItemControl(f, base, handle, selected))
    XuiControlSetText(f, base, control, row + 8);
}

}  // namespace

namespace renut::graphics_menu {

void SetConfigPath(const std::filesystem::path &path) { g_config_path = path; }

std::span<const CategorizedSetting> CategorizedSettings() { return kCategorized; }

}  // namespace renut::graphics_menu

// Display Settings' init, then GRAPHICS under its last row (the big font row,
// LARGE DIALOGUE TEXT, when the game shows it).
REX_HOOK_RAW(sub_82560958) {
  const uint32_t page = ctx.r3.u32;
  __imp__sub_82560958(ctx, base);
  AddGraphicsRow(ctx, base, page);
}

// Leaving a row's editing puts back its navigation; this relinks the last
// row's down to GRAPHICS.
REX_HOOK_RAW(sub_82560C58) {
  const uint32_t page = ctx.r3.u32;
  const uint32_t editing = ctx.r4.u32;
  __imp__sub_82560C58(ctx, base);
  if (editing || !g_row || page != g_display_page)
    return;
  Guest g(ctx, base);
  LinkLastRow(g, base, page);
}

REX_HOOK_RAW(sub_82560DF8) {
  const uint32_t page = ctx.r3.u32;
  __imp__sub_82560DF8(ctx, base);
  if (!g_row || page != g_display_page || Load32(base, page + kDisplayEditing))
    return;
  const uint32_t prompts = Load32(base, page + kDisplayPrompts);
  if (prompts == kNone)
    return;
  Guest g(ctx, base);
  if (UserFocus(g, base, page) == g_row)
    PromptA(g.ctx(), base, prompts, kKeySelect);
}

REX_HOOK_RAW(sub_8258DA40) {
  const uint32_t pause = ctx.r3.u32;
  const uint32_t pressed = ctx.r4.u32;
  if ((pressed == Load32(base, pause + kPauseA) || (g_row && pressed == g_row)) &&
      !Load32(base, pause + kPauseEditing) &&
      RowFocused(ctx, base, Load32(base, pause + kPauseDisplayPage))) {
    OpenGraphics(ctx, base, Host::kPause, pause);
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_8258DA40(ctx, base);
}

REX_HOOK_RAW(sub_825A0F98) {
  const uint32_t menu = ctx.r3.u32;
  const uint32_t pressed = ctx.r4.u32;
  if ((pressed == Load32(base, menu + kMainA) || (g_row && pressed == g_row)) &&
      !Load32(base, menu + kMainEditing) &&
      RowFocused(ctx, base, Load32(base, menu + kMainDisplayPage))) {
    OpenGraphics(ctx, base, Host::kMain, menu);
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_825A0F98(ctx, base);
}

REX_HOOK_RAW(sub_825613A8) {
  const uint32_t page = ctx.r3.u32;
  const uint32_t args = ctx.r4.u32;
  __imp__sub_825613A8(ctx, base);
  g_info_page = 0;
  g_info_list = 0;
  // The page GRAPHICS or the reNut tab asked for: its host gives it that
  // host's B prompt.
  const uint32_t back_key = g_host == Host::kPause ? kPauseBackKey : kMainBackKey;
  if (!g_open_pending || Load32(base, args + 4) != back_key)
    return;
  g_open_pending = false;
  SetUpPage(ctx, base, page);
}

REX_HOOK_RAW(sub_82561960) {
  const uint32_t page = ctx.r3.u32;
  if (!page || page != g_info_page) {
    __imp__sub_82561960(ctx, base);
    return;
  }
  const uint32_t pressed = ctx.r4.u32;
  // Handled, except on the reNut tab where presses not taken here (and all on
  // the categories) are left to the pause.
  const bool categories = g_menu == Menu::kRenut && g_category.empty();
  ctx.r3.u64 = 1;
  if (g_menu == Menu::kRenut &&
      (categories || (pressed != Load32(base, page + kInfoBack) && pressed != Load32(base, page + kInfoMenuList) &&
                      pressed != Load32(base, page + kInfoA)))) {
    ctx.r3.u64 = 0;
    return;
  }
  if (pressed == Load32(base, page + kInfoBack)) {
    if (g_menu == Menu::kGraphics)
      g_exit_requested = true;
    else
      g_back_requested = true;  // the pause's update goes back
  } else if (pressed == Load32(base, page + kInfoMenuList) || pressed == Load32(base, page + kInfoA)) {
    if (g_menu != Menu::kRenut || g_a_update != g_pause_updates)
      ChangeOption(ctx, base, page, 1);
    g_a_update = g_pause_updates;
  } else if (g_menu == Menu::kGraphics) {
    if (pressed == Load32(base, page + kInfoLeft))
      ChangeOption(ctx, base, page, -1);
    else if (pressed == Load32(base, page + kInfoRight))
      ChangeOption(ctx, base, page, 1);
  }
}

REX_HOOK_RAW(sub_82562CE8) {
  if (!g_info_list || ctx.r3.u32 != g_info_list) {
    __imp__sub_82562CE8(ctx, base);
    return;
  }
  const uint32_t index = ctx.r4.u32;
  if (index < g_rows.size())
    WriteRowText(base, ctx.r5.u32, RowText(g_rows[index]));
  ctx.r3.u64 = 1;
}

// The pause's presses: B leaves GRAPHICS; on the reNut tab A on a category
// shows its page and B there goes back to the categories.
REX_HOOK_RAW(sub_8258C948) {
  const uint32_t pause = ctx.r3.u32;
  const uint32_t pressed = ctx.r4.u32;
  if (g_info_page && g_host == Host::kPause && g_host_object == pause) {
    if (g_menu == Menu::kGraphics && pressed == Load32(base, pause + kPauseBack)) {
      ExitGraphics(ctx, base);
      ctx.r3.u64 = 1;
      return;
    }
    if (g_menu == Menu::kRenut && !g_category.empty() && pressed == Load32(base, pause + kPauseBack)) {
      ShowRenutPage(ctx, base, "");
      ctx.r3.u64 = 1;
      return;
    }
    if (g_menu == Menu::kRenut && g_category.empty() &&
        (pressed == Load32(base, pause + kPauseA) || pressed == Load32(base, g_info_page + kInfoMenuList))) {
      Guest g(ctx, base);
      const uint32_t selected =
          XuiContainerGetCurSel(g.ctx(), base, Load32(base, g_info_page + kInfoMenuList));
      if (selected < g_rows.size() && g_rows[selected].kind == Row::Kind::kCategory) {
        const std::string category = g_rows[selected].name;
        ShowRenutPage(ctx, base, category);
      }
      ctx.r3.u64 = 1;
      return;
    }
  }
  __imp__sub_8258C948(ctx, base);
}

// The pause's update: carries out what the page's presses and navigation asked
// for once the button is released, and refocuses the reNut page's list.
REX_HOOK_RAW(sub_8258C7C0) {
  ++g_pause_updates;
  if (g_info_page && g_host == Host::kPause && g_host_object == ctx.r3.u32) {
    if (g_menu == Menu::kGraphics) {
      if (g_exit_requested)
        ExitGraphics(ctx, base);
    } else if (Guest g(ctx, base); [&] {
                 const uint32_t focus = XuiElementGetUserFocus(g.ctx(), base, base[g_info_page + kUser]);
                 return !focus || focus == Load32(base, g_info_page + kInfoMenuList);
               }()) {
      // No focus, or the list itself (its selected row off screen): the row
      // scrolled to and selected again, its item taking the focus.
      const uint32_t list = Load32(base, g_info_page + kInfoMenuList);
      const uint32_t row = XuiContainerGetCurSel(g.ctx(), base, list);
      if (int32_t(row) > 0) {
        XuiListSetTopItem(g.ctx(), base, list, row);
        XuiContainerSetCurSel(g.ctx(), base, list, row);
      }
      XuiElementSetFocus(g.ctx(), base, list, base[g_info_page + kUser]);
    } else if (g_back_requested && !Load32(base, kButtonPressed)) {
      g_back_requested = false;
      ShowRenutPage(ctx, base, "");
    } else if (g_tab_step && !Load32(base, kButtonPressed)) {
      const int step = g_tab_step;
      g_tab_step = 0;
      StepRenutTab(ctx, base, ctx.r3.u32, step);
    }
  }
  __imp__sub_8258C7C0(ctx, base);
}

// The pause's constructor: the reNut tab listed after the game's.
REX_HOOK_RAW(sub_8258B948) {
  __imp__sub_8258B948(ctx, base);
  ForgetRenutPage();
  const uint32_t count = Load32(base, kPauseTabCount);
  if (count >= kPauseTabSlots)
    return;
  for (uint32_t i = 0; i < count; ++i)
    if (Load32(base, kPauseTabs + 4 * i) == kPauseTabRenut)
      return;
  Store32(base, kPauseTabs + 4 * count, kPauseTabRenut);
  Store32(base, kPauseTabCount, count + 1);
}

// The pause shows a tab's page: the reNut tab shows Game Information, made
// into its menu by the page's init. Any other tab destroys its page.
REX_HOOK_RAW(sub_8258D038) {
  const uint32_t pause = ctx.r3.u32;
  // A busy pause shows nothing (the page stays).
  if (Load32(base, pause + kPauseBusy)) {
    __imp__sub_8258D038(ctx, base);
    return;
  }
  ForgetRenutPage();
  if (ctx.r4.u32 != kPauseTabRenut) {
    __imp__sub_8258D038(ctx, base);
    return;
  }
  g_open_pending = true;
  g_menu = Menu::kRenut;
  g_host = Host::kPause;
  g_host_object = pause;
  ctx.r4.u64 = kPausePageInfo;
  __imp__sub_8258D038(ctx, base);
  g_open_pending = false;
  // Its builder sets the pause's prompts after the page's init.
  if (g_info_page) {
    Guest g(ctx, base);
    SetInfoPrompts(g, base, g_info_page, kPauseBackKey);
  }
}

REX_HOOK_RAW(sub_825A0458) {
  const uint32_t menu = ctx.r3.u32;
  if (g_info_page && g_menu == Menu::kGraphics && g_host == Host::kMain && g_host_object == menu &&
      Load32(base, menu + kMainState) == kMainStateInfo && ctx.r4.u32 == Load32(base, menu + kMainBack)) {
    ExitGraphics(ctx, base);
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_825A0458(ctx, base);
}

// A page's navigation: down from brightness goes to (disabled) contrast, not
// GRAPHICS; on the reNut tab left / right queue a tab change instead.
REX_HOOK_RAW(sub_823B33E8) {
  const uint32_t page = ctx.r3.u32;
  const uint32_t navigate = ctx.r4.u32;
  const uint32_t handled = ctx.r5.u32;
  if (page && page == g_info_page && g_menu == Menu::kRenut && g_category.empty() && navigate && handled) {
    const uint32_t direction = Load32(base, navigate);
    if (direction == kNavigateLeft || direction == kNavigateRight) {
      g_tab_step = direction == kNavigateLeft ? -1 : 1;
      Store32(base, navigate + 8, 0);
      Store32(base, navigate + 12, 1);  // stay
      Store32(base, handled, 1);
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_823B33E8(ctx, base);
  if (!g_row || page != g_display_page || Load32(base, navigate + 8) != g_row ||
      Load32(base, navigate + 4) != Load32(base, page + kDisplayBrightness))
    return;
  const uint32_t contrast = Load32(base, page + kDisplayContrast);
  Store32(base, navigate + 8, contrast);
  if (Load32(base, page + kPageLastFocus) == g_row)
    Store32(base, page + kPageLastFocus, contrast);
}

// Display Settings is called Display/Graphics, the map tab RENUT SETTINGS.
REX_HOOK_RAW(sub_825D7F28) {
  const uint32_t key = ctx.r4.u32;
  const uint32_t out = ctx.r5.u32;
  const int32_t out_length = int32_t(ctx.r6.u32);
  __imp__sub_825D7F28(ctx, base);
  if (!key || !out || out_length <= 0)
    return;
  const char *asked = reinterpret_cast<const char *>(base + key);
  const auto text = std::find_if(std::begin(kTexts), std::end(kTexts), [&](const Text &t) {
    return std::strncmp(asked, t.key, std::strlen(t.key) + 1) == 0;
  });
  if (text == std::end(kTexts))
    return;
  const size_t length = std::min<size_t>(std::strlen(text->text), size_t(out_length - 1));
  for (size_t i = 0; i < length; ++i)
    Store16(base, uint32_t(out + 2 * i), uint8_t(text->text[i]));
  Store16(base, uint32_t(out + 2 * length), 0);
  ctx.r3.u64 = 1;  // found
}

REX_HOOK_RAW(sub_8259FDA8) {
  const uint32_t menu = ctx.r3.u32;
  if (g_menu == Menu::kGraphics && g_host == Host::kMain && g_host_object == menu) {
    if (g_exit_requested && g_info_page)
      ExitGraphics(ctx, base);
    else if (g_open_pending && Load32(base, menu + kMainState) != kMainStateInfo)
      g_open_pending = false;  // the menu went elsewhere before the page opened
  }
  __imp__sub_8259FDA8(ctx, base);
}
