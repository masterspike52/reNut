#include <rex/cvar.h>
#include <rex/hook.h>

#include <cstdint>

// Name = "Disable Townsfolk"
REXCVAR_DEFINE_BOOL(disable_townsfolk, false, "Nuts&Bolts/Showdown Town",
	"Stops the wandering Showdown Town townsfolk from spawning. Named characters "
	"(Mumbo, Humba Wumba, ...) are unaffected. Applies as you move around town.");

REX_EXTERN(__imp__sub_82273C68);  // spawn-point blocked test

// Return address of the blocked-test call at 0x821EDBE4, inside sub_821EDB60.
static constexpr uint32_t kCrowdSpawnBlockedTestLr = 0x821EDBE8;

REX_HOOK_RAW(sub_82273C68) {
	if (REXCVAR_GET(disable_townsfolk) && ctx.lr == kCrowdSpawnBlockedTestLr) {
		ctx.r3.u64 = 1;  // "blocked" -> the spawner skips this point
		return;
	}

	__imp__sub_82273C68(ctx, base);
}
