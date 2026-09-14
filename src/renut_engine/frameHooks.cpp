#include <windows.h>

#include <rex/hook.h>
#include <rex/cvar.h>
#include "globals.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

REXCVAR_DEFINE_INT32(target_refreshRate, -1, "Nuts&Bolts/Performance", "The index for a given VSync setting. Options are: \n\t-1 = Unaffected\n\t0 = Immediate (Uncapped)\n\t1 = 60Hz\n\t2 = 30Hz (No Threshold)\n\t3 = 20Hz\n\t4 - 9 = Variants of 30Hz.").range(-1, 9);

// Typed import of the original generated implementation at sub_823edeb8.
REX_IMPORT(__imp__rex_setParamInterval, setParamInterval, void(int));

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


#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

REXCVAR_DEFINE_STRING(frame_cap, "Off", "Nuts&Bolts/Performance",
	"Frame-rate cap (low-overhead limiter that cuts GPU load). "
	"Off = uncapped, Display = match monitor refresh.")
	.allowed({ "Off", "30", "60", "120", "144", "Display" });


static HANDLE renutFrameTimer()
{
	static HANDLE timer = []() -> HANDLE {
		HANDLE h = CreateWaitableTimerExW(nullptr, nullptr,
			CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		if (!h) h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
		return h;
	}();
	return timer;
}

static int renutFrameCapFps()
{
	const std::string& v = REXCVAR_GET(frame_cap);
	if (v.empty() || v == "Off") return 0;
	if (v == "Display") {
		static int hz = []() -> int {
			DEVMODEW dm{}; dm.dmSize = sizeof(dm);
			if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) &&
				dm.dmDisplayFrequency > 1)
				return static_cast<int>(dm.dmDisplayFrequency);
			return 60;
		}();
		return hz;
	}
	return std::atoi(v.c_str());
}

void renutFrameLimit()
{
	using clock = std::chrono::steady_clock;
	static clock::time_point next = clock::now();

	const int fps = renutFrameCapFps();
	if (fps <= 0) {
		next = clock::now(); 
		return;
	}

	const auto frameDur = std::chrono::duration_cast<clock::duration>(
		std::chrono::duration<double>(1.0 / fps));
	next += frameDur;

	clock::time_point now = clock::now();
	if (next <= now) {
		next = now;
		return;
	}

	HANDLE timer = renutFrameTimer();
	constexpr auto kSpinTail = std::chrono::microseconds(400);
	for (;;) {
		now = clock::now();
		if (now >= next) break;
		const auto remaining = next - now;
		if (remaining > kSpinTail) {
			const auto waitFor = remaining - kSpinTail;
			if (timer) {
				LARGE_INTEGER due;
				due.QuadPart = -static_cast<LONGLONG>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(waitFor).count() / 100);
				if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
					WaitForSingleObject(timer, INFINITE);
				else
					std::this_thread::sleep_for(waitFor);
			} else {
				std::this_thread::sleep_for(waitFor);
			}
		} else {
			std::this_thread::yield();  
		}
	}
}

REXCVAR_DEFINE_BOOL(sync_shader_compile, true, "Nuts&Bolts/Graphics",
	"Compile shaders synchronously to stop the black flash when new effects first "
	"appear (small one-time stutter instead). Off = engine default (async).");

void renutApplyShaderCompileMode()
{
	static int applied = -1;
	const bool sync = REXCVAR_GET(sync_shader_compile);
	if (static_cast<int>(sync) != applied) {
		applied = static_cast<int>(sync);
		rex::cvar::SetFlagByName("async_shader_compilation", sync ? "false" : "true");
	}
}
