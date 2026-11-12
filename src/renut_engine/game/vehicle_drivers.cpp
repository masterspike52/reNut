#include "renut_engine/game/vehicle_drivers.h"

#include <rex/kernel/guest_memory.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "renut_engine/game/banner.h"
#include "renut_engine/game/part_classes.h"

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
// [24] (part, vehicle): the button's HUD icon state (0 greyed); the Remote
// Control takes the horn's (sub_82602458) so it shows usable.
constexpr uint32_t kSlotButtonState = 24;
constexpr uint32_t kHornButtonState = 0x82602458;

// Input devices and pad entries.
constexpr uint32_t kDeviceRecord = 16;
constexpr uint32_t kDeviceCopySize = 0x800;
constexpr uint32_t kEntrySize = 40;
constexpr uint32_t kEntryPressed = 0, kEntryReleased = 4, kEntryHeld = 8, kEntryNow = 12;
constexpr uint32_t kStart = 0x0010, kBack = 0x0020, kY = 0x8000;

// The camera (sub_822C0448): + 340 a one-shot target override, + 644 set when
// one was used (a far target then cuts the camera).
constexpr uint32_t kCameraOverride = 340, kCameraWasOverridden = 644;
// The camera's look-at point (+ 448), which sub_821CD438 sets to the avatar's
// position (+ 80) raised by half his height (+ 2236 x + 612 x + 168, x 0.5).
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
  // When the Remote Control started driving; a press during or just after
  // Banjo's exit waits for its release.
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
  rex::kernel::GuestMemory *memory = REX_GUEST_MEMORY();
  if (!memory)
    return false;
  const uint32_t block = memory
                             ->Allocate(kDeviceCopySize + 48 + 16, 0x20, rex::kernel::GuestProtect::kReadWrite,
                                        rex::kernel::GuestHeap::kSystem)
                             .value_or(0);
  if (!block)
    return false;
  g_guest = (block + 15) & ~15u;
  return true;
}

void CallGuest(PPCContext &ctx, uint8_t *base, uint32_t va) {
  if (::PPCFunc *fn = va ? renut::game::GuestFunction(va) : nullptr)
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
  if (!g_thunk_tick) {
    g_thunk_tick = renut::game::AllocateGuestThunk(SlotTick);
    g_thunk_down = renut::game::AllocateGuestThunk(SlotButtonDown);
    g_thunk_up = renut::game::AllocateGuestThunk(SlotButtonUp);
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
  // Clears the one-shot target (it would be Banjo) and its flag, which would
  // cut the camera with the vehicle far from Banjo.
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

// sub_82200AA0(vehicle): its driver, or 0; the empty vehicle the Remote
// Control drives returns Banjo (its callers read his camera, and crash on 0).
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
