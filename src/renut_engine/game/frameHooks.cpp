#if defined(_WIN32)
#include <windows.h>
#else
#include <SDL3/SDL_video.h>
#endif

#include <rex/ppc/hook.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>
#include <rex/types.h>
#include <rexfabric/project_hooks.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "renut_engine/app/renut_settings.h"
#include "renut_engine/game/custom_npcs.h"
#include "renut_engine/game/part_api.h"
#include "renut_engine/game/stereo.h"
#include "renut_engine/ui/position_overlay.h"

namespace {

struct FrameSettings final : rex::SettingOwner {
	FrameSettings() : rex::SettingOwner(renut::settings::kScope) { renut::settings::Add(*this); }
	REX_SETTING(bool, debug_player_position, false,
		"Shows Banjo's position (x, y, z) in the top right: the coordinates world doors, characters and "
		"markers take.");
	REX_SETTING(bool, renut_physics_whole_steps, true,
		"Advance physics by whole steps each frame. Off: the game's own stepping, which "
		"flings Banjo about on ledges and ladders at 60 fps and above.");
	REX_SETTING(bool, renut_physics_substeps, true,
		"Above 60 fps, shorten the physics step to a frame's length so every frame "
		"advances physics by one step. Needs renut_physics_whole_steps.");
	REX_SETTING(std::string, frame_cap, "60",
		"Frame-rate cap (low-overhead limiter that cuts GPU load). "
		"Off = uncapped, Display = match monitor refresh. With renut_vsync on, the "
		"display paces the game at its refresh, and only caps below it apply.",
		rex::Allowed("Off", "30", "60", "120", "144", "Display"));
};
FrameSettings &g_frame = *new FrameSettings();  // never freed, registered by RenutApp::OnConfigure

}  // namespace

namespace renut::debug {

// debug_player_position, for the position overlay (ui/position_overlay.h).
bool PlayerPositionEnabled() {
	return g_frame.debug_player_position.Get();
}

}  // namespace renut::debug

// Typed import of the original generated implementation at sub_823BBE58.
REX_IMPORT(__imp__rex_setParamInterval, SetParamInterval, void(int));

// Klungo's arcade games run their logic once per frame, so while the arcade
// exists the frame limiter holds the game at 30 fps at most.
static constexpr int kKlungoArcadeFps = 30;
static std::atomic<bool> g_klungoArcade{ false };

void klungoConstruct_hook() // Called at 8244a7a0 (End of the function)
{
	SetParamInterval(2);
	g_klungoArcade.store(true, std::memory_order_relaxed);
}

void klungoDestruct_hook() // Called at 8244b744 (End of the function)
{
	// Back to the game's own interval: everything in the game that changes it
	// (the movie player, the loading swap) restores 7 afterwards.
	SetParamInterval(7);
	g_klungoArcade.store(false, std::memory_order_relaxed);
}

// Klungo's arcade 2 (the DLC; built by sub_823C47E0, torn down by
// sub_823C68A8), held at 30 fps the same way.
void klungoDlcConstruct_hook() // Called at 823c519c (End of the function)
{
	klungoConstruct_hook();
}

void klungoDlcDestruct_hook() // Called at 823c6b80 (End of the function)
{
	klungoDestruct_hook();
}

// The Saucer of Peril Returns never ends above 30 fps (the ride stops short of
// its last gate), so it is held at 30 fps; challengeMan + 12 is the running id.
static constexpr uint32_t kSaucerOfPerilChallenge = 0x3DDFA82A;  // aid_challenge_banjox_terrorium_act2_game3
static constexpr uint32_t kChallengeIdOffset = 12;
// challengeMan + 20: its state (challengeManSetState); 6 to 17 is the
// challenge in play.
static constexpr uint32_t kChallengeStateOffset = 20;
static constexpr int32_t kChallengePlayFirst = 6;
static constexpr int32_t kChallengePlayEnd = 18;
static std::atomic<bool> g_saucerOfPeril{ false };

REXFABRIC_HOOKABLE(sub_821BD590);  // mods' rex.hook on the tick run around the game's own
REX_EXTERN(__imp__sub_824FF2F0);

REX_EXTERN(__imp__sub_821CD438);

// An avatar's position (entity + 80, as the camera reads it).
static constexpr uint32_t kAvatarPosition = 80;

// void sub_821CD438(player, , avatar, world): the player's camera, each frame, on foot or driving
// (Showdown Town included); it aims at the avatar's position. The position overlay reads Banjo's here.
REX_HOOK_RAW(sub_821CD438)
{
	const uint32_t avatar = ctx.r5.u32;
	renut::game::custom_npcs::SetPlayerAvatar(avatar);  // renut.player
	if (avatar && g_frame.debug_player_position.Get()) {
		auto loadf = [&](uint32_t va) {
			const uint32_t bits = rex::byte_swap(*reinterpret_cast<const uint32_t*>(base + va));
			float f;
			std::memcpy(&f, &bits, 4);
			return f;
		};
		renut::debug::PlayerPosition &p = renut::debug::g_player_position;
		p.x.store(loadf(avatar + kAvatarPosition), std::memory_order_relaxed);
		p.y.store(loadf(avatar + kAvatarPosition + 4), std::memory_order_relaxed);
		p.z.store(loadf(avatar + kAvatarPosition + 8), std::memory_order_relaxed);
		p.known.store(true, std::memory_order_release);
	}
	__imp__sub_821CD438(ctx, base);
}

