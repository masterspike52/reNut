#include <rex/ppc/hook.h>
#include <rex/setting.h>

#include <cstdint>
#include <string>

#include "renut_engine/app/renut_settings.h"

namespace {

struct DebugMenuSettings final : rex::SettingOwner {
	DebugMenuSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
	REX_SETTING(std::string, debug_main_menu, "main", "Opens the unused blueprint-style debug main menu in place of the retail one.",
		rex::Allowed("main", "debug"));
};
DebugMenuSettings &g_debug_menu = *new DebugMenuSettings();  // never freed, registered by RenutApp::OnConfigure

}  // namespace

// TU3's scene table (0x82D90EC8): frontend_mainmenu and frontend_startmenu
// (the base's 68 and 70; the title update added scenes before them).
static constexpr uint32_t kSceneMainMenu = 76;
static constexpr uint32_t kSceneStartMenu = 78;

REX_EXTERN(__imp__sub_823B7B50);  // original scene open

REX_HOOK_RAW(sub_823B7B50) {
	if (ctx.r4.u32 == kSceneStartMenu && g_debug_menu.debug_main_menu.Get() == "debug") {
		ctx.r4.u64 = kSceneMainMenu;
	}

	__imp__sub_823B7B50(ctx, base);
}
