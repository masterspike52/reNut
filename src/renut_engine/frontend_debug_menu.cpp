#include <rex/cvar.h>
#include <rex/hook.h>

#include <cstdint>
#include <string>

// Name = "Debug Main Menu"
REXCVAR_DEFINE_STRING(debug_main_menu, "main", "Nuts&Bolts/Debug", "Opens the unused blueprint-style debug main menu in place of the retail one.")
.allowed({ "main", "debug" });

// Scene ids -- index both off_82E5E038 and the descriptor array at 0x82E56A90.
static constexpr uint32_t kSceneMainMenu = 68;   // XuiScene_BanjoX_Frontend_MainMenu
static constexpr uint32_t kSceneStartMenu = 70;  // XuiScene_BanjoX_Frontend_StartMenu

REX_EXTERN(__imp__sub_823E9BA8);  // original scene open

REX_HOOK_RAW(sub_823E9BA8) {
	if (ctx.r4.u32 == kSceneStartMenu && REXCVAR_GET(debug_main_menu) == "debug") {
		// Full 64-bit write, same as the `li r4, N` this replaces.
		ctx.r4.u64 = kSceneMainMenu;
	}

	__imp__sub_823E9BA8(ctx, base);
}
