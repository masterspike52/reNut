#include <rex/hook.h>
#include "rex_macros.h"
#include <rex/cvar.h>
#include "globals.h"

REXCVAR_DEFINE_INT32(target_refreshRate, -1, "Nuts&Bolts/Performance", "The index for a given VSync setting. Options are: \n\t-1 = Unaffected\n\t0 = Immediate (Uncapped)\n\t1 = 60Hz\n\t2 = 30Hz (No Threshold)\n\t3 = 20Hz\n\t4 - 9 = Variants of 30Hz.").range(-1, 9);

// Declare the original generated implementation
REX_PPC_EXTERN_IMPORT(setParamInterval); // sub_823edeb8

static void setParamInterval(int idx) {
	REX_PPC_INVOKE(setParamInterval, idx);
}

static std::once_flag g_refreshRateCallbackSet_once;
bool isInKlungoGame = false;

// TODO: Implement a callback so that we can set the vsync value and have it auto change.
void refreshRate_hook(PPCRegister& r3) // Called at 823edeb8 (Start of the function)
{
	bool overrideTarget = false;
	
	// If the value is -1, don't overwrite the R3 register.
	if(REXCVAR_GET(target_refreshRate) == -1){
		overrideTarget = true;
	}
	
	if(isInKlungoGame && !overrideTarget){
		r3.u32 = 7;
		overrideTarget = true;
	}
	
	if(r3.u32 == 0 && !overrideTarget) {
		overrideTarget = true;
	}
	
	if(!overrideTarget) r3.u32 = REXCVAR_GET(target_refreshRate); // Back to normal
	
	currentSyncVal = r3.u32;
}

void klungoConstruct_hook() // Called at 8246e7a8 (End of the function)
{
	isInKlungoGame = true;
	setParamInterval(2);
}

void klungoDestruct_hook() // Called at 8246f62c (End of the function)
{
	isInKlungoGame = false;
	setParamInterval(REXCVAR_GET(target_refreshRate));
}