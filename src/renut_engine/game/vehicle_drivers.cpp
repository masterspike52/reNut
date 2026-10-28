/**
 * @file    game/vehicle_drivers.cpp
 * @brief   The Remote Control: a vehicle driven by a pad of reNut's own.
 *
 * N&B (TU3, IDA): the part is whole (restored_parts.cpp gives it) but has no
 * behaviour left: its descriptor (getter sub_82618A18 -> 0x82E31890) has the
 * generic accessory tick ([7] sub_82600A18), no button slots ([18] down,
 * [19] up, as the Stereo's) and the base HUD button state ([24] 0, greyed);
 * its attach only counts vehicle + 3547, which nothing reads. (The Autopilot,
 * 0x82600EA8 -> 0x82E30FE8, is the same; scrapped by the user.)
 *
 * A vehicle reads its controls from its driver's input device:
 * sub_82201350(vehicle, any) walks its occupants (vehicle + 6376: {+ 4 next,
 * + 8 device, + 12 actor, + 16 seated}) and returns the driver's device, or 0
 * (about twenty readers: engines, wheels, steering, part buttons). A device
 * keeps its pad record at + 16 ({+ 0 the current entry's index, + 4 two
 * 40-byte entries}: + 0 pressed, + 4 released, + 8 held, + 12 now (XInput
 * buttons, + 0x10000 / 0x20000 the triggers), + 16 / + 20 the left stick,
 * + 24 / + 28 the right, + 32 / + 36 the triggers); each frame
 * inputDeviceUpdatePlayerPad (0x82272550) pushes the port's entry into the
 * player's (padStatePush sub_821CB628, hooked by keyboard_input.cpp, which
 * calls VehicleDriversPadPush). Banjo's drive state (entityStateDriveVehicle,
 * tick sub_8245A1F8) gets out on Y (its objparams' 0x8000), and the pause
 * opens on Start pressed (sub_8243C148). The camera update
 * sub_822C0448(camera, device r4, dt, ..., mode r6, ..., target r8)
 * (mouse_camera.cpp's hook calls VehicleDriversCamera) follows its target.
 *
 * reNut gives the vehicle a device of its own (a copy of the player's, its
 * pad record fed here) while the Remote Control drives it: on a press Banjo
 * gets out (a Y press), then the vehicle takes the player's pad and Banjo an
 * idle one (Start and Back kept), and player 1's camera follows the vehicle;
 * press again (or get back in) to stop.
 *
 * Propellers (vehicleBlockJetEngine, tick sub_8222E850) spin only with someone
 * in a driver's seat: sub_8223EBF0(vehicle, type) counts the occupants of its
 * seats of a type (vehicle + 4640: {type, seat}, the seat's three at + 840;
 * type 2 a driver's seat, sub_82617388), and with none the tick zeroes the
 * propeller's speed (+ 1364, + 1360) and the spin (sub_82612020) winds down,
 * after the thrust (+ 1440 ... + 1504, applied by [8] sub_8223FF30) is set: the
 * vehicle flies with its propellers still. While the tick runs for the vehicle
 * the Remote Control drives, its driver's seat counts as taken.
 */
#include "renut_engine/game/vehicle_drivers.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "renut_engine/game/banner.h"

REX_EXTERN(__imp__sub_82618A18);  // the Remote Control's descriptor getter
REX_EXTERN(__imp__sub_82201350);  // a vehicle's driver's input device
REX_EXTERN(__imp__sub_82200AA0);  // a vehicle's driver (actor)
REX_EXTERN(__imp__sub_821CB628);  // padStatePush(record, entry)
REX_EXTERN(__imp__sub_8222E850);  // a propeller's / jet engine's tick
REX_EXTERN(__imp__sub_8223EBF0);  // a vehicle's occupants of its seats of a type

