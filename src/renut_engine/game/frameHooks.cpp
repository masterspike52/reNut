#if defined(_WIN32)
#include <windows.h>
#else
#include <SDL3/SDL_video.h>
#endif

#include <rex/hook.h>
#include <rex/cvar.h>
#include <rex/types.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

#include "renut_engine/online/coop.h"

// Typed import of the original generated implementation at sub_823edeb8.
REX_IMPORT(__imp__rex_setParamInterval, setParamInterval, void(int));

// Klungo's arcade games run their logic once per frame, so they play too fast
// above 30 fps. While the arcade exists the frame limiter holds the game at
// 30 fps at most, whatever frame_cap says; frame_cap applies again once it goes.
static constexpr int kKlungoArcadeFps = 30;
static std::atomic<bool> g_klungoArcade{ false };

void klungoConstruct_hook() // Called at 8246e7a8 (End of the function)
{
	setParamInterval(2);
	g_klungoArcade.store(true, std::memory_order_relaxed);
}

void klungoDestruct_hook() // Called at 8246f62c (End of the function)
{
	// Back to the game's own interval: everything in the game that changes it
	// (the movie player, the loading swap) restores 7 afterwards.
	setParamInterval(7);
	g_klungoArcade.store(false, std::memory_order_relaxed);
}

// The Saucer of Peril Returns (Terrarium of Terror, act 2 game 3) never ends
// above 30 fps: the saucer ride keeps going. While it runs the frame limiter
// holds the game at 30 fps, as for Klungo's arcade. The challenge manager
// (challengeManBegin, sub_8251FF78) keeps the running challenge's asset id at
// +12, and challengeManSetState (sub_82521018) clears it at state 0 (idle).
static constexpr uint32_t kSaucerOfPerilChallenge = 0x3DDFA82A;  // aid_challenge_banjox_terrorium_act2_game3
static constexpr uint32_t kChallengeIdOffset = 12;
static std::atomic<bool> g_saucerOfPeril{ false };

REX_EXTERN(__imp__sub_821ED7F8);
REX_EXTERN(__imp__sub_825214E0);

// void challengeManTick(challengeMan*), once a frame from sub_82238360. In a
// co-op session it runs as single player (online/coop.h).
REX_HOOK_RAW(sub_821ED7F8)
{
	const uint32_t man = ctx.r3.u32;
	{
		renut::online::coop::OfflineChallenge offline(base);
		__imp__sub_821ED7F8(ctx, base);
	}
	const uint32_t id = rex::byte_swap(*reinterpret_cast<const uint32_t*>(base + man + kChallengeIdOffset));
	g_saucerOfPeril.store(id == kSaucerOfPerilChallenge, std::memory_order_relaxed);
}

// void challengeManTeardown(challengeMan*, bool). The tick may stop with the
// id still set (leaving the world), so the hold ends here too.
REX_HOOK_RAW(sub_825214E0)
{
	{
		renut::online::coop::OfflineChallenge offline(base);
		__imp__sub_825214E0(ctx, base);
	}
	g_saucerOfPeril.store(false, std::memory_order_relaxed);
}

// Havok steps N&B's physics in fixed 1/60 s steps on its own thread
// (sub_82386168): the frame sets its time marker to the current time plus
// the frame's delta (capped at four steps), and it steps the world a full
// step at a time until the current time, the earlier of the step time and the
// marker, reaches the marker. On the console's vsynced 30 fps every frame is
// two whole steps and ends on a step. At 60 fps the frames are a step long
// give or take the timer's jitter: a frame a hair short still takes a step,
// one a hair long takes two, and the world drifts ahead of the marker it is
// shown at. Whatever the game pins in place each frame (Banjo on a ledge, a
// tightrope or a ladder) is then flung about. So the frame's delta is sent as
// whole steps, the nearest number to it plus what earlier frames left over,
// and every frame ends on a step as on the console.
REXCVAR_DEFINE_BOOL(renut_physics_whole_steps, true, "Nuts&Bolts/Performance",
	"Advance physics by whole 1/60 s steps each frame, carrying the remainder, as the "
	"console's 30 fps did. Off: the game's own stepping, which flings Banjo about on "
	"ledges, tightropes and ladders at 60 fps.");

static constexpr float kPhysicsStep = 0.016666668f;  // the step sub_8227D3B8 sends
static constexpr int kMaxPhysicsSteps = 4;            // the physics thread's cap
static constexpr uint32_t kPhysicsWorldOffset = 60;
static constexpr uint32_t kPhysicsDueOffset = 232;    // cleared when the step is sent
static double g_physicsCarry = 0.0;

REX_EXTERN(__imp__sub_8227D3B8);

static uint32_t renutLoad32(const uint8_t* base, uint32_t va)
{
	return rex::byte_swap(*reinterpret_cast<const uint32_t*>(base + va));
}

