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
#include <cstring>
#include <string>
#include <thread>

#include "renut_engine/game/stereo.h"

// Typed import of the original generated implementation at sub_823BBE58.
REX_IMPORT(__imp__rex_setParamInterval, setParamInterval, void(int));

// Klungo's arcade games run their logic once per frame, so they play too fast
// above 30 fps. While the arcade exists the frame limiter holds the game at
// 30 fps at most, whatever frame_cap says; frame_cap applies again once it goes.
static constexpr int kKlungoArcadeFps = 30;
static std::atomic<bool> g_klungoArcade{ false };

void klungoConstruct_hook() // Called at 8244a7a0 (End of the function)
{
	setParamInterval(2);
	g_klungoArcade.store(true, std::memory_order_relaxed);
}

void klungoDestruct_hook() // Called at 8244b744 (End of the function)
{
	// Back to the game's own interval: everything in the game that changes it
	// (the movie player, the loading swap) restores 7 afterwards.
	setParamInterval(7);
	g_klungoArcade.store(false, std::memory_order_relaxed);
}

// Klungo's arcade 2 (the DLC) is its own game object (sub_823C47E0 builds it,
// sub_823C68A8 tears it down, dword_82E19AD8 points at it meanwhile), held at
// 30 fps the same way.
void klungoDlcConstruct_hook() // Called at 823c519c (End of the function)
{
	klungoConstruct_hook();
}

void klungoDlcDestruct_hook() // Called at 823c6b80 (End of the function)
{
	klungoDestruct_hook();
}

// The Saucer of Peril Returns (Terrarium of Terror, act 2 game 3) never ends
// above 30 fps: the saucer ride keeps going. While it runs the frame limiter
// holds the game at 30 fps, as for Klungo's arcade. The challenge manager
// (challengeManBegin, sub_824FDC38) keeps the running challenge's asset id at
// +12, and challengeManSetState (sub_824FEDD8) clears it at state 0 (idle).
static constexpr uint32_t kSaucerOfPerilChallenge = 0x3DDFA82A;  // aid_challenge_banjox_terrorium_act2_game3
static constexpr uint32_t kChallengeIdOffset = 12;
// challengeMan + 20: its state (challengeManSetState). The tick treats 6 to
// 17 as the challenge in play (it updates the players' runs then); before are
// talking to its giver, the objective and the intro, after the results.
static constexpr uint32_t kChallengeStateOffset = 20;
static constexpr int32_t kChallengePlayFirst = 6;
static constexpr int32_t kChallengePlayEnd = 18;
static std::atomic<bool> g_saucerOfPeril{ false };

REX_EXTERN(__imp__sub_821BD590);
REX_EXTERN(__imp__sub_824FF2F0);

// void challengeManTick(challengeMan*), once a frame from sub_82207320. The
// running challenge also picks reNut's challenge songs (stereo.h).
REX_HOOK_RAW(sub_821BD590)
{
	const uint32_t man = ctx.r3.u32;
	__imp__sub_821BD590(ctx, base);
	const uint32_t id = rex::byte_swap(*reinterpret_cast<const uint32_t*>(base + man + kChallengeIdOffset));
	g_saucerOfPeril.store(id == kSaucerOfPerilChallenge, std::memory_order_relaxed);
	const int32_t state = int32_t(rex::byte_swap(*reinterpret_cast<const uint32_t*>(base + man + kChallengeStateOffset)));
	renut::game::ChallengeMusicTick(ctx, base, id, state >= kChallengePlayFirst && state < kChallengePlayEnd);
}

// void challengeManTeardown(challengeMan*, bool). The tick may stop with the
// id still set (leaving the world), so the hold and the song end here too.
REX_HOOK_RAW(sub_824FF2F0)
{
	__imp__sub_824FF2F0(ctx, base);
	g_saucerOfPeril.store(false, std::memory_order_relaxed);
	renut::game::ChallengeMusicTick(ctx, base, 0, false);
}

// Havok steps the physics on its own thread (sub_82353060, command 8 from
// physicsStepSend, sub_8224B3A8): fixed 1/60 s steps up to a time marker set
// from the frame delta (at most four steps). On the console's 30 fps every
// frame was exactly two steps. At 60 fps and above, timer jitter makes frames
// take zero or two steps, which flings about whatever the game pins in place
// each frame (Banjo on ledges and ladders, vehicles).
//
// Fix: send each frame a whole number of steps, carrying the remainder. Above
// 60 fps the step is split into n (1/120 s, 1/180 s, ...) so a step is about a
// frame long and every frame takes one. Split steps cost the same CPU each,
// and the four-step cap then covers a shorter hitch.
REXCVAR_DEFINE_BOOL(renut_physics_whole_steps, true, "Nuts&Bolts/Performance",
	"Advance physics by whole steps each frame. Off: the game's own stepping, which "
	"flings Banjo about on ledges and ladders at 60 fps and above.");

REXCVAR_DEFINE_BOOL(renut_physics_substeps, true, "Nuts&Bolts/Performance",
	"Above 60 fps, split the physics step to match the frame rate so every frame "
	"advances physics. Needs renut_physics_whole_steps.");

static constexpr float kPhysicsStep = 0.016666668f;  // the step sub_8224B3A8 sends
static constexpr int kMaxPhysicsSteps = 4;            // the physics thread's cap (x step)
static constexpr int kMaxSubsteps = 8;                // 1/480 s, for up to 480 fps
// Keeps timer noise near a multiple of 60 fps from changing the split.
static constexpr double kSubstepSlack = 0.1;
static constexpr double kSubstepHysteresis = 0.1;

