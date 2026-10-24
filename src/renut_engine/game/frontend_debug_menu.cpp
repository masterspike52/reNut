#include <rex/cvar.h>
#include <rex/hook.h>

#include <cstdint>
#include <string>


REXCVAR_DEFINE_STRING(debug_main_menu, "main", "Nuts&Bolts/Debug", "Opens the unused blueprint-style debug main menu in place of the retail one.")
.allowed({ "main", "debug" });

// TU3's scene table (0x82D90EC8): frontend_mainmenu and frontend_startmenu
// (the base's 68 and 70; the title update added scenes before them).
static constexpr uint32_t kSceneMainMenu = 76;
static constexpr uint32_t kSceneStartMenu = 78;

REX_EXTERN(__imp__sub_823B7B50);  // original scene open

REX_HOOK_RAW(sub_823B7B50) {
	if (ctx.r4.u32 == kSceneStartMenu && REXCVAR_GET(debug_main_menu) == "debug") {
		ctx.r4.u64 = kSceneMainMenu;
	}

	__imp__sub_823B7B50(ctx, base);
}