namespace {

using Clock = std::chrono::steady_clock;

// Descriptor slots.
constexpr uint32_t kSlotTick = 7, kSlotButtonDown = 18, kSlotButtonUp = 19;
// [24] (part, vehicle): its button's HUD icon state, 0 greyed (the base
// sub_826B9BC8, which the part kept), 2 / 3 usable; the horn's
// (sub_82602458: 2 | the vehicle's + 6368 bit 0 clear) for it.
constexpr uint32_t kSlotButtonState = 24;
constexpr uint32_t kHornButtonState = 0x82602458;

// Input devices and pad entries.
constexpr uint32_t kDeviceRecord = 16;
constexpr uint32_t kDeviceCopySize = 0x800;
constexpr uint32_t kEntrySize = 40;
constexpr uint32_t kEntryPressed = 0, kEntryReleased = 4, kEntryHeld = 8, kEntryNow = 12;
constexpr uint32_t kStart = 0x0010, kBack = 0x0020, kY = 0x8000;

// The camera (sub_822C0448): + 340 a one-shot target that replaces the given
// one (cleared each update); + 644 set when one was used, and then a target
// over 30 units from the last looked at (+ 976) cuts the camera (+ 268 / 272).
constexpr uint32_t kCameraOverride = 340, kCameraWasOverridden = 644;
// The avatar's camera update (sub_821CD438; camera = avatar + 3064) puts the
// look-at point in the camera's + 448 itself before updating it: the avatar's
// position (+ 80) raised by half his height (+ 2236 x + 612 x + 168, x 0.5),
// Banjo's even with the vehicle the target: with him left behind, the camera
// is pulled back to him (a flick past a distance).
constexpr uint32_t kCameraLookAt = 448;
constexpr uint32_t kEntityPosition = 80;
constexpr uint32_t kEntityHeightA = 2236, kEntityHeightB = 612, kEntityHeightC = 168;

// sub_8223EBF0's seat type: a driver's seat.
constexpr uint32_t kSeatDriver = 2;

// Banjo out within this, or it gives up; presses this soon after it starts
// driving are the starting one still held.
constexpr auto kExitWait = std::chrono::seconds(3);
constexpr auto kRemoteSettle = std::chrono::milliseconds(1000);
// A part not updated this long: its vehicle is gone.
constexpr auto kPartGone = std::chrono::milliseconds(500);

uint32_t Load32(const uint8_t *base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return rex::byte_swap(v);
}

void Store32(uint8_t *base, uint32_t va, uint32_t value) {
  value = rex::byte_swap(value);
  std::memcpy(base + va, &value, 4);
}

float LoadFloat(const uint8_t *base, uint32_t va) {
  const uint32_t bits = Load32(base, va);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

void StoreFloat(uint8_t *base, uint32_t va, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  Store32(base, va, bits);
}

enum class Mode { None, Remote };
enum class Phase { Off, Exiting, Driving };

struct Drive {
  Mode mode = Mode::None;
  Phase phase = Phase::Off;
  uint32_t part = 0;
  uint32_t vehicle = 0;
  uint32_t player_device = 0;  // the driver's (Banjo's) device
  uint32_t driver = 0;         // the driver (Banjo), from when he sat in it
  Clock::time_point since{};
  Clock::time_point last_tick{};
  bool exit_pressed = false;  // the Y press made
  // The Remote Control: when it started driving; a press seen while Banjo got
  // out, or just after (the vehicle sees the button still held as a new
  // press), waits for its release.
  Clock::time_point driving_since{};
  bool wait_release = false;
};

std::recursive_mutex g_mutex;
Drive g_drive;
uint32_t g_remote_descriptor = 0;
uint32_t g_remote_tick = 0;  // its original tick slot
uint32_t g_thunk_tick = 0, g_thunk_down = 0, g_thunk_up = 0;
uint32_t g_vehicle_camera_mode = 0;  // the camera's mode while one drives
uint32_t g_last_vehicle = 0;         // the vehicle of the last part updated
// The vehicle the Remote Control drives while one of its propellers' ticks runs
// on this thread, else 0.
thread_local uint32_t g_propeller_vehicle = 0;

// Guest memory: the vehicle's device (a copy of the player's), then an entry
// to push.
uint32_t g_guest = 0;
uint32_t FakeDevice() {
  return g_guest;
}
uint32_t EntryA() {
  return g_guest + kDeviceCopySize;
}

bool EnsureGuest() {
  if (g_guest)
    return true;
  const uint32_t block = REX_KERNEL_MEMORY()->SystemHeapAlloc(kDeviceCopySize + 48 + 16);
  if (!block)
    return false;
  g_guest = (block + 15) & ~15u;
  return true;
}

void CallGuest(PPCContext &ctx, uint8_t *base, uint32_t va) {
  if (::PPCFunc *fn = va ? REX_KERNEL_STATE()->function_dispatcher()->GetFunction(va) : nullptr)
    fn(ctx, base);
}

// The vehicle's driver's device, as the game has it (0: none).
uint32_t DriverDevice(PPCContext &ctx, uint8_t *base, uint32_t vehicle) {
  ctx.r3.u64 = vehicle;
  ctx.r4.u64 = 1;
  __imp__sub_82201350(ctx, base);
  return ctx.r3.u32;
}

void Push(PPCContext &ctx, uint8_t *base, uint32_t record, uint32_t entry) {
  ctx.r3.u64 = record;
  ctx.r4.u64 = entry;
  __imp__sub_821CB628(ctx, base);
}

// An idle entry from 'entry': only its Start and Back.
void MakeIdle(uint8_t *base, uint32_t from, uint32_t to) {
  std::memset(base + to, 0, kEntrySize);
  for (uint32_t field : {kEntryPressed, kEntryReleased, kEntryHeld, kEntryNow})
    Store32(base, to + field, Load32(base, from + field) & (kStart | kBack));
}

void Stop(const char *banner) {
  if (banner)
    renut::game::ShowBanner(banner);
  g_drive = {};
}

// --- The Remote Control ---------------------------------------------------------

void RemoteDown(PPCContext &ctx, uint8_t *base, uint32_t part, uint32_t vehicle) {
  if (g_drive.mode == Mode::Remote && g_drive.part == part) {
    if (g_drive.phase != Phase::Driving || Clock::now() - g_drive.driving_since < kRemoteSettle) {
      g_drive.wait_release = true;
      return;
    }
    if (g_drive.wait_release)
      return;
    Stop("Remote control: off");
    return;
  }
  if (g_drive.mode != Mode::None)
    return;
  const uint32_t device = DriverDevice(ctx, base, vehicle);
  if (!device || !EnsureGuest())
    return;
  ctx.r3.u64 = vehicle;
  __imp__sub_82200AA0(ctx, base);
  const uint32_t driver = ctx.r3.u32;
  g_drive = {};
  g_drive.mode = Mode::Remote;
  g_drive.phase = Phase::Exiting;
  g_drive.part = part;
  g_drive.vehicle = vehicle;
  g_drive.player_device = device;
  g_drive.driver = driver;
  g_drive.since = g_drive.last_tick = Clock::now();
}

void RemoteTick(PPCContext &ctx, uint8_t *base) {
  const auto now = Clock::now();
  const uint32_t driver = DriverDevice(ctx, base, g_drive.vehicle);
  if (g_drive.phase == Phase::Exiting) {
    if (!driver) {
      std::memcpy(base + FakeDevice(), base + g_drive.player_device, kDeviceCopySize);
      g_drive.phase = Phase::Driving;
      g_drive.driving_since = now;
      renut::game::ShowBanner("Remote control: on (press it again to stop)");
    } else if (now - g_drive.since > kExitWait) {
      Stop(nullptr);
    }
    return;
  }
  if (driver)
    Stop(nullptr);  // someone got back in
}

// --- Descriptor slots --------------------------------------------------------------

bool IsRemote(const uint8_t *base, uint32_t part) {
  return part && g_remote_descriptor && Load32(base, part) == g_remote_descriptor;
}

// [7] tick(part, vehicle, double dt): the original, then the Remote's.
void SlotTick(PPCContext &ctx, uint8_t *base) {
  const uint32_t part = ctx.r3.u32, vehicle = ctx.r4.u32;
  CallGuest(ctx, base, g_remote_tick);
  std::lock_guard lock(g_mutex);
  g_last_vehicle = vehicle;
  if (g_drive.mode == Mode::None || g_drive.part != part)
    return;
  g_drive.last_tick = Clock::now();
  RemoteTick(ctx, base);
}

// [18] button down(part, vehicle).
void SlotButtonDown(PPCContext &ctx, uint8_t *base) {
  const uint32_t part = ctx.r3.u32, vehicle = ctx.r4.u32;
  std::lock_guard lock(g_mutex);
  if (IsRemote(base, part))
    RemoteDown(ctx, base, part, vehicle);
}

// [19] button up(part, vehicle).
void SlotButtonUp(PPCContext &ctx, uint8_t *base) {
  const uint32_t part = ctx.r3.u32;
  std::lock_guard lock(g_mutex);
  if (IsRemote(base, part) && g_drive.mode == Mode::Remote && g_drive.part == part &&
      g_drive.phase == Phase::Driving)
    g_drive.wait_release = false;
}

// Gives a descriptor (filled by its getter) the slots: tick (its own kept to
// call first), button down / up, the button's HUD state.
void Patch(uint8_t *base, uint32_t descriptor, uint32_t &descriptor_out, uint32_t &tick_out) {
  if (!descriptor || descriptor_out)
    return;
  auto *dispatcher = REX_KERNEL_STATE()->function_dispatcher();
  if (!g_thunk_tick) {
    g_thunk_tick = dispatcher->AllocateThunk(SlotTick, 0);
    g_thunk_down = dispatcher->AllocateThunk(SlotButtonDown, 0);
    g_thunk_up = dispatcher->AllocateThunk(SlotButtonUp, 0);
  }
  if (!g_thunk_tick || !g_thunk_down || !g_thunk_up)
    return;
  tick_out = Load32(base, descriptor + 4 * kSlotTick);
  Store32(base, descriptor + 4 * kSlotTick, g_thunk_tick);
  Store32(base, descriptor + 4 * kSlotButtonDown, g_thunk_down);
  Store32(base, descriptor + 4 * kSlotButtonUp, g_thunk_up);
  Store32(base, descriptor + 4 * kSlotButtonState, kHornButtonState);
  descriptor_out = descriptor;
}

// The player's pad pushed (see VehicleDriversPadPush). Requires g_mutex.
bool PadPush(PPCContext &ctx, uint8_t *base, uint32_t record, uint32_t entry) {
  if (!entry || !g_guest)
    return false;
  // A part not updated for a while: its vehicle is gone.
  if (g_drive.mode != Mode::None && Clock::now() - g_drive.last_tick > kPartGone && g_drive.phase == Phase::Driving)
    Stop(nullptr);
  // Only the driver's device.
  if (g_drive.mode == Mode::None || record - kDeviceRecord != g_drive.player_device)
    return false;
  if (g_drive.phase == Phase::Exiting) {
    if (!g_drive.exit_pressed) {
      g_drive.exit_pressed = true;
      MakeIdle(base, entry, EntryA());
      Store32(base, EntryA() + kEntryPressed, Load32(base, EntryA() + kEntryPressed) | kY);
      Store32(base, EntryA() + kEntryNow, Load32(base, EntryA() + kEntryNow) | kY);
    } else {
      MakeIdle(base, entry, EntryA());
    }
    Push(ctx, base, record, EntryA());
    return true;
  }
  // The vehicle takes the pad, Banjo an idle one.
  Push(ctx, base, FakeDevice() + kDeviceRecord, entry);
  MakeIdle(base, entry, EntryA());
  Push(ctx, base, record, EntryA());
  return true;
}

}  // namespace

namespace renut::game {

bool VehicleDriversPadPush(PPCContext &ctx, uint8_t *base) {
  std::lock_guard lock(g_mutex);
  const uint32_t record = ctx.r3.u32, entry = ctx.r4.u32;
  if (PadPush(ctx, base, record, entry))
    return true;
  // The push goes on as it was.
  ctx.r3.u64 = record;
  ctx.r4.u64 = entry;
  return false;
}

void VehicleDriversCamera(PPCContext &ctx, uint8_t *base) {
  std::lock_guard lock(g_mutex);
  // The camera's mode while it follows a vehicle with these parts.
  if (g_last_vehicle && ctx.r8.u32 == g_last_vehicle && g_drive.phase != Phase::Driving)
    g_vehicle_camera_mode = ctx.r6.u32;
  if (g_drive.mode != Mode::Remote || g_drive.phase != Phase::Driving || ctx.r4.u32 != g_drive.player_device)
    return;
  ctx.r4.u64 = FakeDevice();
  ctx.r8.u64 = g_drive.vehicle;
  if (g_vehicle_camera_mode)
    ctx.r6.u64 = g_vehicle_camera_mode;
  // No one-shot target over the vehicle (it would be Banjo), nor its after
  // effect: the next update cuts the camera when the target is over 30 units
  // from where it last looked, i.e. the vehicle far from Banjo.
  if (const uint32_t camera = ctx.r3.u32) {
    Store32(base, camera + kCameraOverride, 0);
    Store32(base, camera + kCameraWasOverridden, 0);
    // The look-at point: the vehicle, raised as Banjo's would be.
    std::memcpy(base + camera + kCameraLookAt, base + g_drive.vehicle + kEntityPosition, 16);
    if (const uint32_t banjo = g_drive.driver) {
      const float raise = LoadFloat(base, banjo + kEntityHeightA) * LoadFloat(base, banjo + kEntityHeightB) *
                          LoadFloat(base, banjo + kEntityHeightC) * 0.5f;
      if (std::isfinite(raise) && raise > 0.0f && raise < 10.0f)
        StoreFloat(base, camera + kCameraLookAt + 4, LoadFloat(base, camera + kCameraLookAt + 4) + raise);
    }
  }
}

}  // namespace renut::game

// The descriptor getter: its descriptor gets the slots once filled.
REX_HOOK_RAW(sub_82618A18) {
  __imp__sub_82618A18(ctx, base);
  std::lock_guard lock(g_mutex);
  Patch(base, ctx.r3.u32, g_remote_descriptor, g_remote_tick);
}

// sub_82201350(vehicle, any): the vehicle's driver's device; a vehicle the
// Remote Control drives takes reNut's.
REX_HOOK_RAW(sub_82201350) {
  {
    std::lock_guard lock(g_mutex);
    if (g_drive.mode != Mode::None && g_drive.phase == Phase::Driving && ctx.r3.u32 == g_drive.vehicle) {
      ctx.r3.u64 = FakeDevice();
      return;
    }
  }
  __imp__sub_82201350(ctx, base);
}

// sub_82200AA0(vehicle): its driver (the seated occupant's actor, + 6376
// nodes {+ 12 actor, + 16 seated}; else a linked vehicle's), or 0. Its
// callers ask only once sub_82201350 gave a device, and read the driver
// (sub_821E5420: + 3064, his camera, for camera-relative steering): a vehicle
// the Remote Control drives, empty, has Banjo for it (a crash otherwise).
REX_HOOK_RAW(sub_82200AA0) {
  const uint32_t vehicle = ctx.r3.u32;
  __imp__sub_82200AA0(ctx, base);
  if (ctx.r3.u32)
    return;
  std::lock_guard lock(g_mutex);
  if (g_drive.mode == Mode::Remote && g_drive.phase == Phase::Driving && vehicle == g_drive.vehicle &&
      g_drive.driver)
    ctx.r3.u64 = g_drive.driver;
}

// sub_8222E850(part, vehicle, double dt): a propeller's tick; for the vehicle
// the Remote Control drives, its driver's seat counts as taken meanwhile.
REX_HOOK_RAW(sub_8222E850) {
  const uint32_t vehicle = ctx.r4.u32;
  bool driven;
  {
    std::lock_guard lock(g_mutex);
    driven = g_drive.mode == Mode::Remote && g_drive.phase == Phase::Driving && vehicle == g_drive.vehicle;
  }
  const uint32_t outer = g_propeller_vehicle;
  g_propeller_vehicle = driven ? vehicle : 0;
  __imp__sub_8222E850(ctx, base);
  g_propeller_vehicle = outer;
}

// sub_8223EBF0(vehicle, type): the occupants of its seats of a type (1: any).
REX_HOOK_RAW(sub_8223EBF0) {
  const uint32_t vehicle = ctx.r3.u32, type = ctx.r4.u32;
  __imp__sub_8223EBF0(ctx, base);
  if (ctx.r3.u32 == 0 && type == kSeatDriver && vehicle && vehicle == g_propeller_vehicle)
    ctx.r3.u64 = 1;
}
