#include <rex/cvar.h>
#include <rex/hook.h>

#include <cstdint>
#include <string>


REXCVAR_DEFINE_STRING(debug_main_menu, "main", "Nuts&Bolts/Debug", "Opens the unused blueprint-style debug main menu in place of the retail one.")
.allowed({ "main", "debug" });

static constexpr uint32_t kSceneMainMenu = 68;   
static constexpr uint32_t kSceneStartMenu = 70;  

REX_EXTERN(__imp__sub_823E9BA8);  // original scene open

REX_HOOK_RAW(sub_823E9BA8) {
	if (ctx.r4.u32 == kSceneStartMenu && REXCVAR_GET(debug_main_menu) == "debug") {
		ctx.r4.u64 = kSceneMainMenu;
	}

	__imp__sub_823E9BA8(ctx, base);
}