// void challengeManTick(challengeMan*), once a frame from sub_82207320. The
// running challenge also picks reNut's challenge songs (stereo.h).
REX_HOOK_RAW(sub_821BD590)
{
	const uint32_t man = ctx.r3.u32;
	REXFABRIC_ORIGINAL(sub_821BD590)(ctx, base);
	const uint32_t id = rex::byte_swap(*reinterpret_cast<const uint32_t*>(base + man + kChallengeIdOffset));
	g_saucerOfPeril.store(id == kSaucerOfPerilChallenge, std::memory_order_relaxed);
	const int32_t state = int32_t(rex::byte_swap(*reinterpret_cast<const uint32_t*>(base + man + kChallengeStateOffset)));
	renut::game::ChallengeMusicTick(ctx, base, id, state >= kChallengePlayFirst && state < kChallengePlayEnd);
	renut::game::WorldMusicTick(ctx, base, renut::game::custom_npcs::ActiveScene());
	renut::game::NpcSoundsTick(ctx, base, renut::game::custom_npcs::ActiveScene());
	renut::game::part_api::GameTick(ctx, base);  // renut.game_tick, and the mods' calls waiting for it
}

// void challengeManTeardown(challengeMan*, bool). The tick may stop with the
// id still set (leaving the world), so the hold and the song end here too.
REX_HOOK_RAW(sub_824FF2F0)
{
	__imp__sub_824FF2F0(ctx, base);
	g_saucerOfPeril.store(false, std::memory_order_relaxed);
	renut::debug::g_player_position.known.store(false, std::memory_order_release);
	renut::game::custom_npcs::SetPlayerAvatar(0);
	renut::game::ChallengeMusicTick(ctx, base, 0, false);
}

// Havok's physics step (sub_82353060) gets a whole number of steps each frame,
// carrying the remainder; above 60 fps the step is shortened to a frame's length.
static constexpr float kPhysicsStep = 0.016666668f;  // the step sub_8224B3A8 sends
static constexpr int kMaxPhysicsSteps = 4;            // the physics thread's cap (x step)
static constexpr double kMaxSplit = 8.0;              // 1/480 s, for up to 480 fps

REX_EXTERN(__imp__sub_82353060);

// Physics thread state (sub_82353060 runs only there).
static double g_physicsCarry = 0.0;          // seconds the steps sent are behind
static double g_physicsFrameAverage = 0.0;   // seconds between frames, smoothed
static std::chrono::steady_clock::time_point g_physicsLastFrame{};

// How many steps the 1/60 s step is cut into (1 at 60 fps and below, 2.4 at
// 144 fps), from the measured frame interval.
static double renutPhysicsSplit()
{
	using clock = std::chrono::steady_clock;
	const clock::time_point now = clock::now();
	const double interval = g_physicsLastFrame.time_since_epoch().count() == 0
		? 0.0 : std::chrono::duration<double>(now - g_physicsLastFrame).count();
	g_physicsLastFrame = now;

	if (!g_frame.renut_physics_substeps.Get()) {
		g_physicsFrameAverage = 0.0;
		return 1.0;
	}
	// Ignore pauses, loading and hitches.
	if (interval > 0.0 && interval < 0.1)
		g_physicsFrameAverage = g_physicsFrameAverage > 0.0
			? g_physicsFrameAverage + (interval - g_physicsFrameAverage) * 0.1
			: interval;
	if (g_physicsFrameAverage <= 0.0)
		return 1.0;
	// Frames per 1/60 s step.
	return std::clamp(double(kPhysicsStep) / g_physicsFrameAverage, 1.0, kMaxSplit);
}

// void physicsStep(float frameDelta, float step), physics thread.
REX_HOOK_RAW(sub_82353060)
{
	const double split = renutPhysicsSplit();
	if (!g_frame.renut_physics_whole_steps.Get() ||float(ctx.f2.f64) != kPhysicsStep) {
		g_physicsCarry = 0.0;
		__imp__sub_82353060(ctx, base);
		return;
	}
	float step = float(double(kPhysicsStep) / split);
	double wanted = ctx.f1.f64 + g_physicsCarry;
	// A hitch: 1/60 s steps catch up with it.
	if (step < kPhysicsStep && wanted > kMaxPhysicsSteps * double(step)) {
		step = kPhysicsStep;
		wanted = ctx.f1.f64;
	}
	int steps = int(std::floor(wanted / step + 0.5));
	// With short steps a frame takes at least one step, unless physics is
	// already well ahead.
	if (step < kPhysicsStep && ctx.f1.f64 >= 0.5 * step && wanted >= 0.25 * step)
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

// Vehicle steering (calcSteering sub_8233FA80) can leave a centred wheel's angle
// just under 2pi (full lock); wraps the angles back to [-pi, pi).

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

// renut_vsync (gpu/backend/present.cpp), found once it is registered; its default (on) until then.
static bool renutVsync()
{
	static std::atomic<rex::Setting<bool>*> vsync{ nullptr };
	rex::Setting<bool>* setting = vsync.load(std::memory_order_acquire);
	if (!setting) {
		auto found = REX_SETTINGS().Find("renut.renut_vsync");
		if (!found || (*found)->GetType() != rex::SettingType::kBool)
			return true;
		setting = static_cast<rex::Setting<bool>*>(*found);
		vsync.store(setting, std::memory_order_release);
	}
	return setting->Get();
}


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
	const std::string v = g_frame.frame_cap.Get();
	if (v.empty() || v == "Off") return 0;
	// Asked once it is known.
	static int hz = 0;
	if (!hz) hz = renutDisplayHz();
	// With vsync the display paces the game, so a cap at or above the refresh
	// is dropped (it would drift against vblank).
	const bool vsync = renutVsync();
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