REX_EXTERN(__imp__sub_82353060);

// Physics thread state (sub_82353060 runs only there).
static double g_physicsCarry = 0.0;          // seconds the steps sent are behind
static std::atomic<int> g_physicsSubsteps{ 1 };  // n: the 1/60 s step split in n
static double g_physicsFrameAverage = 0.0;   // seconds between frames, smoothed
static std::chrono::steady_clock::time_point g_physicsLastFrame{};

// n from the measured frame interval (not the game's delta, which slow
// motion scales).
static int renutPhysicsSubsteps()
{
	using clock = std::chrono::steady_clock;
	const clock::time_point now = clock::now();
	const double interval = g_physicsLastFrame.time_since_epoch().count() == 0
		? 0.0 : std::chrono::duration<double>(now - g_physicsLastFrame).count();
	g_physicsLastFrame = now;

	if (!REXCVAR_GET(renut_physics_substeps)) {
		g_physicsFrameAverage = 0.0;
		return 1;
	}
	// Ignore pauses, loading and hitches.
	if (interval > 0.0 && interval < 0.1)
		g_physicsFrameAverage = g_physicsFrameAverage > 0.0
			? g_physicsFrameAverage + (interval - g_physicsFrameAverage) * 0.1
			: interval;
	if (g_physicsFrameAverage <= 0.0)
		return g_physicsSubsteps.load(std::memory_order_relaxed);

	const double frames = double(kPhysicsStep) / g_physicsFrameAverage;  // frames a 1/60 s step
	// Fewest splits that make a step no longer than a frame.
	const int fit = std::max(1, int(std::ceil(frames - kSubstepSlack)));
	int n = g_physicsSubsteps.load(std::memory_order_relaxed);
	if (fit > n || frames < n - 1 - kSubstepHysteresis)
		n = fit;
	return std::clamp(n, 1, kMaxSubsteps);
}

// void physicsStep(float frameDelta, float step), physics thread.
REX_HOOK_RAW(sub_82353060)
{
	const int substeps = renutPhysicsSubsteps();
	if (!REXCVAR_GET(renut_physics_whole_steps) || float(ctx.f2.f64) != kPhysicsStep) {
		g_physicsCarry = 0.0;
		g_physicsSubsteps.store(1, std::memory_order_relaxed);
		__imp__sub_82353060(ctx, base);
		return;
	}
	g_physicsSubsteps.store(substeps, std::memory_order_relaxed);
	const float step = kPhysicsStep / float(substeps);
	const double wanted = ctx.f1.f64 + g_physicsCarry;
	int steps = int(std::floor(wanted / step + 0.5));
	// With split steps a frame takes at least one step, unless physics is
	// already well ahead.
	if (substeps > 1 && ctx.f1.f64 >= 0.5 * step && wanted >= 0.25 * step)
		steps = std::max(steps, 1);
	// The physics thread's own cap.
	steps = std::clamp(steps, 0, kMaxPhysicsSteps);
	const double sent = double(float(steps) * step);
	g_physicsCarry = std::clamp(wanted - sent, -double(step), double(step));
	ctx.f1.f64 = sent;
	ctx.f2.f64 = double(step);
	__imp__sub_82353060(ctx, base);
}

static uint32_t renutLoadU32(const uint8_t* base, uint32_t va)
{
	return rex::byte_swap(*reinterpret_cast<const uint32_t*>(base + va));
}

// Vehicle steering (vtable 0x82115B2C, calcSteering sub_8233FA80) moves each
// wheel's angle towards its target with sub_82210520, which wraps to [0, 2pi).
// While steering the angles are kept in [-pi, pi), but with the stick centred
// they are not: a wheel released from a left turn can settle just under 2pi,
// which reads as full steering lock, so the vehicle keeps turning. Short
// (split) steps hit this almost every time. Wrap the angles back to [-pi, pi).

static constexpr uint32_t kSteeringWheelCountOffset = 12;  // steering + 12: wheel count
static constexpr uint32_t kSteeringOutWheelsOffset = 4;    // out + 4: wheel angles

REX_EXTERN(__imp__sub_8233FA80);

// void calcSteering(this, float dt, vehicle, driverInput, SteeringAnglesOutput* out)
REX_HOOK_RAW(sub_8233FA80)
{
	const uint32_t steering = ctx.r3.u32;
	const uint32_t out = ctx.r7.u32;
	__imp__sub_8233FA80(ctx, base);
	if (!steering || !out)
		return;
	const int32_t count = int32_t(renutLoadU32(base, steering + kSteeringWheelCountOffset));
	const uint32_t wheels = renutLoadU32(base, out + kSteeringOutWheelsOffset);
	if (!wheels || count <= 0 || count > 64)
		return;
	constexpr float kPi = 3.14159265f;
	constexpr float kTwoPi = 6.28318531f;
	for (int32_t i = 0; i < count; ++i) {
		uint32_t* slot = reinterpret_cast<uint32_t*>(base + wheels + 4 * uint32_t(i));
		const uint32_t raw = rex::byte_swap(*slot);
		float angle;
		std::memcpy(&angle, &raw, sizeof(angle));
		if (!(angle >= kPi || angle < -kPi))
			continue;
		angle = angle >= kPi ? angle - kTwoPi : angle + kTwoPi;
		uint32_t fixed;
		std::memcpy(&fixed, &angle, sizeof(fixed));
		*slot = rex::byte_swap(fixed);
	}
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