// void physicsStepSend(game*, ?, float frameDelta): queues the physics
// thread's step (command 8: the delta and the 1/60 s step), from the game
// tick (sub_82261DD0, delta = time scale x frame delta) and sub_8229B330.
REX_HOOK_RAW(sub_8227D3B8)
{
	const uint32_t game = ctx.r3.u32;
	const bool due = renutLoad32(base, game + kPhysicsWorldOffset) &&
		renutLoad32(base, game + kPhysicsDueOffset);
	if (!due || !REXCVAR_GET(renut_physics_whole_steps)) {
		__imp__sub_8227D3B8(ctx, base);
		return;
	}
	const double wanted = ctx.f1.f64 + g_physicsCarry;
	const int steps = std::clamp(int(std::floor(wanted / kPhysicsStep + 0.5)), 0, kMaxPhysicsSteps);
	const double sent = double(float(steps * kPhysicsStep));
	ctx.f1.f64 = sent;
	__imp__sub_8227D3B8(ctx, base);
	// Sent unless the game had no physics world for this thread.
	if (!renutLoad32(base, game + kPhysicsDueOffset))
		g_physicsCarry = std::clamp(wanted - sent, -double(kPhysicsStep), double(kPhysicsStep));
}


REXCVAR_DEFINE_STRING(frame_cap, "Off", "Nuts&Bolts/Performance",
	"Frame-rate cap (low-overhead limiter that cuts GPU load). "
	"Off = uncapped, Display = match monitor refresh. With renut_vsync on, the "
	"display paces the game at its refresh, and only caps below it apply.")
	.allowed({ "Off", "30", "60", "120", "144", "Display" });

// gpu/backend/present.cpp
REXCVAR_DECLARE(bool, renut_vsync);


#if defined(_WIN32)

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

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

// The primary display's refresh rate, or 0 if unknown.
static int renutDisplayHz()
{
	DEVMODEW dm{}; dm.dmSize = sizeof(dm);
	if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
		return static_cast<int>(dm.dmDisplayFrequency);
	return 0;
}

// Sleeps about 'duration' (the limiter spins the rest).
static void renutSleep(std::chrono::steady_clock::duration duration)
{
	if (HANDLE timer = renutFrameTimer()) {
		LARGE_INTEGER due;
		due.QuadPart = -static_cast<LONGLONG>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() / 100);
		if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
			WaitForSingleObject(timer, INFINITE);
			return;
		}
	}
	std::this_thread::sleep_for(duration);
}

#else

// The primary display's refresh rate, or 0 if unknown (before SDL's video
// subsystem is up, for one).
static int renutDisplayHz()
{
	const SDL_DisplayID display = SDL_GetPrimaryDisplay();
	const SDL_DisplayMode* mode = display ? SDL_GetCurrentDisplayMode(display) : nullptr;
	return mode && mode->refresh_rate > 1.0f ? static_cast<int>(mode->refresh_rate + 0.5f) : 0;
}

// Linux sleeps (nanosleep) are fine-grained already.
static void renutSleep(std::chrono::steady_clock::duration duration)
{
	std::this_thread::sleep_for(duration);
}

#endif

// The user's frame_cap setting; 0 = uncapped.
static int renutUserFrameCapFps()
{
	const std::string& v = REXCVAR_GET(frame_cap);
	if (v.empty() || v == "Off") return 0;
	// Asked once it is known.
	static int hz = 0;
	if (!hz) hz = renutDisplayHz();
	// With vsync the display paces the game at its refresh. A cap at or above
	// it would only fight that: the limiter runs off the rounded refresh rate,
	// drifts against the real one, and a frame now and then misses its vblank.
	const bool vsync = REXCVAR_GET(renut_vsync);
	if (v == "Display")
		return vsync ? 0 : (hz ? hz : 60);
	const int fps = std::atoi(v.c_str());
	return vsync && hz && fps >= hz ? 0 : fps;
}

// What the limiter holds the game to: the user's cap, lowered to 30 fps in
// Klungo's arcade and The Saucer of Peril Returns (a lower user cap stays).
static int renutFrameCapFps()
{
	const int fps = renutUserFrameCapFps();
	if (!g_klungoArcade.load(std::memory_order_relaxed) &&
		!g_saucerOfPeril.load(std::memory_order_relaxed))
		return fps;
	return fps > 0 ? std::min(fps, kKlungoArcadeFps) : kKlungoArcadeFps;
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

	constexpr auto kSpinTail = std::chrono::microseconds(400);
	for (;;) {
		now = clock::now();
		if (now >= next) break;
		const auto remaining = next - now;
		if (remaining > kSpinTail) {
			renutSleep(remaining - kSpinTail);
		} else {
			std::this_thread::yield();  
		}
	}
}
