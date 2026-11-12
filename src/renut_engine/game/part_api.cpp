#include "renut_engine/game/part_api.h"

#include <rex/kernel/guest_memory.h>
#include <rex/kernel/memory_view.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/call.h>
#include <rex/ppc/hook.h>
#include <rex/string/text.h>
#include <rex/types.h>
#include <rexfabric/game_api.h>
#include <rexfabric/project_hooks.h>
#include <rexfabric/rexfabric.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "renut_engine/game/field_of_view.h"
#include "renut_engine/game/flag_editor.h"
#include "renut_engine/game/keyboard_input.h"
#include "renut_engine/game/part_classes.h"
#include "renut_engine/game/custom_npcs.h"
#include "renut_engine/game/custom_worlds.h"
#include "renut_engine/app/workshop.h"
#include "renut_engine/game/actor_assets.h"
#include "renut_engine/game/effect_list.h"
#include "renut_engine/game/game_characters.h"
#include "renut_engine/game/part_scripts.h"
#include "renut_engine/gpu/render/shape_weights.h"
#include "renut_engine/mods/custom_parts.h"
#include "renut_engine/mods/mod_loader.h"

// What mods call. A float argument takes the next of f1.. and also uses up the next of r3..r10.
REX_IMPORT(__imp__sub_82256130, PushBody, void(u32, f64, u32));            // (body, dt, &force): wakes it, force through its centre
REX_IMPORT(__imp__sub_825D9C68, DamageVehicle, u32(u32, f64, u32, u32));  // (vehicle, amount, part, attacker)
REX_IMPORT(__imp__sub_825D9F80, SetVehicleHealth, u32(u32, f64));          // 0 disables it
REX_IMPORT(__imp__sub_825D9EB0, RepairVehicle, void(u32));                 // full health, not disabled
REX_IMPORT(__imp__sub_82246278, UseResource, u32(u32, u32, f64));          // (vehicle, kind, amount): whether there was enough
REX_IMPORT(__imp__sub_8254B988, VehicleDriver, u32(u32));                  // the driver actor, or 0
REX_IMPORT(sub_82201350, DriverPad, u32(u32, u32));                        // (vehicle, 0): the driver's pad (vehicle_drivers.cpp)
REX_IMPORT(__imp__sub_82626070, FireProjectile, u32(u32, u32, u32, u32));  // (weapon, &pos, &rotation, &velocity): projectile or 0
REX_IMPORT(__imp__sub_8224AA08, CopyObjparams, u32(u32));       // (&aid): a copy of the loaded objparams, or 0
REX_IMPORT(__imp__sub_821E5370, ClassIdByName, u32(u32));       // (&name): the class id, or 0
REX_IMPORT(__imp__sub_8236A570, CreateObject, u32(u32, u32, u32));  // (class id, objparams copy, 0)
REX_IMPORT(__imp__rex_meInternalFree, FreeObject, void(u32));      // sub_82332C30 (config/renut_funcs.toml)
REX_IMPORT(__imp__sub_82411220, ReleaseProjectile, void(u32));  // it goes once it has landed
REX_IMPORT(__imp__sub_8267EE30, NetObjectRemote, u32(u32));     // (net object): u8
REX_IMPORT(__imp__sub_82210708, ModelBoneCount, u32(u32));      // (model instance)
REX_IMPORT(__imp__sub_821D3648, DamagePart, u32(u32, f64));                // (part, amount): whether it should break off
REX_IMPORT(__imp__sub_82616198, BreakOffPart, u32(u32, u32, u32));         // (part, 1, 0): the vehicle splits next tick
REX_IMPORT(__imp__sub_8260FC20, PressPart, void(u32, u32, u32));           // (part, vehicle, control): its button down
REX_IMPORT(__imp__sub_8260FD50, ReleasePart, void(u32, u32, u32));         // ... and up
REX_IMPORT(__imp__sub_82960310, CastRay, void(u32, u32, u32));             // hkpWorld::castRay(world, &input, &output)
REX_IMPORT(__imp__sub_821D23E0, MoveActor, void(u32, u32, u32, u32, u32));  // (actor, &pos, &rotation or 0, 0, 0): msg 46
REX_IMPORT(__imp__sub_82265BD0, PlayAnimation, void(u32, u32, u32));        // (entity, &name, suffix or 0)
REX_IMPORT(__imp__sub_8260AE08, PaintPart, void(u32, u32, u32));            // (part, &rgba, effect): redrawn next frame
REX_IMPORT(__imp__rex_gameAssetRefGetAid, AssetByName, u32(u32, u32, u32));   // (&name, &aid, 0): the aid of a named asset
REX_IMPORT(__imp__rex_assetManOpenParams, LoadAsset, u32(u32, u32));             // (&aid, 1): its data, or 0
REX_IMPORT(__imp__sub_8228F0F0, AssetBytes, u32(u32));                       // (data): its size
REX_IMPORT(__imp__sub_82204DF0, ReleaseAsset, void(u32));                    // (data): LoadAsset's undone
REX_IMPORT(__imp__sub_82554410, AwardCrate, void(u32, u32, u32));            // (HUD list, flag, show contents)
REX_IMPORT(__imp__sub_82384340, RemoveObject, void(u32, u32));               // (object, 0): it (and what it holds) goes
REX_IMPORT(__imp__sub_82224670, SetRotation, void(u32, u32, u32));           // (entity, &euler radians[3], 1)
REX_IMPORT(__imp__sub_82281EC8, SetStrategyState, void(u32, u32));           // (strategy, state id): exits and enters
REX_IMPORT(__imp__sub_821B2CA8, FindMindAction, u32(u32, u32));              // (mind, &name): its action's &aid, or 0
REX_IMPORT(__imp__sub_8225B268, PlayMindAction, void(u32, u32));             // (mind, &aid)
REX_IMPORT(__imp__sub_8227E078, ClearGoals, void(u32, u32));                 // (mind, 0)
REX_IMPORT(__imp__sub_823A3EE0, WalkToPoint, void(u32, u32));                // (mind, &vec4): goal 88, default speeds
REX_IMPORT(__imp__sub_822495A8, SetBodyState, u32(u32, u32));                // (body, state id): 0 when it has none
REX_IMPORT(__imp__sub_821B4678, DialogPlaying, u32(u32));                    // (dialog manager)
REX_IMPORT(__imp__sub_8228BAC8, LoadBundle, void(u32, u32, u32));            // (owner world, &stream bundle aid, wait)
// Awards and pickups (renut.game.award, renut.objects.on("collected")).
REX_IMPORT(__imp__sub_824AC1C0, AddCounter, void(u32, u32, u32, u32));       // (profile, counter, amount, quiet)
REX_EXTERN(__imp__sub_824400C0);  // (collectable, world): an entityPropsCollectable's tick; + 3008 set as collected
// Challenges from code (renut.game.challenge_*): the challengeMan's state, a stage, a player's slot.
REX_IMPORT(__imp__sub_824FEDD8, SetChallengeState, void(u32, u32));          // (challengeMan, state)
REX_IMPORT(__imp__sub_824FDC38, BeginChallenge, void(u32, u32, u32));        // (challengeMan, &aid, 0): starts it
REX_IMPORT(__imp__sub_825015F0, ChallengeStage, u32(u32, u32));              // (challengeMan, stage id): it, or 0
REX_IMPORT(__imp__sub_82285DC8, PlayerIndex, u32(u32));                      // (profile): its player slot
// Going to another world, as a door does once its sequence ends (sub_8221B718); the scene id is copied at once.
REX_IMPORT(__imp__rex_supportGameLoadNewScene, GoToScene, void(u32, u32, u32, u32));  // sub_82548110 (&scene aid, running world, 1, &placement or 0)
// Whole vehicles from a blueprint (renut.vehicle.spawn), as the garage builds one (sub_824EB100), and seats.
REX_IMPORT(__imp__sub_8254CAD0, InitVehicleBuilder, void(u32, u32, u32, u32));  // (builder, world, &pos, &quat)
REX_IMPORT(__imp__sub_8254CEF0, BuildVehicle, u32(u32));                     // (builder): the vehicle, or 0
// A built vehicle starts inactive (+0x7D4 at 1); the game's spawns switch it on with these (sub_82402910).
REX_IMPORT(__imp__sub_823A5D68, EnableEntity, u32(u32, u32));               // (entity, 1): +0x7D4 down, state cleared
REX_IMPORT(__imp__sub_823A5D18, ShowEntity, u32(u32));                      // (entity)
REX_IMPORT(__imp__sub_825F6540, SeatActor, void(u32, u32, u32, u32, u32));   // (vehicle, actor, seat type, part, 1)
// Characters' health, defeat, targets and models (renut.actors).
REX_IMPORT(__imp__sub_82279920, ReviveBody, void(u32, f64));                // (body, health): current and max, alive
REX_IMPORT(__imp__sub_82279988, DamageBody, void(u32, f64));                // (body, amount): 0 at least; dead at 0
REX_IMPORT(__imp__sub_823A3140, DefeatActor, void(u32));                    // (actor): dies; removed unless a player
REX_IMPORT(__imp__sub_8227E238, SetMindTarget, void(u32, u32, u32));        // (mind, target record or 0, 0)
REX_IMPORT(__imp__sub_822B0BC8, SetEntityModel, u32(u32, u32, u32));        // (entity, &model aid, world)
REX_EXTERN(__imp__sub_823CD070);  // (body, hit, out): a hit; out + 4 set when taken, out + 0 its damage
REX_EXTERN(__imp__sub_823A31B8);  // (actor): it dies (every way there is)
// The HUD (renut.hud): its message box, the score/time panel of a challenge.
REX_IMPORT(__imp__sub_8255A948, ShowMessageBox, void(u32, u32, u32));       // (profile, const u16 *text, 0): no buttons
REX_IMPORT(__imp__sub_8255AAE0, ShowChoiceBox, void(u32, u32, u32, u32, u32, u32, u32, u32));  // (profile, text, A key, cb, arg, B key, cb, arg)
REX_IMPORT(__imp__sub_8255AD38, CloseMessageBox, void(u32, u32));           // (profile, 1): at once
REX_IMPORT(__imp__sub_825CA310, BoxButton, u32(u32, u32, u32, u32));        // (box, pressed element, -, -)
REX_IMPORT(__imp__sub_822B0BC8, MakeEntityModel, u32(u32, u32, u32));      // (entity, &model aid, world or 0)
REX_IMPORT(__imp__sub_82228A88, HudPlayer, u32(u32, u32, u32));             // (hud, last, profile): its HUD container
REX_IMPORT(__imp__sub_8221A350, ProfileIndex, u32(u32));                    // (profile)
REX_IMPORT(__imp__sub_825B2858, OpenScorePanel, void(u32, u32, u32));       // (container, profile, &params[13])
REX_IMPORT(__imp__sub_8225E4E8, ScorePanel, u32(u32, u32));                 // (hud, profile): its scene, or 0
REX_IMPORT(__imp__sub_825AF8B0, SetPanelTime, void(u32, f64));              // (scene, seconds)
REX_IMPORT(__imp__sub_825AF970, SetPanelScore, void(u32, u32));             // (scene, value)
REX_IMPORT(__imp__sub_82A25780, SetElementText, void(u32, u32));            // (element, const u16 *text)
REX_IMPORT(__imp__sub_82553E88, ClosePanels, void(u32));                    // (profile): the score/time panel goes
REX_IMPORT(__imp__rex_assetDbBundleLoadBundle, LoadDataBundle, void(u32, u32, u32, u32, u32));  // (owner world, &bundle aid, 1, 0, 0)
// The game's object messages (sub_822107C8 clears one to its table size, then entity vtable + 12 handles it).
REX_IMPORT(__imp__sub_822107C8, InitMessage, void(u32, u32));           // (message, id)
// Saving, and the game's sound cues (stereo.cpp plays its songs the same way).
REX_IMPORT(__imp__rex_gameSaveSave, SaveGame, u32(u32));                // sub_824E8838 (profile): whether a save began
REX_IMPORT(__imp__rex_audioMainPlayHash, PlayCue, u32(u32, u32, u32, f64));  // (&cue id, -, workspace 0, volume): its handle
REX_IMPORT(__imp__sub_8239F790, StopCue, void(u32, u32, u32));           // (cue record, 1, 0)
REX_IMPORT(__imp__sub_82211450, CuePlaying, u32(u32));                   // (cue record): u8
REXFABRIC_HOOKABLE(rex_gameSaveSave);
REX_EXTERN(__imp__sub_8260FC20);  // (part, vehicle, control): its button down
REX_EXTERN(__imp__sub_8260FD50);  // ... and up
REX_EXTERN(__imp__sub_82384340);  // (object, 0): it's removed
// The game's physical memory (sub_826F1FB8, sub_826F2048): to see whether another world's bundle still fits.
REX_IMPORT(__imp__rex_XPhysicalAlloc, PhysicalAlloc, u32(u32, u32, u32, u32));  // (size, address or -1, alignment, protect): 0 when full
REX_IMPORT(__imp__rex_XPhysicalFree, PhysicalFree, void(u32));
constexpr uint32_t kPageReadWrite = 4;
// The originals of the functions hooked for events (end of the file).
REX_EXTERN(__imp__sub_825DBA20);
REX_EXTERN(__imp__sub_825EF480);
REX_EXTERN(__imp__sub_821ECA98);
REX_EXTERN(__imp__sub_825ED3E0);
REX_EXTERN(__imp__sub_825EDDA0);
REX_EXTERN(__imp__sub_82216C78);
REX_EXTERN(__imp__sub_82289908);  // a world becomes the running one
REX_EXTERN(__imp__sub_82289C30);  // (stack entry): a world is torn down
REX_EXTERN(__imp__sub_823D0AF0);  // avatarSetVehicle (avatar, vehicle or 0, seat part)
// Mods' rex.hook on these run around the game's own (rexfabric/project_hooks.h).
REXFABRIC_HOOKABLE(sub_824FDC38);  // (challengeMan, &aid, flag): a challenge begins
REXFABRIC_HOOKABLE(sub_82503568);  // (challengeMan): settles a finished challenge's outcome
REXFABRIC_HOOKABLE(sub_8236A570);  // (class, objparams copy, net): makes an object
REX_EXTERN(__imp__sub_8255B428);  // opens the pause menu
REX_EXTERN(__imp__sub_8255B598);  // closes it
REX_EXTERN(__imp__rex_meCalculateView);  // builds the view from the current camera node
REX_EXTERN(__imp__sub_82554288);  // (HUD list, record, show, ..., add): what a crate's award adds and shows
REX_EXTERN(__imp__sub_824FA620);  // (&blockset aid, ...): adds a blockset's parts to the garage inventory

namespace renut::game::part_api {
namespace {

using renut::mods::CustomPart;
using Error = std::runtime_error;

// Guest data. A part: its world matrix (translation unconfirmed), grid cell (x 11 bits,
// y 10, z 11, signed), orientation (0-23), health, objparams aid, vehicle.
constexpr uint32_t kPartWorldPosition = 0xC0;
constexpr uint32_t kPartCell = 0xEC;
constexpr uint32_t kPartOrientation = 0xF0;
constexpr uint32_t kPartMaxHealth = 0x26C;
constexpr uint32_t kPartHealth = 0x270;
constexpr uint32_t kPartHealthFraction = 0x274;
constexpr uint32_t kPartHealthChanged = 0x280;
constexpr uint32_t kPartDamageLevel = 0x284;
constexpr uint32_t kPartAid = 0x3DC;
constexpr uint32_t kPartVehicle = 0x46C;

// A vehicle: its level, health, resources (fuel, ammo, a third: current, max, infinite
// flag, a word each), flags, name (UTF-16, 64), parts (176-byte entries, the part at + 4).
constexpr uint32_t kVehicleLevel = 76;
constexpr uint32_t kVehicleBody = 0x7C0;
constexpr uint32_t kVehicleMaxHealth = 0xD0C;
constexpr uint32_t kVehicleHealth = 0xD10;
constexpr uint32_t kVehicleHealthFraction = 0xD18;
constexpr uint32_t kVehicleDamageLevel = 0xD20;
constexpr uint32_t kVehicleResourceMax = 0xEA0;
constexpr uint32_t kVehicleResource = 0xEAC;
constexpr uint32_t kVehicleResourceInfinite = 0xEC4;
constexpr uint32_t kVehicleDisabled = 0x1070;  // i32: > 0 is disabled
constexpr uint32_t kVehicleFlags = 0x18E0;
constexpr uint32_t kVehicleInvulnerable = 0x10;
constexpr uint32_t kVehicleName = 0x1830;
constexpr uint32_t kVehicleNameLength = 64;
constexpr uint32_t kVehicleParts = 0x1498;  // begin, end
// The contact pass (sub_8223ECC8) sends a frame's strongest hit as objMsgId_Banjo_Actor_VehicleImpact via sub_82216C78,
// which returns here with the hit's contact record in r30: its strength (f32), what it hit, the part it hit.
constexpr uint32_t kImpactReturn = 0x8223F074;
constexpr uint32_t kContactStrength = 0xA0, kContactOther = 0x94, kContactPart = 0xAC;
constexpr uint32_t kVehiclePartEntry = 176;
constexpr uint32_t kVehiclePartEntryPart = 4;

// The body's motion state (position; rotation before it, centres, inverse mass,
// velocities after), and its hkpMotion (body + 0xD0) with its virtual functions.
constexpr uint32_t kBodyMotion = 0x110;
constexpr int32_t kMotionRotation = -0x30;  // columns: right, up, forward
constexpr uint32_t kMotionCentres[] = {0x10, 0x20};
constexpr uint32_t kMotionInverseMass = 0x8C;
constexpr uint32_t kMotionVelocity = 0x90;
constexpr uint32_t kMotionAngularVelocity = 0xA0;
constexpr uint32_t kBodyHkMotion = 0xD0;
constexpr uint32_t kBodyWorld = 8;  // hkpEntity: the hkpWorld it's in
// hkpEntity: its collidable's (+0x10) collision filter info (+0x1C), as hkpGroupFilter (sub_8293B2F8) reads it.
constexpr uint32_t kBodyFilterInfo = 0x2C;
// hkpWorldRayCastInput {from, to, bool shape filter (+ 32), u32 filter info} and its output
// {normal, hit fraction (+ 16; starts at 1), ...}.
constexpr uint32_t kRayInputSize = 0x30, kRayOutputSize = 0x60, kRayOutputFraction = 0x10;
constexpr uint32_t kRayInputFilterInfo = 36;
// renut.vehicle._in_air: how far below the lowest parts it looks by default, and how tall that lowest layer is.
constexpr double kInAirReach = 1.5, kLowestLayer = 1.0;
constexpr uint32_t kActivateBody = 0x8295DAE0;  // hkpEntity::activate(body)
constexpr uint32_t kPointImpulse = 0x4C;        // (motion, &impulse, &point)
constexpr uint32_t kAngularImpulse = 0x50;      // (motion, &impulse)
constexpr uint32_t kForceAtPoint = 0x54;        // (motion, f1 dt, -, &force, &point)

// Driver input. The vehicle's drive units (count, then the array) each hold the input
// sub_82241E00 gave them this frame; without any, the driver's pad.
constexpr uint32_t kDriveUnitCount = 0xAC0;
constexpr uint32_t kDriveUnits = 0xAC4;
constexpr uint32_t kDriveUnitInput = 0x9C;         // -> steer, stick y, brake, accelerate (f32), braking (u8)
constexpr uint32_t kPadFrameIndex = 16;            // pad + 16: the current frame, 40 bytes each after it
constexpr uint32_t kVehicleStickFlipped = 0x1074;  // sub_825DC418 negates the stick when set

// Weapons (vehicleBlockWeapon): their projectiles are made on their first tick.
constexpr const char kWeaponClass[] = "vehicleBlockWeapon";
constexpr uint32_t kWeaponHeld = 0x48C;             // u32: its button is held (it fires)
constexpr uint32_t kWeaponProjectilesMade = 0x6FC;  // u32

constexpr uint32_t kMaxRecordSize = 0x10000;

// The camera's world matrix (stereo.cpp reads it too): rows right, up, forward, position.
constexpr uint32_t kCameraMatrix = 0x82E1B4D0;
constexpr uint32_t kCameraFieldOfView = 0x82E1B6B4;  // f32: vertical, degrees (sub_821E4540 writes it)

// A part's world matrix (rows right, up, forward; unconfirmed).
constexpr uint32_t kPartWorldMatrix = 0x90;
// A weapon's turret (sub_821D0B30): pitch (radians, between two limits in degrees) and yaw
// (radians, wrapping), and their copies its model is drawn with.
constexpr uint32_t kWeaponPitch = 0x668, kWeaponYaw = 0x670;
constexpr uint32_t kWeaponPitchLimits = 0x690;  // f32 degrees, two
constexpr uint32_t kWeaponAmmoPerShot = 0x698;  // f32: what each shot of its own takes (sub_82625FD0)
constexpr uint32_t kWeaponDrawnYaw = 0x77C, kWeaponDrawnPitch = 0x784, kWeaponDrawnPitchDown = 0x78C,
                   kWeaponDrawnPitchUp = 0x794;
// Its barrel's end in the world, from the model's muzzle locators each tick (sub_82627788), where
// its own shots start (sub_826266E8); set once 0x854 is.
constexpr uint32_t kWeaponMuzzle = 0x7E0, kWeaponMuzzleDirection = 0x7F0, kWeaponMuzzleSet = 0x854;
// Its projectiles (sub_82627368 makes them): a pool of up to 30, which sub_82626070 fires from.
constexpr uint32_t kWeaponPool = 0x700, kPoolSize = 30;

// A projectile's objparams copy, as a weapon readies it (sub_82627368): made, local, level,
// a start, and its owners (sub_8240FC00: a part puts it in that part's weapon pool).
constexpr uint32_t kProjectileMade = 0x8C, kProjectileLocal = 0x90, kProjectileLevel = 0x84,
                   kProjectileStart = 0x98, kProjectileClassName = 0x4B, kProjectileOwnerVehicle = 0x240,
                   kProjectileOwnerPart = 0x244;
constexpr uint32_t kObjectNet = 0x20, kObjectRemote = 0x24;  // an object's net object, or this flag
constexpr uint32_t kVehiclePosition = 0x50;
constexpr uint8_t kTypeObjparams = 0x1F;

// A part's model instance; its render graph for the level's current view is instance[3 + level[80]],
// whose bones sub_82211B58 (instance, graph, bone, f1 angle, v1 axis) turns about their joint, as a
// turret's barrel or a wheel's tyre turn (sub_82211AD0).
constexpr uint32_t kPartModel = 0x380;
constexpr uint32_t kPartModelAid = 0x3A0;  // its model's asset id: its shape keys' table
constexpr uint32_t kLevelView = 80;
constexpr uint32_t kSetBoneRotation = 0x82211B58;

// renut.vehicle._get / _set: what is read or written.
enum Quantity : int64_t { kPosition, kVelocity, kAngularVelocity, kRight, kUp, kForward };
// renut.vehicle._input: which input.
enum Input : int64_t { kSteer, kStickY, kBrake, kAccelerate, kBraking };

// The renut events running on this thread, and the guest context the innermost lends:
// functions calling the game work inside one (renut.fx.play and Havok's virtuals use the context).
thread_local int t_events = 0;
thread_local PPCContext *t_ctx = nullptr;
thread_local uint8_t *t_base = nullptr;
// The amount a damage event's listeners may change (renut.set_amount).
thread_local double *t_amount = nullptr;

// renut.parts.add's parts, between _define and _commit (under RexFabric's Lua lock).
std::map<std::string, CustomPart> g_pending;

struct Events {
  rexfabric::Event part_spawn, part_destroy, part_attach, part_detach, part_tick, part_physics, part_power,
      part_button, part_damage, part_break, vehicle_spawn, vehicle_removed, vehicle_tick, vehicle_damage,
      vehicle_health, vehicle_part_added, vehicle_part_removed, vehicle_collide, vehicle_enter, vehicle_exit,
      world_enter, world_leave, challenge_start, challenge_finish, game_paused, game_resumed, crate_award, game_tick,
      zone_enter, zone_exit, hud_choice, hud_timer_done, actor_hit, actor_defeated, collected, player_state, game_save,
      game_load, part_use, object_spawn, object_removed;
};
std::optional<Events> g_events;

uint8_t *Base() {
  return REX_GUEST_MEMORY()->GetView().TranslateVirtual<uint8_t *>(0);
}

// System heap memory, zeroed; 0 when there's none.
uint32_t SystemAlloc(uint32_t size, uint32_t alignment = 0x20) {
  rex::kernel::GuestMemory *memory = REX_GUEST_MEMORY();
  if (!memory)
    return 0;
  return memory->Allocate(size, alignment, rex::kernel::GuestProtect::kReadWrite, rex::kernel::GuestHeap::kSystem)
      .value_or(0);
}

void SystemFree(uint32_t address) {
  if (rex::kernel::GuestMemory *memory = REX_GUEST_MEMORY(); memory && address)
    (void)memory->Free(address);
}

bool Heap(uint32_t va) {
  return va >= 0x40000000 && va < 0xC0000000;
}

uint32_t Load32(uint32_t va) {
  uint32_t v;
  std::memcpy(&v, Base() + va, 4);
  return rex::byte_swap(v);
}

void Store32(uint32_t va, uint32_t value) {
  value = rex::byte_swap(value);
  std::memcpy(Base() + va, &value, 4);
}

float LoadFloat(uint32_t va) {
  const uint32_t bits = Load32(va);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

void StoreFloat(uint32_t va, double value) {
  const float f = float(value);
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  Store32(va, bits);
}

uint32_t Address(int64_t value, const char *what) {
  if (value <= 0 || value > 0xFFFFFFFFll || !Heap(uint32_t(value)))
    throw Error(std::string("not a ") + what);
  return uint32_t(value);
}

void RequireGame() {
  if (!t_events)
    throw Error("this works inside the renut events (a part's or a vehicle's callbacks)");
}

// The vehicle's Havok body, or an error when it has none now.
uint32_t Body(int64_t vehicle) {
  const uint32_t body = Load32(Address(vehicle, "vehicle") + kVehicleBody);
  if (!Heap(body))
    throw Error("the vehicle has no body right now (it is being rebuilt)");
  return body;
}

uint32_t Motion(int64_t vehicle) {
  return Body(vehicle) + kBodyMotion;
}

int32_t QuantityOffset(int64_t what) {
  switch (what) {
  case kPosition: return 0;
  case kVelocity: return kMotionVelocity;
  case kAngularVelocity: return kMotionAngularVelocity;
  case kRight: return kMotionRotation;
  case kUp: return kMotionRotation + 0x10;
  case kForward: return kMotionRotation + 0x20;
  }
  throw Error("unknown vehicle quantity");
}

uint32_t ResourceKind(int64_t kind) {
  if (kind < 0 || kind > 2)
    throw Error("a resource is 0 (fuel), 1 (ammo) or 2");
  return uint32_t(kind);
}

// A weapon part, or an error for any other.
uint32_t Weapon(int64_t instance) {
  const uint32_t part = Address(instance, "part");
  if (!renut::game::InstanceIsA(Base(), part, kWeaponClass))
    throw Error("the part isn't a weapon (its base has to be one)");
  return part;
}

// Three 16-byte-aligned guest vec4s, for game calls (game thread only).
uint32_t Scratch() {
  static uint32_t scratch = 0;
  if (!scratch && !(scratch = SystemAlloc(48, 16)))
    throw Error("no guest memory");
  return scratch;
}

// A ray cast's input and output, 16-byte aligned (game thread only).
uint32_t RayScratch() {
  static uint32_t scratch = 0;
  if (!scratch && !(scratch = SystemAlloc(kRayInputSize + kRayOutputSize, 16)))
    throw Error("no guest memory");
  return scratch;
}

void StoreVec4(uint32_t va, double x, double y, double z, double w) {
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w))
    throw Error("not a finite vector");
  StoreFloat(va, x);
  StoreFloat(va + 4, y);
  StoreFloat(va + 8, z);
  StoreFloat(va + 12, w);
}

// How far along from (fx, fy, fz) to (tx, ty, tz) the first thing in `world` is (1: nothing), as an object with
// collision filter info `filter` sees it: 0 sees everything. Game thread, in a renut event.
double RayFraction(uint32_t world, double fx, double fy, double fz, double tx, double ty, double tz, uint32_t filter) {
  const uint32_t input = RayScratch(), output = input + kRayInputSize;
  StoreVec4(input, fx, fy, fz, 0);
  StoreVec4(input + 16, tx, ty, tz, 0);
  for (uint32_t at = 32; at < kRayInputSize + kRayOutputSize; at += 4)
    Store32(input + at, 0);
  Store32(input + kRayInputFilterInfo, filter);
  StoreFloat(output + kRayOutputFraction, 1.0);
  CastRay(world, input, output);
  return LoadFloat(output + kRayOutputFraction);
}

// --- Game state: worlds, challenges, the player (renut.game, renut.player) --------------------------

constexpr uint32_t kRunningWorld = 0x82E19AB4;  // the running world (level), or 0: sub_82289908 sets it
constexpr uint32_t kWorldScene = 0, kWorldNumber = 88;  // its scene's asset id; its number, from 1
constexpr uint32_t kWorldSceneObject = 5552, kSceneChallengeMan = 7008;  // *(world + 5552) + 7008: the challengeMan
constexpr uint32_t kWorldAvatars = 2628;  // world: each player's avatar, 8 bytes apart, by profile + 128
constexpr uint32_t kWorldTable = 0x82101914, kWorldEntry = 20, kWorldNumbers = 8;  // {name, ...} by number - 1
constexpr uint32_t kChallengeRunning = 12, kChallengeState = 20, kChallengeOutcome = 1444;  // player 1's: 1 won, 2 lost
constexpr uint32_t kChallengeSettling = 14, kChallengeSettled = 15;  // sub_82503568 moves 14 to 15 once
constexpr uint32_t kPauseMenu = 0x82D8BED0;  // the pause menu's handle, -1 while not paused (sub_8255B428)
constexpr uint32_t kPlayerProfile = 0x82E1B280, kProfilePlayer = 128;
constexpr uint32_t kAvatarPosition = 0x50, kAvatarVehicle = 0xC3C;
constexpr uint8_t kTypeChallenge = 0x3D;

// A NUL-terminated guest string, at most `max` bytes.
std::string CString(uint32_t va, size_t max = 256) {
  std::string text;
  if (!Heap(va) && (va < 0x82000000 || va >= 0x83000000))
    return text;
  for (size_t i = 0; i < max; ++i) {
    const char c = char(Base()[va + i]);
    if (!c)
      break;
    text += c;
  }
  return text;
}

uint32_t RunningWorld() {
  const uint32_t world = Load32(kRunningWorld);
  return Heap(world) ? world : 0;
}

// "nuttyacres" by world number (1-8), or "".
std::string WorldName(uint32_t number) {
  if (number < 1 || number > kWorldNumbers)
    return {};
  return CString(Load32(kWorldTable + kWorldEntry * (number - 1)), 64);
}

uint32_t ChallengeMan() {
  const uint32_t world = RunningWorld();
  const uint32_t scene = world ? Load32(world + kWorldSceneObject) : 0;
  return Heap(scene) ? scene + kSceneChallengeMan : 0;
}

// Player 1's avatar in the running world (Banjo), or 0.
uint32_t PlayerAvatar() {
  const uint32_t world = RunningWorld(), profile = Load32(kPlayerProfile);
  if (!world || !Heap(profile))
    return 0;
  const uint32_t player = Load32(profile + kProfilePlayer);
  if (player >= 4)
    return 0;
  const uint32_t avatar = Load32(world + kWorldAvatars + 8 * player);
  return Heap(avatar) ? avatar : 0;
}

// A guest buffer for a name the game reads (game thread only).
uint32_t TextScratch(const std::string &text) {
  static uint32_t scratch = 0;
  constexpr uint32_t kSize = 256;
  if (!scratch && !(scratch = SystemAlloc(kSize, 16)))
    throw Error("no guest memory");
  if (text.size() >= kSize)
    throw Error("a name is under 256 characters");
  std::memcpy(Base() + scratch, text.c_str(), text.size() + 1);
  return scratch;
}

// A flag or counter by name: "gameFlag_Normal_...", "gameFlag_Global_...", "gameFlag_Volatile_...",
// "gameCounter_BanjoX_...", or a counter's short name ("JiggiesBanked").
std::pair<renut::game::flags::Table, uint32_t> FlagByName(const std::string &name) {
  using renut::game::flags::Table;
  const auto find = [&name](Table table, const std::string &full) -> std::optional<uint32_t> {
    const std::vector<std::string> &names = renut::game::flags::Names(table);
    for (uint32_t i = 1; i < names.size(); ++i) {
      if (names[i] == full)
        return i;
    }
    return std::nullopt;
  };
  for (const Table table : {Table::kNormal, Table::kGlobal, Table::kVolatile, Table::kCounters}) {
    if (const auto index = find(table, name))
      return {table, *index};
  }
  if (const auto index = find(Table::kCounters, "gameCounter_BanjoX_" + name))
    return {Table::kCounters, *index};
  throw Error("no game flag or counter '" + name + "'");
}

constexpr uint32_t kPartPaint = 0x388;             // u32 0xRRGGBBAA; sub_8260AE08 also sets the float4 at +0x390
constexpr uint32_t kMotionInverseInertia = 0x80;  // hkpMotion: inverse inertia (x, y, z), then inverse mass

// --- The camera (renut.camera.set) -----------------------------------------------------------------------------

// sub_822BB8F0 builds the view (kCameraMatrix: its world matrix, rows right, up, forward or back, position) from the
// current camera node: position at +16, orientation quaternion (x, y, z, w) at +64, used unless +168 bit 0 is set.
constexpr uint32_t kCurrentCamera = 0x82E1B5F0, kViewportCamera = 0x82E182A8;
constexpr uint32_t kNodePosition = 16, kNodeRotation = 64, kNodeFlags = 168;

struct CameraOverride {
  bool active = false;
  float at[3] = {};
  float look[3] = {};
};
std::mutex g_camera_mutex;
CameraOverride g_camera;

// renut.camera.shake (under g_camera_mutex): how far the camera is thrown about (units), easing out until `until`.
struct CameraShake {
  double strength = 0, seconds = 0;
  std::chrono::steady_clock::time_point until{};
};
CameraShake g_shake;

// This frame's shake offset, game thread (the view hook), under g_camera_mutex.
std::array<float, 3> ShakeOffset() {
  const auto now = std::chrono::steady_clock::now();
  if (g_shake.strength <= 0 || now >= g_shake.until || g_shake.seconds <= 0)
    return {0, 0, 0};
  const double left = std::chrono::duration<double>(g_shake.until - now).count() / g_shake.seconds;
  static uint32_t seed = 0x2545F491u;
  std::array<float, 3> offset;
  for (float &axis : offset) {
    seed = seed * 1664525u + 1013904223u;
    axis = float(g_shake.strength * left * (double(seed >> 8) / double(1u << 24) * 2.0 - 1.0));
  }
  return offset;
}

// How the node's quaternion makes the world matrix, read off the game's own frames: which of the rotation's columns
// (or rows) each matrix row is, its sign, which way the third row faces, and the rows' handedness.
struct CameraFrame {
  int axis[3] = {0, 1, 2};
  float sign[3] = {1, 1, 1};
  bool transposed = false;
  float facing = 1;  // +1: the third row is where the camera looks
  float handed = 1;
};
CameraFrame g_camera_frame;
std::atomic<bool> g_camera_known{false};

struct V3 {
  float x = 0, y = 0, z = 0;
};
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Scale(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V3 Normalized(V3 a) {
  const float length = std::sqrt(Dot(a, a));
  return length > 1e-6f ? Scale(a, 1.0f / length) : V3{};
}
V3 LoadV3(uint32_t va) { return {float(LoadFloat(va)), float(LoadFloat(va + 4)), float(LoadFloat(va + 8))}; }

// The rotation matrix's columns of quaternion (x, y, z, w).
std::array<V3, 3> QuaternionColumns(float x, float y, float z, float w) {
  return {V3{1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)},
          V3{2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)},
          V3{2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)}};
}

// After the game built a frame of its own: how the node's quaternion became the matrix, and which way it faces.
void LearnCameraFrame(uint32_t node) {
  if (Load32(node + kNodeFlags) & 1)
    return;
  const float qx = float(LoadFloat(node + kNodeRotation)), qy = float(LoadFloat(node + kNodeRotation + 4));
  const float qz = float(LoadFloat(node + kNodeRotation + 8)), qw = float(LoadFloat(node + kNodeRotation + 12));
  const std::array<V3, 3> columns = QuaternionColumns(qx, qy, qz, qw);
  const std::array<V3, 3> rows = {Normalized(LoadV3(kCameraMatrix)), Normalized(LoadV3(kCameraMatrix + 16)),
                                  Normalized(LoadV3(kCameraMatrix + 32))};
  for (const bool transposed : {false, true}) {
    std::array<V3, 3> basis = columns;
    if (transposed)
      basis = {V3{columns[0].x, columns[1].x, columns[2].x}, V3{columns[0].y, columns[1].y, columns[2].y},
               V3{columns[0].z, columns[1].z, columns[2].z}};
    CameraFrame frame;
    frame.transposed = transposed;
    bool fits = true, used[3] = {};
    for (int i = 0; i < 3 && fits; ++i) {
      int best = -1;
      float best_dot = 0;
      for (int j = 0; j < 3; ++j) {
        const float d = Dot(rows[i], Normalized(basis[j]));
        if (std::abs(d) > std::abs(best_dot)) {
          best_dot = d;
          best = j;
        }
      }
      fits = best >= 0 && !used[best] && std::abs(best_dot) > 0.97f;
      if (fits) {
        used[best] = true;
        frame.axis[i] = best;
        frame.sign[i] = best_dot < 0 ? -1.0f : 1.0f;
      }
    }
    if (!fits)
      continue;
    frame.handed = Dot(Cross(rows[0], rows[1]), rows[2]) < 0 ? -1.0f : 1.0f;
    // The gameplay camera looks toward the player, which settles which way the third row faces.
    const uint32_t avatar = PlayerAvatar();
    if (!avatar)
      return;
    const uint32_t vehicle = Load32(avatar + kAvatarVehicle);
    const uint32_t body = Heap(vehicle) ? Load32(vehicle + kVehicleBody) : 0;
    const V3 player = Heap(body) ? LoadV3(body + kBodyMotion) : LoadV3(avatar + kAvatarPosition);
    const V3 camera = LoadV3(kCameraMatrix + 48);
    const V3 toward{player.x - camera.x, player.y - camera.y, player.z - camera.z};
    if (Dot(toward, toward) < 1.0f)
      return;
    frame.facing = Dot(rows[2], toward) < 0 ? -1.0f : 1.0f;
    g_camera_frame = frame;
    g_camera_known.store(true, std::memory_order_release);
    return;
  }
}

std::array<float, 4> MatrixQuaternion(const float m[3][3]);

// The node quaternion (x, y, z, w) that makes the camera at `at` look at `look`, by the learned frame.
std::array<float, 4> CameraQuaternion(const CameraFrame &frame, V3 at, V3 look) {
  const V3 forward = Normalized({look.x - at.x, look.y - at.y, look.z - at.z});
  V3 up{0, 1, 0};
  if (std::abs(Dot(forward, up)) > 0.999f)
    up = {0, 0, 1};
  std::array<V3, 3> rows;
  rows[2] = Scale(forward, frame.facing);
  rows[0] = Normalized(Cross(up, rows[2]));
  rows[1] = Cross(rows[2], rows[0]);
  if (frame.handed < 0)
    rows[0] = Scale(rows[0], -1);
  // Back to the rotation's columns, then the matrix as m[row][column].
  std::array<V3, 3> basis;
  for (int i = 0; i < 3; ++i)
    basis[frame.axis[i]] = Scale(rows[i], frame.sign[i]);
  float m[3][3];
  for (int j = 0; j < 3; ++j) {
    const V3 v = basis[j];
    if (frame.transposed) {
      m[j][0] = v.x, m[j][1] = v.y, m[j][2] = v.z;
    } else {
      m[0][j] = v.x, m[1][j] = v.y, m[2][j] = v.z;
    }
  }
  return MatrixQuaternion(m);
}

// The quaternion (x, y, z, w) of rotation matrix m[row][column].
std::array<float, 4> MatrixQuaternion(const float m[3][3]) {
  float x, y, z, w;
  const float trace = m[0][0] + m[1][1] + m[2][2];
  if (trace > 0) {
    const float s = std::sqrt(trace + 1.0f) * 2;
    w = 0.25f * s, x = (m[2][1] - m[1][2]) / s, y = (m[0][2] - m[2][0]) / s, z = (m[1][0] - m[0][1]) / s;
  } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
    const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2;
    w = (m[2][1] - m[1][2]) / s, x = 0.25f * s, y = (m[0][1] + m[1][0]) / s, z = (m[0][2] + m[2][0]) / s;
  } else if (m[1][1] > m[2][2]) {
    const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2;
    w = (m[0][2] - m[2][0]) / s, x = (m[0][1] + m[1][0]) / s, y = 0.25f * s, z = (m[1][2] + m[2][1]) / s;
  } else {
    const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2;
    w = (m[1][0] - m[0][1]) / s, x = (m[0][2] + m[2][0]) / s, y = (m[1][2] + m[2][1]) / s, z = 0.25f * s;
  }
  return {x, y, z, w};
}

// --- Crates and the garage's parts (renut.crates, renut.parts.owned) ------------------------------------------

// unlockable_blocksets: 144-byte records {blockset aid, flag name}; a blockset: 8-byte {count, part aid} pairs.
constexpr uint32_t kCrateRecordSize = 144, kCrateRecordFlag = 4, kCrateFlagLength = 140;
constexpr uint32_t kNoCrate = 1696;               // a flag sub_82554410 treats as owned already, adding nothing
constexpr uint32_t kCrateVtable = 0x82E32F98;     // entityAvatarComponentCrate's, built at run time
constexpr uint32_t kCrateObjectFlag = 0xAC8;      // a crate's flag (the normal table)
constexpr uint32_t kWorldObjects = 1292, kObjectNode = 56;  // world: its first object's node (object + 56); node + 4: next
constexpr uint32_t kSceneCrateList = 19136 + 8;   // scene: the crate HUD's parts list sub_82554410 fills
// The garage inventory: 64-byte entries {part aid, ...}; per player slot (player 1's is 0), its total and available.
constexpr uint32_t kInventoryBegin = 0x82E19E4C, kInventoryEnd = 0x82E19E50, kInventoryStride = 0x82E19E58;
constexpr uint32_t kInventoryTotal = 36, kInventoryAvailable = 40;

// An objparams copy, as a level's placement fills it in (sub_82366440) before CreateObject: its class's name, world,
// aid, position, rotation (Euler radians) and scale; a crate's flag name (sub_823FF108 reads it) up to its next field.
constexpr uint32_t kCopyClassName = 75, kCopyWorld = 132, kCopyAid = 136, kCopyPosition = 152, kCopyRotation = 164;
constexpr uint32_t kCopyScale = 176;
constexpr uint32_t kCopyCrateFlag = 452, kCopyCrateFlagLength = 64;
// The profile's normal flags: a bit each, by the game's id (sub_823FF108).
constexpr uint32_t kProfileFlags = 276, kFlagBits = 8;
constexpr uint32_t kProfileCounters = 308;  // the profile's counters, int32 by id (flag_editor.cpp)
constexpr char kCrateClass[] = "entityAvatarComponentCrate";

// The crates' objparams aid, learned from the first crate CreateObject makes (Showdown Town places them).
std::atomic<uint32_t> g_crate_objparams{0};

// Every kind of object the game has made this session (renut.objects), by its objTag without "objTag_BanjoX_": its
// objparams aid and class. An objTag two objparams share names the later one "<tag>@<aid>".
constexpr uint32_t kCopyTag = 2, kCopyTagLength = 64;
struct Kind {
  std::string tag, cls;
  uint32_t aid = 0;
};
std::mutex g_kinds_mutex;                         // the two below
std::map<std::string, Kind> g_kinds;
std::unordered_map<uint32_t, std::string> g_made;  // an object -> its kind, until its world goes

// The kind of an objparams copy CreateObject is given, learned the first time; empty for none.
std::string LearnKind(uint32_t copy) {
  const uint32_t aid = Load32(copy + kCopyAid);
  if (uint8_t(aid >> 24) != kTypeObjparams)
    return {};
  const std::string tag = CString(copy + kCopyTag, kCopyTagLength), cls = CString(copy + kCopyClassName, 64);
  std::string name = tag.empty() ? cls : tag;
  for (const std::string_view prefix : {"objTag_BanjoX_", "objTag_"})
    if (name.starts_with(prefix)) {
      name.erase(0, prefix.size());
      break;
    }
  if (name.empty())
    return {};
  std::lock_guard lock(g_kinds_mutex);
  const auto [it, added] = g_kinds.try_emplace(name, Kind{tag, cls, aid});
  if (!added && it->second.aid != aid) {
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "@%08X", aid);
    name += suffix;
    g_kinds.try_emplace(name, Kind{tag, cls, aid});
  }
  return name;
}

// The object is on the running world's object list.
bool InWorld(uint32_t object) {
  const uint32_t world = RunningWorld();
  if (!world || !Heap(object))
    return false;
  uint32_t node = Load32(world + kWorldObjects);
  for (uint32_t n = 0; Heap(node) && n < 50000; ++n, node = Load32(node + 4))
    if (node - kObjectNode == object)
      return true;
  return false;
}

// Classes: an object's id at + 4; two tables {table, -, first id, end id} of {name, parent, getter} (sub_82245AB8).
constexpr uint32_t kObjectClass = 4, kClassTables = 0x82D99434, kClassEntry = 12;
constexpr uint32_t kClassActor = 5;  // every character: Banjo, the NPCs, the Gruntbots
uint32_t ClassEntry(uint32_t id) {
  for (const uint32_t table : {kClassTables, Load32(kClassTables + 16)}) {
    if (table && id >= Load32(table + 8) && id < Load32(table + 12))
      return Load32(table) + kClassEntry * (id - Load32(table + 8));
  }
  return 0;
}
bool ClassIsActor(uint32_t id) {
  for (int depth = 0; id && depth < 32; ++depth) {
    if (id == kClassActor)
      return true;
    const uint32_t entry = ClassEntry(id);
    id = entry ? Load32(entry + 4) : 0;
  }
  return false;
}
bool IsActor(uint32_t object) { return Heap(object) && ClassIsActor(Load32(object + kObjectClass)); }

// A character's parts (sub_8227D190): its strategy (what it's doing: states), mind (goals, actions) and body
// (animation states); the vehicle it rides, if any. A strategy's state is at + 60.
constexpr uint32_t kActorStrategy = 0xAC0, kActorMind = 0xAC4, kActorBody = 0xAC8, kActorVehicle = 0xC54;
constexpr uint32_t kStrategyState = 60;
// The names of the strategy states (id 1001 on) and body states (id = index), as the game sorts them.
constexpr uint32_t kStrategyStateNames = 0x82D9D474, kFirstStrategyState = 1001;
constexpr uint32_t kBodyStateNames = 0x82D9D5A8;
constexpr char kStrategyPrefix[] = "actorStrategyId_", kBodyPrefix[] = "actorBodyStateId_";

// A list of the game's names (char * each) that start with `prefix`, without it.
std::vector<std::string> GameNames(uint32_t names, std::string_view prefix) {
  std::vector<std::string> list;
  for (uint32_t i = 0; i < 512; ++i) {
    const std::string name = CString(Load32(names + 4 * i), 96);
    if (!name.starts_with(prefix))
      break;
    list.push_back(name.substr(prefix.size()));
  }
  return list;
}

// The dialogs: the scene's dialog manager (sub_823F05B0) and its talking actor; the scene is busy (a cutscene, a
// dialog) while its + 7020 is set. sub_823F0890 starts one (as an NPC's hit line does, sub_823D40A8).
constexpr uint32_t kSceneDialogs = 0x4AB8, kSceneBusy = 0x1B6C, kDialogTalking = 744;
constexpr uint32_t kStartDialog = 0x823F0890, kEmptyText = 0x8210A5E1;
constexpr uint8_t kTypeDialog = 0x43, kTypeDataBundle = 0x4F, kTypeStreamBundle = 0x50;
constexpr uint32_t kCallFrame = 0x100;

// Calls guest function `address` with r3.. from `args`, on a frame of its own below the lent context's stack whose
// stack arguments are 0; returns r3. Inside a renut event.
uint32_t CallWithFrame(uint32_t address, std::initializer_list<uint32_t> args) {
  if (!t_ctx)
    throw Error("this works inside the renut events (a part's or a vehicle's callbacks)");
  ::PPCFunc *function = renut::game::GuestFunction(address);
  if (!function)
    throw Error("no game function there");
  // On the lent context itself, put back afterwards but for fpscr (as rex::ppc::Call does).
  PPCContext &ctx = *t_ctx;
  const PPCContext saved = ctx;
  rex::ppc::OpenCallFrame(ctx, t_base, kCallFrame - rex::ppc::kLinkageSize);
  std::memset(Base() + ctx.r1.u32 + 4, 0, kCallFrame - 4);
  PPCRegister *const regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  size_t i = 0;
  for (const uint32_t arg : args)
    regs[i++]->u64 = arg;
  function(ctx, t_base);
  const uint32_t result = ctx.r3.u32;
  const PPCFPSCRRegister fpscr = ctx.fpscr;
  ctx = saved;
  ctx.fpscr = fpscr;
  return result;
}

// An asset id from its name ("aid_<type>_<name>", "<name>", or "0x1234ABCD").
uint32_t AidByName(uint8_t type, std::string_view type_name, std::string name) {
  if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X'))
    return uint32_t(std::strtoul(name.c_str() + 2, nullptr, 16));
  const std::string prefix = "aid_" + std::string(type_name) + "_";
  if (name.starts_with(prefix))
    name.erase(0, prefix.size());
  return renut::mods::AssetId(type, name);
}

// While a mod spawns something, an asset it needs that no loaded bundle has fails quietly instead of bringing up the
// dirty-disc screen (custom_parts.cpp's sub_82241780 hook asks QuietFailedOpen). The first such aid is kept.
std::atomic<int> g_spawning{0};
std::atomic<uint32_t> g_missing_asset{0};
struct SpawnGuard {
  SpawnGuard() {
    g_missing_asset.store(0);
    ++g_spawning;
  }
  ~SpawnGuard() { --g_spawning; }
};

// renut.player.set_model's player model (its asset id), or 0: then a world's player_model, else Banjo's own.
std::atomic<uint32_t> g_player_model{0};
// Banjo's models, his own and his tuxedo (aid_model_banjox_actor_banjo, _banjotuxedo), which a player model replaces.
constexpr uint32_t kBanjoModels[] = {0x04041D5E, 0x040DDF84};

// After a guarded CreateObject: when an asset was missing, the half-made object goes and this raises.
void ThrowIfMissing(uint32_t object) {
  const uint32_t missing = g_missing_asset.load();
  if (!missing)
    return;
  if (object)
    RemoveObject(object, 0);
  char text[160];
  std::snprintf(text, sizeof(text),
                "it needs an asset this world hasn't loaded (0x%08X): it may be another world's own version", missing);
  throw Error(text);
}

// Whether asset `aid` opens in this world: a quiet open, under a SpawnGuard.
bool AssetOpens(uint32_t aid) {
  const uint32_t s = Scratch() + 36;
  Store32(s, aid);
  const uint32_t data = LoadAsset(s, 0);
  if (!Heap(data))
    return false;
  ReleaseAsset(data);
  return true;
}

// The player model Banjo is made with in `world` (its asset id), when it's loaded; else 0.
uint32_t PlayerModelIn(uint32_t world) {
  uint32_t model = g_player_model.load();
  if (!model && Heap(world))
    model = renut::game::custom_worlds::WorldPlayerModel(Load32(world + kWorldScene));
  if (!model)
    return 0;
  // A model no loaded bundle has would crash the game: Banjo keeps his own. Looked for quietly.
  const uint32_t missing = g_missing_asset.load();
  bool loaded;
  {
    SpawnGuard guard;
    loaded = AssetOpens(model);
  }
  g_missing_asset.store(missing);
  return loaded ? model : 0;
}

// Loads the game's bundle `name` ("banjox_...") into world `owner` until it goes, as a world loads its own
// (sub_8239C318): its data (type 0x4F: objparams, models) and its stream bundle (0x50). One loaded already counts.
// The model pool (ModelPool_size): + 12 its free entries. A world bundle's models (up to 107 each) need room there.
constexpr uint32_t kModelPool = 0x82E1B754, kModelPoolFree = 12, kModelRoom = 512;

// Other worlds' bundles spawns loaded into the running world (under g_kinds_mutex), and when each was asked for.
std::map<std::string, std::chrono::steady_clock::time_point> g_world_bundles;
// A bundle asked for this recently may still be loading on the game's loader thread: its memory isn't taken yet.
constexpr auto kBundleLoading = std::chrono::seconds(15);

bool IsWorldBundle(const std::string &name) {
  return !name.starts_with("banjox_actor_") && !name.starts_with("renutworld_");
}

// What it takes once loaded: its data bundle unpacked (Bundle/4f/<id>: about three times its file) and its stream
// bundle (Bundle/50/<id>); 0 when the game has neither.
std::uintmax_t BundleBytes(const std::string &name) {
  char data[32], stream[32];
  std::snprintf(data, sizeof(data), "Bundle/4f/%06x", unsigned(renut::mods::AssetId(kTypeDataBundle, name) & 0xFFFFFF));
  std::snprintf(stream, sizeof(stream), "Bundle/50/%06x",
                unsigned(renut::mods::AssetId(kTypeStreamBundle, name) & 0xFFFFFF));
  return renut::mods::GameFileUnpackedSize(data) + renut::mods::GameFileSize(stream);
}

// Raises when the world bundles among `names` not loaded here yet, with those still loading, wouldn't fit in the
// game's physical memory: a block of each one's size, all at once, and kPhysicalSpare more must be free (then freed).
constexpr std::uintmax_t kPhysicalSpare = std::uintmax_t(4) << 20;
void CheckBundleRoom(const std::vector<std::string> &names) {
  std::vector<std::uintmax_t> sizes;
  std::uintmax_t adding = 0;
  {
    std::lock_guard lock(g_kinds_mutex);
    for (const std::string &name : names)
      if (IsWorldBundle(name) && !g_world_bundles.contains(name))
        if (const std::uintmax_t bytes = BundleBytes(name)) {
          sizes.push_back(bytes);
          adding += bytes;
        }
    if (sizes.empty())
      return;
    const auto now = std::chrono::steady_clock::now();
    for (const auto &[name, asked] : g_world_bundles)
      if (now - asked < kBundleLoading)
        if (const std::uintmax_t bytes = BundleBytes(name))
          sizes.push_back(bytes);
  }
  sizes.push_back(kPhysicalSpare);
  std::vector<uint32_t> held;
  bool fits = true;
  for (const std::uintmax_t size : sizes) {
    const uint32_t block = size < 0xF0000000u ? PhysicalAlloc(uint32_t(size), 0xFFFFFFFFu, 0, kPageReadWrite) : 0;
    if (!block) {
      fits = false;
      break;
    }
    held.push_back(block);
  }
  for (const uint32_t block : held)
    PhysicalFree(block);
  if (!fits)
    throw Error("there's no memory left in this world for another world's bundles (" + std::to_string(adding >> 20) +
                " MB loaded): try a lighter world, or this character's plain version");
}

void LoadGameBundle(uint32_t owner, const std::string &name) {
  if (IsWorldBundle(name)) {
    const uint32_t pool = Load32(kModelPool);
    if (!Heap(pool) || Load32(pool + kModelPoolFree) < kModelRoom)
      throw Error("there's no room left for another world's models here (" + name + "): the game's model pool is full");
    CheckBundleRoom({name});
  }
  const uint32_t s = Scratch() + 32;
  Store32(s, renut::mods::AssetId(kTypeDataBundle, name));
  LoadDataBundle(owner, s, 1, 0, 0);
  Store32(s, renut::mods::AssetId(kTypeStreamBundle, name));
  LoadBundle(owner, s, 1);
  if (IsWorldBundle(name)) {
    std::lock_guard lock(g_kinds_mutex);
    g_world_bundles.emplace(name, std::chrono::steady_clock::now());
  }
}

// The world bundle with a character's asset `aid` that this world lacks (actor_assets.h), or null.
const char *ActorAssetBundle(uint32_t aid) {
  const auto found = std::lower_bound(std::begin(kActorAssets), std::end(kActorAssets), aid & 0xFFFFFF,
                                      [](const ActorAsset &asset, uint32_t id) { return asset.id < id; });
  return found != std::end(kActorAssets) && found->id == (aid & 0xFFFFFF) ? kActorAssetBundles[found->bundle]
                                                                           : nullptr;
}

// The assets a copy names that making it opens without a check (sub_822B0420: its model at + 192, + 212; a
// character's strategy, mind and body objparams), so one this world hasn't loaded would crash the game. One a
// world bundle has (actor_assets.h) is loaded with it first.
constexpr uint32_t kCopyModel = 192, kCopyOpened = 212, kCopyActorParts[] = {464, 468, 472};
void RequireAssets(uint32_t copy, bool actor) {
  std::vector<uint32_t> aids = {Load32(copy + kCopyModel), Load32(copy + kCopyOpened)};
  if (actor)
    for (const uint32_t offset : kCopyActorParts)
      if (const uint32_t aid = Load32(copy + offset); uint8_t(aid >> 24) == kTypeObjparams)
        aids.push_back(aid);
  for (const uint32_t aid : aids) {
    if (!aid || AssetOpens(aid))
      continue;
    if (const char *bundle = ActorAssetBundle(aid)) {
      LoadGameBundle(RunningWorld(), std::string("banjox_") + bundle);
      if (AssetOpens(aid))
        continue;
    }
    FreeObject(copy);
    char text[160];
    std::snprintf(text, sizeof(text),
                  "it needs an asset no bundle under 32 MB has (0x%08X): it may be another world's own version", aid);
    throw Error(text);
  }
}

// Makes an object from objparams `aid` in the running world as a level's placement does: at x, y, z, turned `yaw`
// degrees, at its own scale; `fill` writes anything else into the copy first. Inside a renut event.
uint32_t SpawnObject(uint32_t aid, double x, double y, double z, double yaw,
                     const std::function<void(uint32_t copy)> &fill = {}) {
  const uint32_t world = RunningWorld();
  if (!world)
    throw Error("objects are spawned in a world");
  for (const double value : {x, y, z, yaw})
    if (!std::isfinite(value))
      throw Error("not a finite position");
  RequireGame();
  // An asset found missing while it's made (checked by the game, so quiet) has its world bundle loaded, and it's
  // made again; the half-made one goes.
  for (uint32_t tried = 0, attempts = 0;; ++attempts) {
    uint32_t object = 0, missing = 0;
    {
      const uint32_t s = Scratch();
      Store32(s, aid);
      SpawnGuard guard;
      const uint32_t copy = CopyObjparams(s);
      if (!Heap(copy))
        throw Error("its objparams aren't loaded here: they come with the worlds that have it");
      Store32(copy + kCopyWorld, world);
      const double position[3] = {x, y, z}, rotation[3] = {0.0, yaw * 3.14159265358979 / 180.0, 0.0};
      for (uint32_t i = 0; i < 3; ++i) {
        StoreFloat(copy + kCopyPosition + 4 * i, position[i]);
        StoreFloat(copy + kCopyRotation + 4 * i, rotation[i]);
      }
      StoreFloat(copy + kCopyScale, 1.0);
      if (fill)
        fill(copy);
      const uint32_t cls = ClassIdByName(copy + kCopyClassName);
      RequireAssets(copy, ClassIsActor(cls));
      object = CreateObject(cls, copy, 0);
      FreeObject(copy);
      missing = g_missing_asset.load();
    }
    if (!missing) {
      if (!object)
        throw Error("the game didn't make it");
      return object;
    }
    const char *bundle = ActorAssetBundle(missing);
    if (!bundle || missing == tried || attempts >= 4) {
      ThrowIfMissing(object);  // removes it and raises
      throw Error("the game didn't make it");
    }
    if (object)
      RemoveObject(object, 0);
    tried = missing;
    LoadGameBundle(world, std::string("banjox_") + bundle);
  }
}

// A character in the running world, or an error.
uint32_t Actor(int64_t value) {
  const uint32_t actor = Address(value, "character");
  if (!InWorld(actor) || !IsActor(actor))
    throw Error("not a character in this world");
  return actor;
}

// One of a character's parts, or an error when it has none.
uint32_t ActorPart(uint32_t actor, uint32_t offset, const char *what) {
  const uint32_t part = Load32(actor + offset);
  if (!Heap(part))
    throw Error(std::string("that character has no ") + what);
  return part;
}

// A state's id by its name ("Idle"), from the game's list of `names` starting at `first`; an error when there's none.
uint32_t StateId(uint32_t names, std::string_view prefix, uint32_t first, const std::string &name) {
  const std::vector<std::string> list = GameNames(names, prefix);
  const auto found = std::find(list.begin(), list.end(), name);
  if (found == list.end())
    throw Error("no state '" + name + "'");
  return first + uint32_t(found - list.begin());
}

// Where a spawn or a move goes (objects.Where): "front" (or empty) in front of Banjo as the camera sees him, turned
// to face him; "banjo" where he is; "xyz" at x, y, z.
struct Place {
  double x, y, z, yaw;
};
Place Where(const std::string &where, double x, double y, double z, double yaw) {
  if (where == "xyz")
    return {x, y, z, yaw};
  if (!where.empty() && where != "front" && where != "banjo")
    throw Error("no place '" + where + "' (front, banjo or xyz)");
  const uint32_t avatar = PlayerAvatar();
  if (!avatar)
    throw Error("Banjo isn't in this world to place it by");
  const double bx = LoadFloat(avatar + kAvatarPosition), by = LoadFloat(avatar + kAvatarPosition + 4),
               bz = LoadFloat(avatar + kAvatarPosition + 8);
  if (where == "banjo")
    return {bx, by, bz, yaw};
  double dx = bx - LoadFloat(kCameraMatrix + 48), dz = bz - LoadFloat(kCameraMatrix + 56);
  const double length = std::hypot(dx, dz);
  if (length < 0.01) {
    dx = 0;
    dz = 1;
  } else {
    dx /= length;
    dz /= length;
  }
  const double reach = Heap(Load32(avatar + kAvatarVehicle)) ? 6.0 : 3.0;
  return {bx + dx * reach, by, bz + dz * reach, yaw + std::atan2(-dx, -dz) * 180.0 / 3.14159265358979};
}

// The game's characters (game_characters.h) by objparams aid, and by name.
const std::unordered_map<uint32_t, std::string> &GameCharacterAids() {
  static const std::unordered_map<uint32_t, std::string> aids = [] {
    std::unordered_map<uint32_t, std::string> map;
    for (const GameCharacter &character : kGameCharacters)
      map.emplace(renut::mods::AssetId(kTypeObjparams, std::string("banjox_actor_") + character.name), character.name);
    return map;
  }();
  return aids;
}
const GameCharacter *GameCharacterNamed(std::string_view name) {
  for (const GameCharacter &character : kGameCharacters)
    if (name == character.name)
      return &character;
  return nullptr;
}

// The objparams aid each object was made from, and the last object a mod spawned (actors.Who's "last").
std::unordered_map<uint32_t, uint32_t> g_made_aid;  // under g_kinds_mutex, as g_made
std::unordered_set<uint32_t> g_mod_spawned;          // under g_kinds_mutex: what mods spawned, until its world goes
std::atomic<uint32_t> g_last_spawned{0};

// What a character in the world is called: its game character's name, else its kind. Under g_kinds_mutex.
std::string ActorName(uint32_t object) {
  if (const auto aid = g_made_aid.find(object); aid != g_made_aid.end())
    if (const auto named = GameCharacterAids().find(aid->second); named != GameCharacterAids().end())
      return named->second;
  const auto made = g_made.find(object);
  return made != g_made.end() ? made->second : std::string();
}

// The character in the running world nearest Banjo (other than him) that `match` takes, or 0.
uint32_t NearestActor(const std::function<bool(uint32_t)> &match) {
  const uint32_t world = RunningWorld(), avatar = PlayerAvatar();
  if (!world)
    return 0;
  const uint32_t from = avatar ? avatar + kAvatarPosition : kCameraMatrix + 48;
  const double fx = LoadFloat(from), fy = LoadFloat(from + 4), fz = LoadFloat(from + 8);
  uint32_t best = 0;
  double best_distance = 0;
  uint32_t node = Load32(world + kWorldObjects);
  for (uint32_t n = 0; Heap(node) && n < 50000; ++n, node = Load32(node + 4)) {
    const uint32_t object = node - kObjectNode;
    if (object == avatar || !IsActor(object) || !match(object))
      continue;
    const double dx = LoadFloat(object + kAvatarPosition) - fx, dy = LoadFloat(object + kAvatarPosition + 4) - fy,
                 dz = LoadFloat(object + kAvatarPosition + 8) - fz;
    const double distance = dx * dx + dy * dy + dz * dz;
    if (!best || distance < best_distance) {
      best = object;
      best_distance = distance;
    }
  }
  return best;
}

// The character a call acts on: `character` when given, else `who` (actors.Who): "nearest" (or empty), "last",
// "banjo", a character's name (the nearest of them) or its number.
uint32_t Who(int64_t character, const std::string &who) {
  if (character)
    return Actor(character);
  if (who == "banjo") {
    const uint32_t avatar = PlayerAvatar();
    if (!avatar)
      throw Error("Banjo isn't in this world");
    return avatar;
  }
  if (who == "last") {
    const uint32_t last = g_last_spawned.load();
    if (!last || !InWorld(last))
      throw Error("nothing a mod spawned is still in this world");
    return last;
  }
  if (!who.empty() && who.find_first_not_of("0123456789") == std::string::npos)
    return Actor(std::strtoll(who.c_str(), nullptr, 10));
  uint32_t found = 0;
  if (who.empty() || who == "nearest") {
    found = NearestActor([](uint32_t) { return true; });
  } else {
    std::lock_guard lock(g_kinds_mutex);
    found = NearestActor([&who](uint32_t object) { return ActorName(object) == who; });
  }
  if (!found)
    throw Error(who.empty() || who == "nearest" ? std::string("there's no character in this world")
                                                : "there's no '" + who + "' in this world");
  return found;
}

// Loads a game character's bundle into world `owner` (until it goes), with its group's shared one and what it lacks:
// its pack (NutTool's renut-characters), else the world bundles with it (game_characters.h).
void LoadCharacterBundles(uint32_t owner, std::string_view bundle, std::string_view character) {
  const uint32_t pack = renut::mods::AssetId(kTypeStreamBundle, "renutchar_" + std::string(character));
  const bool packed = renut::mods::HasCharacterPack(pack);
  std::vector<std::string> bundles;
  if (bundle.starts_with("gruntbot") && bundle != "gruntbotcommon")
    bundles.push_back("banjox_actor_gruntbotcommon");
  if (bundle.starts_with("npc_")) {
    bundles.push_back("banjox_actor_npccommon");
    bundles.push_back("banjox_actor_townsfolkcommon");
  }
  bundles.push_back("banjox_actor_" + std::string(bundle));
  if (!packed)
    for (const CharacterExtras &extras : kCharacterExtras)
      if (bundle == extras.bundle)
        for (const char *extra : extras.extras)
          if (extra)
            bundles.push_back(std::string("banjox_") + extra);
  CheckBundleRoom(bundles);  // all or none: no half-loaded set when the second wouldn't fit
  for (const std::string &name : bundles)
    LoadGameBundle(owner, name);
  if (packed) {
    const uint32_t s = Scratch() + 32;
    Store32(s, pack);
    LoadBundle(owner, s, 1);
  }
}

void Spawned(uint32_t object, const std::string &name, uint32_t aid);

// A mod's character (renut.npcs.add) in the running world, whichever it is: its world's bundles are loaded into this
// one (until it goes) and its objparams spawned. Inside a renut event.
uint32_t SpawnModNpc(const std::string &world, const std::string &id, const Place &at) {
  const std::string objparams = renut::game::custom_npcs::NpcObjparams(world, id);
  if (objparams.empty())
    throw Error("no character " + world + "/" + id + " (renut.npcs.add)");
  const uint32_t running = RunningWorld();
  if (!running)
    throw Error("characters are spawned in a world");
  RequireGame();
  LoadGameBundle(running, "renutworld_" + world);
  const uint32_t aid = renut::mods::AssetId(kTypeObjparams, objparams);
  const uint32_t object = SpawnObject(aid, at.x, at.y, at.z, at.yaw);
  Spawned(object, world + "/" + id, aid);
  return object;
}

// Notes what a mod spawned, for the lists and actors.Who (a character is the "last" one).
void Spawned(uint32_t object, const std::string &name, uint32_t aid) {
  std::lock_guard lock(g_kinds_mutex);
  g_made[object] = name;
  g_made_aid[object] = aid;
  g_mod_spawned.insert(object);
  if (IsActor(object))
    g_last_spawned.store(object);
}

struct Crate {
  std::string flag;
  uint32_t blockset = 0;
  std::vector<std::pair<uint32_t, uint32_t>> parts;  // part aid, count
};
std::mutex g_crates_mutex;                           // the two below
std::vector<Crate> g_crates;                         // read once, on the game thread
std::map<std::string, std::string> g_crate_swaps;   // a crate's flag -> the crate whose contents it gives

// Every crate's record and contents, from the game's assets. Game thread.
std::vector<Crate> ReadCrates() {
  static uint32_t aid = 0;
  if (!aid && !(aid = SystemAlloc(96, 16)))
    throw Error("no guest memory");
  std::vector<Crate> crates;
  AssetByName(TextScratch("unlockable_blocksets"), aid, 0);
  const uint32_t data = LoadAsset(aid, 1);
  if (!Heap(data))
    return crates;
  const uint32_t count = std::min<uint32_t>(AssetBytes(data) / kCrateRecordSize, 4096);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t record = data + i * kCrateRecordSize;
    Crate crate{CString(record + kCrateRecordFlag, kCrateFlagLength), Load32(record), {}};
    // The record starts with its blockset's aid, so it's what the game loads the blockset from.
    if (const uint32_t pairs = LoadAsset(record, 1); Heap(pairs)) {
      const uint32_t n = std::min<uint32_t>(AssetBytes(pairs) / 8, 256);
      for (uint32_t j = 0; j < n; ++j)
        crate.parts.emplace_back(Load32(pairs + 8 * j + 4), Load32(pairs + 8 * j));
      ReleaseAsset(pairs);
    }
    crates.push_back(std::move(crate));
  }
  ReleaseAsset(data);
  return crates;
}

// The crates, read the first time on the game thread (a renut event or a crate hook).
const std::vector<Crate> &Crates(bool in_game) {
  {
    std::lock_guard lock(g_crates_mutex);
    if (!g_crates.empty())
      return g_crates;
  }
  if (!in_game)
    RequireGame();
  std::vector<Crate> read = ReadCrates();
  std::lock_guard lock(g_crates_mutex);
  if (g_crates.empty())
    g_crates = std::move(read);
  return g_crates;
}

const Crate *FindCrate(const std::vector<Crate> &crates, const std::string &flag) {
  const auto found = std::find_if(crates.begin(), crates.end(), [&flag](const Crate &crate) { return crate.flag == flag; });
  return found != crates.end() ? &*found : nullptr;
}

// Guest words standing in for a crate record: just its blockset's aid, which is all the inventory reads of it.
uint32_t SwapSlot(int which) {
  static uint32_t slots = 0;
  if (!slots && !(slots = SystemAlloc(16, 16)))
    throw Error("no guest memory");
  return slots + 4 * uint32_t(which);
}

// The blockset a crate record gives instead (renut.crates.swap), or 0. Game thread.
uint32_t SwappedBlockset(uint32_t record) {
  if (!Heap(record) && (record < 0x82000000 || record >= 0x83000000))
    return 0;
  const std::string flag = CString(record + kCrateRecordFlag, kCrateFlagLength);
  if (flag.rfind("gameFlag_", 0) != 0)
    return 0;
  std::string other;
  {
    std::lock_guard lock(g_crates_mutex);
    const auto found = g_crate_swaps.find(flag);
    if (found == g_crate_swaps.end())
      return 0;
    other = found->second;
  }
  const Crate *crate = FindCrate(Crates(true), other);
  return crate ? crate->blockset : 0;
}

// The garage inventory's entry for part `aid`, or 0.
uint32_t InventoryEntry(uint32_t aid) {
  const uint32_t begin = Load32(kInventoryBegin), end = Load32(kInventoryEnd);
  const uint32_t stride = uint32_t(Base()[kInventoryStride]) << 8 | Base()[kInventoryStride + 1];
  if (!Heap(begin) || end < begin || stride < 4)
    return 0;
  for (uint32_t entry = begin, n = 0; entry < end && n < 4096; entry += stride, ++n) {
    if (Load32(entry) == aid)
      return entry;
  }
  return 0;
}

// A shipped part's objparams aid from its name, as renut.parts.add takes it ("gadgets_horn").
uint32_t PartAid(std::string name) {
  if (name.rfind("aid_objparams_", 0) == 0)
    name = name.substr(14);
  if (name.rfind("banjox_vehicleblock_", 0) != 0)
    name = "banjox_vehicleblock_" + name;
  return renut::mods::AssetId(kTypeObjparams, name);
}

// --- Projectiles of any kind, for scripted parts ----------------------------------

// A part's projectiles of one kind, made as a weapon makes its own but owned by no part (so
// they stay out of any weapon's pool); 'slots' is a guest copy of a weapon's pool.
struct ProjectilePool {
  uint32_t part = 0, aid = 0, slots = 0;
  uint32_t age = 0;  // the part's ticks since they were made
};
std::vector<ProjectilePool> g_pools;  // game thread
// New objects join their level on its next update (sub_82384AE8 only queues them); a
// projectile launched before that crashes (sub_82410820 reads its level), so a pool waits.
constexpr uint32_t kPoolReadyTicks = 2;
constexpr uint32_t kProjectileState = 0xAC0;  // 0 idle (sub_82626070 takes those)

// 'name': a renut.projectiles name's asset ("banjox_avatar_projectile_egg") or "aid_objparams_...".
uint32_t ProjectileAid(const std::string &name) {
  constexpr std::string_view kPrefix = "aid_objparams_";
  if (name.starts_with(kPrefix))
    return renut::mods::AssetId(kTypeObjparams, name.substr(kPrefix.size()));
  return renut::mods::AssetId(kTypeObjparams, "banjox_avatar_projectile_" + name);
}

ProjectilePool MakePool(uint32_t part, uint32_t vehicle, uint32_t aid, uint32_t size) {
  const uint32_t s = Scratch();
  Store32(s, aid);
  const uint32_t params = CopyObjparams(s);
  if (!params)
    throw Error("that projectile isn't loaded here (its objparams asset isn't in this level)");
  const uint32_t net = Load32(part + kObjectNet);
  const bool remote = net ? (NetObjectRemote(net) & 0xFF) != 0 : Load32(part + kObjectRemote) != 0;
  Store32(params + kProjectileMade, 1);
  Store32(params + kProjectileLocal, remote ? 0 : 1);
  Store32(params + kProjectileLevel, Load32(vehicle + kVehicleLevel));
  for (uint32_t i = 0; i < 3; ++i)
    Store32(params + kProjectileStart + 4 * i, Load32(vehicle + kVehiclePosition + 4 * i));
  Store32(params + kProjectileOwnerVehicle, vehicle);
  Store32(params + kProjectileOwnerPart, 0);
  ProjectilePool pool{part, aid, SystemAlloc(4 * kPoolSize)};
  if (!pool.slots) {
    FreeObject(params);
    throw Error("no guest memory");
  }
  std::memset(Base() + pool.slots, 0, 4 * kPoolSize);
  const uint32_t class_id = ClassIdByName(params + kProjectileClassName);
  for (uint32_t i = 0; class_id && i < size; ++i)
    Store32(pool.slots + 4 * i, CreateObject(class_id, params, 0));
  FreeObject(params);
  if (!Load32(pool.slots)) {
    SystemFree(pool.slots);
    throw Error("that asset isn't a projectile the game could make");
  }
  return pool;
}

// The pool's projectiles go once they land, as a destroyed weapon's do (sub_826257A0).
void ReleasePools(uint32_t part) {
  std::erase_if(g_pools, [part](const ProjectilePool &pool) {
    if (pool.part != part)
      return false;
    for (uint32_t i = 0; i < kPoolSize; ++i)
      if (const uint32_t projectile = Load32(pool.slots + 4 * i))
        ReleaseProjectile(projectile);
    SystemFree(pool.slots);
    return true;
  });
}

// The part's pool of 'aid' projectiles, made with up to 'size' of them the first time (it uses
// Scratch()).
const ProjectilePool &PoolFor(int64_t instance, int64_t aid_value, int64_t size_value) {
  const uint32_t part = Address(instance, "part");
  if (!renut::game::IsScriptedPart(Base(), part))
    throw Error("only parts added with renut.parts.add (with callbacks) fire a projectile of their choice");
  if (aid_value <= 0 || aid_value > 0xFFFFFFFFll || (uint32_t(aid_value) >> 24) != kTypeObjparams)
    throw Error("not a projectile's objparams asset id (0x1F......)");
  if (size_value < 1 || size_value > kPoolSize)
    throw Error("max_flying is 1-30");
  RequireGame();
  const uint32_t aid = uint32_t(aid_value), size = uint32_t(size_value);
  const uint32_t vehicle = Load32(part + kPartVehicle);
  if (!Heap(vehicle))
    throw Error("the part isn't on a vehicle");
  auto it = std::find_if(g_pools.begin(), g_pools.end(),
                         [&](const ProjectilePool &pool) { return pool.part == part && pool.aid == aid; });
  if (it == g_pools.end())
    it = g_pools.insert(g_pools.end(), MakePool(part, vehicle, aid, size));
  return *it;
}

// Whether the pool can launch one now: ready, with one not flying.
bool PoolCanFire(const ProjectilePool &pool) {
  if (pool.age < kPoolReadyTicks)
    return false;
  for (uint32_t i = 0; i < kPoolSize; ++i) {
    const uint32_t projectile = Load32(pool.slots + 4 * i);
    if (projectile && !Load32(projectile + kProjectileState) && !Load32(projectile + kObjectNet))
      return true;
  }
  return false;
}

// --- Defining parts -----------------------------------------------------------

CustomPart &Pending(const std::string &id) {
  auto it = g_pending.find(id);
  if (it == g_pending.end())
    throw Error("no part '" + id + "' is being added");
  return it->second;
}

// True for a new part; false for one already in the game (a script reload: only
// its callbacks change).
bool Define(const std::string &id, const std::string &base, const std::string &mod) {
  if (!renut::mods::ValidPartId(id))
    throw Error("a part id is 1-40 characters of a-z, 0-9 and _ (got '" + id + "')");
  const CustomPart *existing = renut::mods::FindCustomPart(id);
  if (renut::mods::CustomPartsFixed()) {
    if (existing)
      return false;
    throw Error("parts are added as the game starts: restart reNut to add '" + id + "'");
  }
  if (existing)
    throw Error("the part id '" + id + "' is taken by " + existing->mod);
  if (g_pending.contains(id))
    throw Error("the part '" + id + "' is added twice");
  CustomPart part;
  part.id = id;
  if (!base.empty())
    part.base = renut::mods::PartBaseName(base);
  part.mod = mod;
  g_pending.emplace(id, std::move(part));
  return true;
}

// A block's objparams record (class vehicleBlock, 960 bytes), written from scratch: its
// grouping strings, mass, health and paint; reNut adds the name, model and footprint.
std::vector<uint8_t> BlockRecord(const std::string &category, double mass, double health, const std::string &paint) {
  std::vector<uint8_t> r(0x3C0, 0);
  const auto text = [&](uint32_t at, const std::string &s, size_t field) {
    std::memcpy(r.data() + at, s.data(), std::min(s.size(), field - 1));
  };
  const auto word = [&](uint32_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i)
      r[at + i] = uint8_t(v >> (24 - 8 * i));
  };
  const auto real = [&](uint32_t at, double v) {
    const float f = float(v);
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    word(at, bits);
  };
  r[0] = 0x03, r[1] = 0xC0;                    // record size
  text(0x002, "objTag_Null", 0x40);
  text(0x042, "objDefId_vehicleBlock", 0x40);  // the class: a plain block
  text(0x130, paint, 0x20);                    // paint colour (a paint_colours name)
  real(0x190, mass);
  real(0x194, health);
  word(0x1B8, 1);                              // strength and weight ratings
  word(0x1BC, 1);
  word(0x220, 1);
  text(0x228, category, 0x40);                 // garage category (reNut makes its type and size)
  text(0x2E8, "metal_light", 0x40);            // part material, surface and hit material
  text(0x328, "vehicledefault", 0x40);
  text(0x368, "metalblock", 0x40);
  return r;
}

std::vector<uint8_t> FieldBytes(const std::string &type, double value) {
  std::vector<uint8_t> bytes;
  const auto put = [&](uint64_t v, size_t size) {
    for (size_t i = 0; i < size; ++i)
      bytes.push_back(uint8_t(v >> (8 * (size - 1 - i))));
  };
  if (type == "f32") {
    const float f = float(value);
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    put(bits, 4);
  } else if (type == "u32" || type == "i32") {
    put(uint32_t(int64_t(std::llround(value))), 4);
  } else if (type == "u16") {
    put(uint16_t(std::llround(value)), 2);
  } else if (type == "u8") {
    put(uint8_t(std::llround(value)), 1);
  } else {
    throw Error("unknown field type '" + type + "' (f32, u32, i32, u16, u8 or str)");
  }
  return bytes;
}

void AddField(const std::string &id, int64_t offset, std::vector<uint8_t> bytes) {
  if (offset < 0 || offset >= kMaxRecordSize)
    throw Error("field offset out of range");
  Pending(id).fields.push_back({uint32_t(offset), std::move(bytes)});
}

// --- Firing events ----------------------------------------------------------------

// Fires 'event' with the game reachable from its listeners; 'ctx' (a part's class
// callback's) is lent too, and is as it was afterwards.
template <typename... A>
void Fire(PPCContext *ctx, uint8_t *base, const rexfabric::Event &event, A &&...args) {
  if (!event.HasListeners())
    return;
  std::optional<PPCContext> saved;
  if (ctx)
    saved = *ctx;
  PPCContext *const outer_ctx = t_ctx;
  uint8_t *const outer_base = t_base;
  if (ctx) {
    t_ctx = ctx;
    t_base = base;
  }
  ++t_events;
  event.Fire(std::forward<A>(args)...);
  --t_events;
  t_ctx = outer_ctx;
  t_base = outer_base;
  if (ctx)
    *ctx = *saved;
}

// Fires a damage event whose listeners may change 'amount' (renut.set_amount).
template <typename... A>
void FireDamage(PPCContext &ctx, uint8_t *base, const rexfabric::Event &event, double &amount, A &&...args) {
  double *const outer = t_amount;
  t_amount = &amount;
  Fire(&ctx, base, event, std::forward<A>(args)..., amount);
  t_amount = outer;
}

// --- Trigger zones (renut.zones) --------------------------------------------------
// A sphere (radius) or a box (full size), watching Banjo, his vehicle or the characters, in one world or any; each
// game tick, what came in or went out fires renut.zone_enter / renut.zone_exit.

constexpr uint32_t kWatchBanjo = 1, kWatchVehicle = 2, kWatchCharacters = 4;
struct Zone {
  bool box = false;
  double at[3] = {}, size[3] = {};
  uint32_t watch = kWatchBanjo;
  std::string world;                                    // WorldName's, or "" for any
  std::unordered_map<uint32_t, const char *> inside;    // an object -> what it is ("banjo", ...)
};
std::mutex g_zones_mutex;  // the one below
std::map<std::string, Zone> g_zones;

uint32_t WatchBits(const std::string &watch) {
  uint32_t bits = 0;
  size_t at = 0;
  while (at <= watch.size()) {
    const size_t comma = std::min(watch.find(',', at), watch.size());
    const std::string word = watch.substr(at, comma - at);
    if (word == "banjo")
      bits |= kWatchBanjo;
    else if (word == "vehicle")
      bits |= kWatchVehicle;
    else if (word == "characters")
      bits |= kWatchCharacters;
    else if (word == "all")
      bits |= kWatchBanjo | kWatchVehicle | kWatchCharacters;
    else if (!word.empty())
      throw Error("a zone watches banjo, vehicle, characters or all, not '" + word + "'");
    at = comma + 1;
  }
  return bits ? bits : kWatchBanjo;
}

bool InZone(const Zone &zone, uint32_t object) {
  double d[3];
  for (uint32_t i = 0; i < 3; ++i)
    d[i] = LoadFloat(object + kAvatarPosition + 4 * i) - zone.at[i];
  if (zone.box)
    return std::abs(d[0]) <= zone.size[0] / 2 && std::abs(d[1]) <= zone.size[1] / 2 && std::abs(d[2]) <= zone.size[2] / 2;
  return d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= zone.size[0] * zone.size[0];
}

// Game thread, each game tick: what entered or left each zone, then their events.
void ZonesTick(PPCContext &ctx, uint8_t *base) {
  struct Change {
    std::string zone;
    uint32_t object;
    std::string what;
    bool entered;
  };
  std::vector<Change> changes;
  {
    std::lock_guard lock(g_zones_mutex);
    const uint32_t world = RunningWorld();
    if (g_zones.empty() || !world)
      return;
    const std::string world_name = WorldName(Load32(world + kWorldNumber));
    const uint32_t scene = Load32(world + kWorldScene);
    uint32_t watched = 0;
    for (const auto &[name, zone] : g_zones)
      watched |= zone.watch;
    struct Candidate {
      uint32_t object, bit;
      const char *what;
    };
    std::vector<Candidate> candidates;
    const uint32_t avatar = PlayerAvatar();
    const uint32_t vehicle = avatar ? Load32(avatar + kAvatarVehicle) : 0;
    if (avatar && (watched & kWatchBanjo))
      candidates.push_back({avatar, kWatchBanjo, "banjo"});
    if (Heap(vehicle) && (watched & kWatchVehicle))
      candidates.push_back({vehicle, kWatchVehicle, "vehicle"});
    if (watched & kWatchCharacters) {
      uint32_t node = Load32(world + kWorldObjects);
      for (uint32_t n = 0; Heap(node) && n < 50000; ++n, node = Load32(node + 4)) {
        const uint32_t object = node - kObjectNode;
        if (object != avatar && IsActor(object))
          candidates.push_back({object, kWatchCharacters, "character"});
      }
    }
    for (auto &[name, zone] : g_zones) {
      // Its world: a game world's name, or a mod world's id (renut.worlds.add), by its scene.
      if (!zone.world.empty() && zone.world != world_name && renut::game::custom_worlds::WorldScene(zone.world) != scene)
        continue;
      std::unordered_map<uint32_t, const char *> now;
      for (const Candidate &candidate : candidates)
        if ((zone.watch & candidate.bit) && InZone(zone, candidate.object))
          now.emplace(candidate.object, candidate.what);
      for (const auto &[object, what] : now)
        if (!zone.inside.contains(object))
          changes.push_back({name, object, what, true});
      for (const auto &[object, what] : zone.inside)
        if (!now.contains(object))
          changes.push_back({name, object, what, false});
      zone.inside = std::move(now);
    }
  }
  if (g_events)
    for (const Change &change : changes)
      Fire(&ctx, base, change.entered ? g_events->zone_enter : g_events->zone_exit, change.zone,
           int64_t(change.object), change.what);
}

// --- The HUD (renut.hud) ----------------------------------------------------------
// The game's message box (text, no buttons; or two buttons, "confirm" and "cancel"), and a challenge's score/time
// panel (a timer, a counter) shown outside one. The HUD: *(*0x82E19A60 + 80); each player's container in it.

constexpr uint32_t kHudOwner = 0x82E19A60, kHudOfOwner = 80;
constexpr uint32_t kBoxArgA = 196, kBoxArgB = 204, kBoxTwoChoices = 192, kBoxButtonA = 240, kBoxButtonB = 244,
                   kBoxSwapped = 336;
constexpr uint32_t kChoiceToken = 0x524E0000;  // "RN" and the choice's number: a reNut box's argument
constexpr uint32_t kPanelScoreText = 192;      // the panel scene's score element
// The panel's opening parameters (sub_825AEE10): show the timer, show the score, initial score and time, thresholds.
constexpr uint32_t kPanelShowTime = 0, kPanelShowScore = 4, kPanelScore = 24, kPanelTime = 28, kPanelThresholds = 44;
constexpr uint32_t kPanelParams = 13 * 4;

// Writes `text` (UTF-8) at `at` as the game's UTF-16 big-endian text, at most `chars` units with its terminating 0.
void StoreUtf16(uint32_t at, const std::string &text, uint32_t chars) {
  uint32_t n = 0;
  const auto put = [&](uint32_t unit) {
    if (n + 1 < chars) {
      Base()[at + 2 * n] = uint8_t(unit >> 8);
      Base()[at + 2 * n + 1] = uint8_t(unit);
      ++n;
    }
  };
  for (size_t i = 0; i < text.size();) {
    const uint8_t c = uint8_t(text[i]);
    uint32_t cp = c, extra = 0;
    if (c >= 0xF0) cp = c & 0x07, extra = 3;
    else if (c >= 0xE0) cp = c & 0x0F, extra = 2;
    else if (c >= 0xC0) cp = c & 0x1F, extra = 1;
    ++i;
    for (; extra && i < text.size(); --extra, ++i)
      cp = cp << 6 | (uint8_t(text[i]) & 0x3F);
    if (cp >= 0x10000) {
      cp -= 0x10000;
      put(0xD800 | (cp >> 10));
      put(0xDC00 | (cp & 0x3FF));
    } else {
      put(cp);
    }
  }
  Base()[at + 2 * n] = 0;
  Base()[at + 2 * n + 1] = 0;
}

// `text` (UTF-8) as the game's UTF-16 big-endian text, in a guest buffer of its own per `slot`. Game thread.
uint32_t Text16(const std::string &text, uint32_t slot) {
  constexpr uint32_t kChars = 1024, kSlots = 3;
  static uint32_t buffers = 0;
  if (!buffers && !(buffers = SystemAlloc(kChars * 2 * kSlots, 16)))
    throw Error("no guest memory");
  const uint32_t at = buffers + kChars * 2 * (slot % kSlots);
  StoreUtf16(at, text, kChars);
  return at;
}

// The HUD and player 1's container in it, or an error.
struct HudOf {
  uint32_t hud = 0, profile = 0, container = 0, last = 0;
};
HudOf Hud() {
  HudOf of;
  of.profile = Load32(kPlayerProfile);
  const uint32_t owner = Load32(kHudOwner);
  of.hud = Heap(owner) ? Load32(owner + kHudOfOwner) : 0;
  if (!Heap(of.hud) || !Heap(of.profile))
    throw Error("there's no HUD up");
  const uint32_t end = Load32(of.hud + 8);
  const uint32_t last = end == Load32(of.hud + 4) ? 0 : end - (uint32_t(Base()[of.hud + 16]) << 8 | Base()[of.hud + 17]);
  of.last = last;
  of.container = last ? HudPlayer(of.hud, last, of.profile) : 0;
  if (!Heap(of.container))
    throw Error("there's no HUD up for the player");
  return of;
}

// The panel as Lua set it, put on screen each tick; and the choices answered since the last tick.
struct Panel {
  bool open = false, timer = false, score = false;
  double seconds = 0, rate = 0;  // the timer's, and how it moves (-1 down, 1 up, 0 held)
  bool done = false;             // a countdown reached 0 (renut.hud_timer_done fired)
  std::string counter;           // the score's text, "" to show `value`
  int64_t value = 0;
};
std::mutex g_hud_mutex;  // the four below
Panel g_panel;
std::vector<std::pair<int64_t, bool>> g_answers;  // a choice's number, A (true) or B
double g_message_left = 0;                        // seconds before reNut's message box closes; 0: it stays
std::chrono::steady_clock::time_point g_hud_last{};

// Game thread, each game tick: the panel's timer and counter, the message box's time, the answers' events.
void HudTick(PPCContext &ctx, uint8_t *base) {
  const auto now = std::chrono::steady_clock::now();
  bool timer_done = false, close_message = false;
  std::vector<std::pair<int64_t, bool>> answers;
  Panel panel;
  {
    std::lock_guard lock(g_hud_mutex);
    const double dt = g_hud_last.time_since_epoch().count() ? std::min(0.1, std::chrono::duration<double>(now - g_hud_last).count()) : 0;
    g_hud_last = now;
    if (g_panel.open && g_panel.timer && g_panel.rate != 0) {
      g_panel.seconds += g_panel.rate * dt;
      if (g_panel.rate < 0 && g_panel.seconds <= 0) {
        g_panel.seconds = 0;
        g_panel.rate = 0;
        timer_done = !g_panel.done;
        g_panel.done = true;
      }
    }
    if (g_message_left > 0 && (g_message_left -= dt) <= 0) {
      g_message_left = 0;
      close_message = true;
    }
    answers.swap(g_answers);
    panel = g_panel;
  }
  try {
    const HudOf of = Hud();
    if (close_message)
      CloseMessageBox(of.profile, 1);
    if (panel.open)
      if (const uint32_t scene = ScorePanel(of.hud, of.profile); Heap(scene)) {
        if (panel.timer)
          SetPanelTime(scene, panel.seconds);
        if (panel.score) {
          if (panel.counter.empty())
            SetPanelScore(scene, uint32_t(panel.value));
          else if (Heap(Load32(scene + kPanelScoreText)))
            SetElementText(Load32(scene + kPanelScoreText), Text16(panel.counter, 2));
        }
      }
  } catch (const Error &) {
    // No HUD this tick (a menu, a load): it's put on screen when there's one.
  }
  if (g_events) {
    if (timer_done)
      Fire(&ctx, base, g_events->hud_timer_done);
    for (const auto &[choice, a] : answers)
      Fire(&ctx, base, g_events->hud_choice, choice, a);
  }
}

// --- Calling the game ------------------------------------------------------------

// Calls the vehicle body's hkpMotion function at vtable offset 'slot' (r4.. from 'args',
// f1 = dt for forces), after waking the body; needs a renut event's context.
void CallMotion(int64_t vehicle, uint32_t slot, std::initializer_list<uint32_t> args,
                std::optional<double> dt = std::nullopt) {
  if (!t_ctx)
    throw Error("this works inside the renut events (a part's or a vehicle's callbacks)");
  const uint32_t body = Body(vehicle);
  ::PPCFunc *activate = renut::game::GuestFunction(kActivateBody);
  const uint32_t motion = body + kBodyHkMotion;
  ::PPCFunc *call = renut::game::GuestFunction(Load32(Load32(motion) + slot));
  if (!activate || !call)
    throw Error("no Havok function there");
  t_ctx->r3.u64 = body;
  activate(*t_ctx, t_base);
  PPCRegister *const regs[] = {&t_ctx->r4, &t_ctx->r5, &t_ctx->r6};
  t_ctx->r3.u64 = motion;
  size_t i = 0;
  for (uint32_t arg : args)
    regs[i++]->u64 = arg;
  if (dt)
    t_ctx->f1.f64 = *dt;
  call(*t_ctx, t_base);
}

double DriverInput(int64_t vehicle, int64_t which) {
  if (which < kSteer || which > kBraking)
    throw Error("unknown driver input");
  const uint32_t v = Address(vehicle, "vehicle");
  if (Load32(v + kDriveUnitCount)) {
    const uint32_t unit = Load32(v + kDriveUnits);
    const uint32_t input = Heap(unit) ? Load32(unit + kDriveUnitInput) : 0;
    if (!Heap(input))
      return 0;
    if (which == kBraking)
      return Base()[input + 24] ? 1 : 0;
    return LoadFloat(input + 8 + 4 * uint32_t(which));
  }
  // No drive units (no wheels): the driver's pad, as the game reads it.
  if (!t_events || which == kBraking)
    return 0;
  const uint32_t pad = DriverPad(v, 0);
  if (!Heap(pad))
    return 0;
  const uint32_t frame = pad + kPadFrameIndex + 40 * Load32(pad + kPadFrameIndex);
  const double flip = Load32(v + kVehicleStickFlipped) ? -1.0 : 1.0;
  switch (which) {
  case kSteer: return flip * LoadFloat(frame + 20);
  case kStickY: return flip * LoadFloat(frame + 24);
  case kBrake: return LoadFloat(frame + 36);
  default: return LoadFloat(frame + 40);
  }
}

// UTF-16 (big-endian, the basic plane) to UTF-8.
std::string Utf8(uint32_t va, uint32_t length) {
  std::string out;
  for (uint32_t i = 0; i < length; ++i) {
    const uint16_t c = uint16_t(Base()[va + 2 * i] << 8 | Base()[va + 2 * i + 1]);
    if (!c)
      break;
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | (c >> 6));
      out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xE0 | (c >> 12));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    }
  }
  return out;
}

// --- The game's object messages ---------------------------------------------------
// Ids and sizes from the table sub_822107C8 reads (dword_821051F8: {name, size} by id).

constexpr uint32_t kMsgContactingFloor = 148;  // objMsgId_Object_GetIsContactingFloor (8): its answer at + 4
constexpr uint32_t kMsgGetScale = 150;         // objMsgId_Object_GetScale (8): f32 at + 4
constexpr uint32_t kMsgGetVelocity = 151;      // objMsgId_Object_GetVelocity (32): vec4 at + 16 (sub_823D94A8)
constexpr uint32_t kMsgAddVelocity = 165;      // objMsgId_Banjo_Actor_ApplyVelocityAccumulator (48), sub_82481830's
constexpr uint32_t kMsgKnockedOut = 173;       // objMsgId_Banjo_Actor_KnockedOutOfVehicle (4): its driver gets out
constexpr uint32_t kMessageBytes = 128, kMessagePayload = 16;

// Sends message `id` to `entity` after `fill` writes into it; returns the message, holding the answer. In an event.
uint32_t SendObjectMessage(uint32_t entity, uint32_t id, const std::function<void(uint32_t message)> &fill = {}) {
  static uint32_t message = 0;
  if (!message && !(message = SystemAlloc(kMessageBytes, 16)))
    throw Error("no guest memory");
  RequireGame();
  const uint32_t vtable = Heap(entity) ? Load32(entity) : 0;
  if (vtable < 0x82000000 || vtable >= 0x83000000)
    throw Error("that isn't one of the game's objects");
  std::memset(Base() + message, 0, kMessageBytes);
  InitMessage(message, id);
  if (fill)
    fill(message);
  CallWithFrame(Load32(vtable + 12), {entity, message});
  return message;
}

// --- More of the game's state (the later additions) -------------------------------

// A level's physics: its manager at + 1744 (sub_8238E258, from sub_82391BA0), whose + 8 is its hkpWorld; it's read
// so next to the level's effects (+ 3120) by sub_82259C48.
constexpr uint32_t kWorldHavok = 1744 + 8;
constexpr uint32_t kHkpWorldVtable = 0x82055DE8;  // hkpWorld's (its RTTI's object locator at 0x8216CC78)

// The running world's hkpWorld, else that of the level Banjo is in (entity + 76).
uint32_t LevelPhysics() {
  const uint32_t avatar = PlayerAvatar();
  for (const uint32_t level : {RunningWorld(), avatar ? Load32(avatar + kVehicleLevel) : 0u}) {
    const uint32_t havok = Heap(level) ? Load32(level + kWorldHavok) : 0;
    if (Heap(havok) && Load32(havok) == kHkpWorldVtable)
      return havok;
  }
  throw Error("this world has no physics world up");
}

// Player `number` (1-4)'s avatar in the running world, or 0: dword_82E1B280[number - 1], its slot at + 128.
uint32_t PlayerAvatarOf(int64_t number) {
  if (number < 1 || number > 4)
    throw Error("a player is 1 to 4");
  const uint32_t world = RunningWorld(), profile = Load32(kPlayerProfile + 4 * uint32_t(number - 1));
  if (!world || !Heap(profile))
    return 0;
  const uint32_t slot = Load32(profile + kProfilePlayer);
  const uint32_t avatar = slot < 4 ? Load32(world + kWorldAvatars + 8 * slot) : 0;
  return Heap(avatar) ? avatar : 0;
}

// An entity's rotation quaternion (x, y, z, w), as custom_npcs.cpp reads its yaw.
constexpr uint32_t kEntityRotation = 0xC0;
double EntityYaw(uint32_t entity) {
  const double x = LoadFloat(entity + kEntityRotation), y = LoadFloat(entity + kEntityRotation + 4),
               z = LoadFloat(entity + kEntityRotation + 8), w = LoadFloat(entity + kEntityRotation + 12);
  return std::atan2(2 * (w * y + x * z), 1 - 2 * (x * x + y * y)) * 180.0 / 3.14159265358979;
}

// A body's state now (sub_8227C7F8 keeps the id at + 1648), by the game's names; "" for none.
constexpr uint32_t kBodyStateNow = 1648;
std::string BodyStateNow(uint32_t actor) {
  static std::vector<std::string> names;  // sorted by the game before a level runs, then fixed
  const uint32_t body = Heap(actor) ? Load32(actor + kActorBody) : 0;
  if (!Heap(body))
    return {};
  if (names.empty())
    names = GameNames(kBodyStateNames, kBodyPrefix);
  const uint32_t id = Load32(body + kBodyStateNow);
  return id < names.size() ? names[id] : std::string();
}

// The vehicles in the game, from their init to their leaving the level (renut.vehicle.list).
std::mutex g_vehicles_mutex;
std::set<uint32_t> g_vehicles;

// renut.game.set_time_scale: the game's frame time (g_frameDeltaSeconds and its copy, set once at boot by
// sub_82284968), its own value, and the scale on it.
constexpr uint32_t kFrameDelta = 0x82E19924, kFrameDeltaCopy = 0x82E19920;
std::atomic<float> g_frame_delta{0.0f};
std::atomic<double> g_time_scale{1.0};

// Player 1's last body state and save, for renut.player_state and renut.game_load (game thread).
std::string g_player_state;
uint32_t g_player_state_avatar = 0;
uint32_t g_loaded_counters = 0;

// renut.sound.cue's cues: their records {handle, -1, -1, 0, cue id} (stereo.cpp's StartRecord), by handle.
constexpr uint32_t kCueRecordBytes = 32, kCueRecordId = 16, kMaxCues = 64;
std::map<int64_t, uint32_t> g_cues;  // game thread
int64_t g_next_cue = 1;

// Each player's pad: g_padInputs[port] + 52 its record, whose + 0 is the current entry's index (0 or 1), the two
// 40-byte entries from + 4 (padStatePush, sub_821CB628); the player's port at their profile + 128.
constexpr uint32_t kPadInputs = 0x82E0B158, kPadRecord = 52, kPadIndex = 0, kPadEntries = 4, kPadEntry = 40;
// Player `number`'s pad entry this frame into `entry` (big-endian, as the game keeps it): what a mod holding the game's
// input keeps from it (keyboard_input.cpp), else the record's current one. False without a pad.
bool PadEntry(int64_t number, uint8_t (&entry)[kPadEntry]) {
  if (number < 1 || number > 4)
    throw Error("a player is 1 to 4");
  const uint32_t profile = Load32(kPlayerProfile + 4 * uint32_t(number - 1));
  const uint32_t port = Heap(profile) ? Load32(profile + kProfilePlayer) : 4;
  if (port >= 4)
    return false;
  if (renut::game::HeldPadEntry(port, entry))
    return true;
  const uint32_t inputs = Load32(kPadInputs + 4 * port);
  if (!Heap(inputs))
    return false;
  const uint32_t record = inputs + kPadRecord;
  const uint32_t index = Load32(record + kPadIndex);
  if (index >= 2)
    return false;
  std::memcpy(entry, Base() + record + kPadEntries + kPadEntry * index, kPadEntry);
  return true;
}

// Wakes a vehicle's body, as CallMotion does before Havok's virtuals; needs a renut event's context.
void ActivateBody(uint32_t body) {
  if (!t_ctx)
    throw Error("this works inside the renut events (a part's or a vehicle's callbacks)");
  ::PPCFunc *activate = renut::game::GuestFunction(kActivateBody);
  if (!activate)
    throw Error("no Havok function there");
  t_ctx->r3.u64 = body;
  activate(*t_ctx, t_base);
}

// hkMotionState after the position (kBodyMotion): the swept transform's two rotations and its local centre of mass.
constexpr uint32_t kMotionRotations[] = {0x30, 0x40};
constexpr uint32_t kMotionLocalCentre = 0x50;


// --- The Lua side: renut.parts.add, part and vehicle objects, vectors, events ----

constexpr const char kLua[] = R"lua(
local parts, vehicles, fx = renut.parts, renut.vehicle, renut.fx

-- Vectors ------------------------------------------------------------------------
local Vec = {}
Vec.__index = Vec
local function vec(x, y, z)
  return setmetatable({x = x or 0, y = y or 0, z = z or 0}, Vec)
end
-- A place as the _ functions take it: a position, "front", "banjo", or nothing for `default`.
local function where_of(at, default)
  if type(at) == "table" then return "xyz", at.x, at.y, at.z end
  return at or default, 0, 0, 0
end
Vec.__add = function(a, b) return vec(a.x + b.x, a.y + b.y, a.z + b.z) end
Vec.__sub = function(a, b) return vec(a.x - b.x, a.y - b.y, a.z - b.z) end
Vec.__unm = function(a) return vec(-a.x, -a.y, -a.z) end
Vec.__mul = function(a, b)
  if type(a) == "number" then a, b = b, a end
  return vec(a.x * b, a.y * b, a.z * b)
end
Vec.__div = function(a, s) return vec(a.x / s, a.y / s, a.z / s) end
Vec.__eq = function(a, b) return a.x == b.x and a.y == b.y and a.z == b.z end
Vec.__tostring = function(a) return string.format("vec(%g, %g, %g)", a.x, a.y, a.z) end
function Vec:length() return math.sqrt(self.x * self.x + self.y * self.y + self.z * self.z) end
function Vec:normalized()
  local l = self:length()
  return l > 0 and self / l or vec()
end
function Vec:dot(b) return self.x * b.x + self.y * b.y + self.z * b.z end
function Vec:cross(b)
  return vec(self.y * b.z - self.z * b.y, self.z * b.x - self.x * b.z, self.x * b.y - self.y * b.x)
end
renut.vec = vec

local instances = {}  -- part instance address -> its part table (custom parts)

-- Parts --------------------------------------------------------------------------
local Part = {}
Part.__index = Part
Part.__eq = function(a, b) return a.instance == b.instance end
Part.__tostring = function(p) return string.format("part(%s, 0x%08X)", p.id or p:class(), p.instance) end

-- A part object for any part (a custom part's own table when it has one). A custom part no event
-- has shown yet (a block without callbacks) still gets its id and params.
local function wrap_part(instance)
  if not instance or instance == 0 then return nil end
  if instances[instance] then return instances[instance] end
  local id = parts._id_of(instance)
  if id == "" then return setmetatable({instance = instance}, Part) end
  local def = parts._lua_def(id)
  return setmetatable({id = id, instance = instance, params = def and def.params or {}}, Part)
end
parts.wrap = wrap_part

-- Vehicles -----------------------------------------------------------------------
local POSITION, VELOCITY, ANGULAR, RIGHT, UP, FORWARD = 0, 1, 2, 3, 4, 5
local STEER, STICK_Y, BRAKE, ACCELERATE, BRAKING = 0, 1, 2, 3, 4
local RESOURCES = {fuel = 0, ammo = 1, power = 2}
local Vehicle = {}
Vehicle.__index = Vehicle
Vehicle.__eq = function(a, b) return a.address == b.address end
Vehicle.__tostring = function(v) return string.format("vehicle(0x%08X)", v.address) end
function vehicles.at(address)
  if not address or address == 0 then return nil end
  return setmetatable({address = address}, Vehicle)
end

local function read(v, what)
  local a = v.address
  return vec(vehicles._get(a, what, 0), vehicles._get(a, what, 1), vehicles._get(a, what, 2))
end
local function write(v, what, p) vehicles._set(v.address, what, p.x, p.y, p.z) end
local function resource(kind)
  local r = RESOURCES[kind] or kind
  assert(type(r) == "number", "a resource is \"fuel\", \"ammo\" or \"power\"")
  return r
end

-- Motion.
function Vehicle:has_body() return vehicles._has_body(self.address) end
function Vehicle:position() return read(self, POSITION) end
function Vehicle:set_position(p) write(self, POSITION, p) end
function Vehicle:move(d) write(self, POSITION, read(self, POSITION) + d) end
function Vehicle:velocity() return read(self, VELOCITY) end
function Vehicle:set_velocity(v) write(self, VELOCITY, v) end
function Vehicle:angular_velocity() return read(self, ANGULAR) end
function Vehicle:set_angular_velocity(w) write(self, ANGULAR, w) end
function Vehicle:right() return read(self, RIGHT) end
function Vehicle:up() return read(self, UP) end
function Vehicle:forward() return read(self, FORWARD) end
function Vehicle:speed() return read(self, VELOCITY):dot(read(self, FORWARD)) end
function Vehicle:mass()
  local inverse = vehicles._inverse_mass(self.address)
  return inverse > 0 and 1 / inverse or math.huge
end
function Vehicle:impulse(j, point)
  if point then
    vehicles._impulse_at(self.address, j.x, j.y, j.z, point.x, point.y, point.z)
  else
    vehicles._force(self.address, 1, j.x, j.y, j.z)
  end
end
function Vehicle:angular_impulse(j) vehicles._angular_impulse(self.address, j.x, j.y, j.z) end
function Vehicle:in_air(reach) return vehicles._in_air(self.address, reach or 0) end
function Vehicle:push(force, dt, point)
  if point then
    vehicles._force_at(self.address, dt, force.x, force.y, force.z, point.x, point.y, point.z)
  else
    vehicles._force(self.address, dt, force.x, force.y, force.z)
  end
end

-- Driver.
function Vehicle:input()
  local a = self.address
  return {steer = vehicles._input(a, STEER), stick_y = vehicles._input(a, STICK_Y),
          brake = vehicles._input(a, BRAKE), accelerate = vehicles._input(a, ACCELERATE),
          braking = vehicles._input(a, BRAKING) ~= 0}
end
function Vehicle:throttle()
  return vehicles._input(self.address, ACCELERATE) - vehicles._input(self.address, BRAKE)
end
function Vehicle:steering() return vehicles._input(self.address, STEER) end
function Vehicle:driver()
  local d = vehicles._driver(self.address)
  return d ~= 0 and d or nil
end

-- Health.
function Vehicle:health() return vehicles._health(self.address, 0) end
function Vehicle:max_health() return vehicles._health(self.address, 1) end
function Vehicle:health_fraction() return vehicles._health(self.address, 2) end
function Vehicle:damage_level() return vehicles._health(self.address, 3) end
function Vehicle:set_health(h) vehicles._set_health(self.address, h) end
function Vehicle:repair() vehicles._repair(self.address) end
function Vehicle:damage(amount, part, attacker)
  vehicles._damage(self.address, amount, part and part.instance or 0, attacker or 0)
end
function Vehicle:disabled() return vehicles._disabled(self.address) end
function Vehicle:invulnerable() return vehicles._invulnerable(self.address) end
function Vehicle:set_invulnerable(on) vehicles._set_invulnerable(self.address, on ~= false) end

-- Fuel, ammo and power.
function Vehicle:resource(kind) return vehicles._resource(self.address, resource(kind), 0) end
function Vehicle:max_resource(kind) return vehicles._resource(self.address, resource(kind), 1) end
function Vehicle:set_resource(kind, value) vehicles._set_resource(self.address, resource(kind), value) end
function Vehicle:use(kind, amount) return vehicles._use(self.address, resource(kind), amount) end
function Vehicle:infinite(kind) return vehicles._resource(self.address, resource(kind), 2) ~= 0 end
function Vehicle:set_infinite(kind, on) vehicles._set_infinite(self.address, resource(kind), on ~= false) end
function Vehicle:fuel() return self:resource("fuel") end
function Vehicle:max_fuel() return self:max_resource("fuel") end
function Vehicle:set_fuel(value) self:set_resource("fuel", value) end
function Vehicle:ammo() return self:resource("ammo") end
function Vehicle:max_ammo() return self:max_resource("ammo") end
function Vehicle:set_ammo(value) self:set_resource("ammo", value) end

-- The rest.
function Vehicle:name() return vehicles._name(self.address) end
function Vehicle:level() return vehicles._level(self.address) end
function Vehicle:parts()
  local list = {}
  for i = 0, vehicles._part_count(self.address) - 1 do
    list[#list + 1] = wrap_part(vehicles._part(self.address, i))
  end
  return list
end
function Vehicle:find_parts(class)
  local list = {}
  for _, p in ipairs(self:parts()) do
    if p:is_a(class) then list[#list + 1] = p end
  end
  return list
end

-- Part methods ------------------------------------------------------------------
function Part:vehicle()
  local a = self._vehicle
  if not a or a == 0 then a = parts._vehicle_of(self.instance) end
  return vehicles.at(a)
end
function Part:def() return self.id and parts._lua_def(self.id) end
function Part:class() return parts._class(self.instance) end
function Part:is_a(class) return parts._is_a(self.instance, class) end
function Part:aid() return parts._aid(self.instance) end
-- Its model's shape keys (a NutTool block's), this part's own: renut.shapes.set sets every part's.
function Part:shape(name)
  local i = parts._shape_index(self.instance, tostring(name))
  if i < 0 then return nil end
  return parts._shape(self.instance, i)
end
function Part:set_shape(name, weight)
  assert(type(weight) == "number", "part:set_shape: the weight is a number (0 none, 1 all of it)")
  local i = parts._shape_index(self.instance, tostring(name))
  if i < 0 then error("part:set_shape: its model has no shape '" .. tostring(name) .. "'", 2) end
  parts._set_shape(self.instance, i, weight)
end
function Part:position()
  local i = self.instance
  return vec(parts._position(i, 0), parts._position(i, 1), parts._position(i, 2))
end
function Part:cell() return parts._cell(self.instance, 0), parts._cell(self.instance, 1), parts._cell(self.instance, 2) end
function Part:orientation() return parts._orientation(self.instance) end
function Part:health() return parts._health(self.instance, 0) end
function Part:max_health() return parts._health(self.instance, 1) end
function Part:health_fraction() return parts._health(self.instance, 2) end
function Part:damage_level() return parts._health(self.instance, 3) end
function Part:set_health(h) parts._set_health(self.instance, h) end
function Part:repair() parts._set_health(self.instance, parts._health(self.instance, 1)) end
function Part:damage(amount) return parts._damage(self.instance, amount) end
function Part:break_off() parts._break(self.instance) end
function Part:press(control) parts._press(self.instance, control or 0) end
function Part:release(control) parts._release(self.instance, control or 0) end
-- The game's projectiles, by the names part:fire takes (asset ids of their objparams).
renut.projectiles = {
  egg = 0x1F164B76, eggbig = 0x1F9F0C6C, grenade = 0x1FA2A387, mumbobombo = 0x1FFAFDAA,
  laser = 0x1FBEB58D, citrusslick = 0x1F69280F, emp = 0x1F2A263B, clockworkkazooie = 0x1F8DDB30,
  weldarsbreath = 0x1F66F322, torpedile = 0x1FBDAD44, freezeezy = 0x1FD8A5EB, rustbin = 0x1F36049E,
}
local function projectile_aid(p)
  if math.type(p) == "integer" then return p end
  assert(type(p) == "string", "part:fire: projectile is a name (\"egg\") or an asset id")
  return renut.projectiles[p] or parts._projectile_aid(p)
end
-- options: { projectile = "mumbobombo" (default: a weapon's own), rotation = {x, y, z, w},
-- max_flying = 1-30 (default 10) }; a quaternion table alone is the rotation.
function Part:fire(position, velocity, options)
  options = options or {}
  local q = options.w and options or options.rotation or {x = 0, y = 0, z = 0, w = 1}
  local projectile
  if options.projectile then
    projectile = parts._fire_as(self.instance, projectile_aid(options.projectile), options.max_flying or 10,
                                position.x, position.y, position.z, velocity.x, velocity.y, velocity.z,
                                q.x, q.y, q.z, q.w)
  else
    projectile = parts._fire(self.instance, position.x, position.y, position.z,
                             velocity.x, velocity.y, velocity.z, q.x, q.y, q.z, q.w)
  end
  return projectile ~= 0 and projectile or nil
end
-- Whether part:fire with these options would launch one now (it readies them the first time).
function Part:can_fire(options)
  options = options or {}
  assert(options.projectile, "part:can_fire: give the projectile")
  return parts._can_fire_as(self.instance, projectile_aid(options.projectile), options.max_flying or 10)
end
function Part:hold(held) parts._hold(self.instance, held ~= false) end
-- The ammo each shot of the base weapon takes (what part:fire should spend with v:use("ammo", n)).
function Part:ammo_per_shot() return parts._turret(self.instance, 4) end
-- Where the weapon's barrel ends and points (as of its last tick), which its own shots start from;
-- nil before its first tick.
function Part:muzzle()
  local i = self.instance
  local x = parts._muzzle(i, 0, 0)
  if x ~= x then return nil end
  return vec(x, parts._muzzle(i, 0, 1), parts._muzzle(i, 0, 2)),
         vec(parts._muzzle(i, 1, 0), parts._muzzle(i, 1, 1), parts._muzzle(i, 1, 2)):normalized()
end
-- Its axes in the world: right, up, forward (unconfirmed).
function Part:axes()
  local i = self.instance
  local function row(n) return vec(parts._axis(i, n, 0), parts._axis(i, n, 1), parts._axis(i, n, 2)) end
  return row(0), row(1), row(2)
end
-- Turrets (weapons with a turning barrel): yaw (positive to the left) and pitch (positive down)
-- in radians from the part's forward, as the game turns them (seen in game).
function Part:aim_angles() return parts._turret(self.instance, 0), parts._turret(self.instance, 1) end
-- Bones of the part's model, by index (NutTool's model-export lists them in <model>.bones.txt).
function Part:bone_count() return parts._bone_count(self.instance) end
-- Turns bone `index` by `angle` radians about `axis` (in its parent's space; default x) at its joint,
-- in place of its pose, as the game turns a turret's barrel. Call it each frame (on_tick).
function Part:bone(index, angle, axis)
  local a = axis or vec(1, 0, 0)
  parts._bone(self.instance, index, angle, a.x, a.y, a.z)
end
-- seat_yaw_sign: -1 if the seat's animation turns the wrong way.
function Part:aim(yaw, pitch, seat_yaw_sign)
  parts._aim(self.instance, yaw, pitch, -(seat_yaw_sign or 1) * yaw)
end
-- signs: { yaw_sign, pitch_sign, seat_yaw_sign }, -1 for each that turns the wrong way.
function Part:aim_at(direction, signs)
  signs = signs or {}
  local r, u, f = self:axes()
  local x, y, z = direction:dot(r), direction:dot(u), direction:dot(f)
  self:aim(-(signs.yaw_sign or 1) * math.atan(x, z), -(signs.pitch_sign or 1) * math.atan(y, math.sqrt(x * x + z * z)),
           signs.seat_yaw_sign)
end

-- Effects ------------------------------------------------------------------------
function fx.play(effect, position, vehicle, seconds)
  if type(effect) == "number" then effect = string.format("0x%X", effect) end
  local where, x, y, z = where_of(position, "banjo")
  return fx._play(effect, where, x, y, z, vehicle and vehicle.address or 0, seconds or 0)
end

-- The camera ---------------------------------------------------------------------
local camera = renut.camera
local function camera_row(i) return vec(camera._get(i, 0), camera._get(i, 1), camera._get(i, 2)) end
function camera.right() return camera_row(0) end
function camera.up() return camera_row(1) end
-- The way it looks, once reNut has seen a gameplay frame (camera._facing); its matrix's third row before that.
function camera.forward()
  local facing = camera._facing()
  return facing < 0 and -camera_row(2) or camera_row(2)
end
function camera.facing_known() return camera._facing() ~= 0 end
function camera.position() return camera_row(3) end
function camera.fov() return camera._fov() end

-- The physics world --------------------------------------------------------------
local world = renut.world
-- What the line from 'from' to 'to' hits first in the vehicle's world: where, its normal and
-- how far along (0-1); nil when nothing.
function world.ray_cast(from, to, vehicle, past_vehicle)
  local f
  if vehicle then
    local cast = past_vehicle and world._ray_past or world._ray
    f = cast(vehicle.address, from.x, from.y, from.z, to.x, to.y, to.z)
  else
    f = world._ray_level(from.x, from.y, from.z, to.x, to.y, to.z)
  end
  if not (f < 1) then return nil end
  local from_v = vec(from.x, from.y, from.z)
  return from_v + (vec(to.x, to.y, to.z) - from_v) * f, vec(world._normal(0), world._normal(1), world._normal(2)), f
end
-- The ground under a place: the first thing a line down from just above it hits, within `reach` (default 50).
function world.ground(at, reach)
  local p = vec(at.x, at.y, at.z)
  return world.ray_cast(p + vec(0, 2, 0), p - vec(0, reach or 50, 0))
end

-- The mouse cursor ---------------------------------------------------------------
local mouse = renut.mouse
function mouse.position() return mouse._get(0), mouse._get(1) end
function mouse.inside() return mouse._get(3) ~= 0 end
function mouse.down(button) return rex.input.mouse_button(button or 0) end
-- The camera's position and the world direction under the cursor; the camera's forward is turned
-- towards 'toward' (the vehicle) when given, as its sign isn't confirmed.
function mouse.ray(toward)
  local from, f, r, u = camera.position(), camera.forward(), camera.right(), camera.up()
  if toward and f:dot(toward - from) < 0 then f = -f end
  local t = math.tan(math.rad(camera.fov()) / 2)
  local x, y = mouse.position()
  return from, (f + r * ((x * 2 - 1) * t * mouse._get(2)) + u * ((1 - y * 2) * t)):normalized()
end

-- Custom parts -------------------------------------------------------------------
local defs = {}     -- part id -> its renut.parts.add table
local owners = {}   -- part id -> the mod that added it
local stopped = {}  -- part id -> true once its callbacks failed too often
local errors = {}   -- part id -> callback errors so far
local MAX_ERRORS = 20
local CALLBACKS = {"on_spawn", "on_attach", "on_detach", "on_tick", "on_physics", "on_throttle", "on_use",
                   "on_release", "on_button_down", "on_button_up", "on_driver_in", "on_driver_out",
                   "on_damage", "on_break", "on_destroy", "on_collide"}

function parts._lua_def(id) return defs[id] end

-- A part's action button (button): its record's first control, +0x3AC (sub_82607A38), with the
-- default button it gets: 2 A, 3 B, 4 X (the spring, the horn, the egg gun); 0 none.
local BUTTON_CONTROL = 0x3AC
local BUTTONS = {A = 2, B = 3, X = 4}

-- The mod folder of the script 'level' calls up ("mods/<mod>/scripts/x.lua").
local function caller_mod(level)
  local source = debug.getinfo(level + 1, "S").source or ""
  return source:match(".*[/\\]([^/\\]+)[/\\]scripts[/\\]") or "lua"
end

function parts.add(def)
  assert(type(def) == "table", "renut.parts.add takes a table")
  assert(type(def.id) == "string", "renut.parts.add: the part needs an id")
  local id = def.id:lower()
  local mod = caller_mod(2)
  if parts._define(id, def.base or "", mod) then
    if not def.base then
      -- A part of its own: a block made from scratch, with its own model and footprint.
      assert(type(def.model) == "string", "renut.parts.add: a part without a base needs its model")
      assert(type(def.attach) == "string", "renut.parts.add: a part without a base needs its footprint (attach)")
      assert(type(def.category) == "string", "renut.parts.add: a part without a base needs its garage category, e.g. \"body\"")
      parts._block(id, def.category, tonumber(def.mass) or 10, tonumber(def.health) or 5, def.paint or "colour_orange")
    end
    if def.name then parts._name(id, tostring(def.name)) end
    if def.count then parts._count(id, def.count) end
    if def.model then parts._model(id, def.model) end
    if def.attach then parts._attach(id, def.attach) end
    if def.type then
      assert(type(def.type) == "string" and #def.type > 0 and #def.type <= 40,
             "renut.parts.add: type is a name of 1-40 characters")
      parts._type(id, def.type)
    end
    for offset, value in pairs(def.fields or {}) do
      local at = tonumber(offset)
      assert(at, "renut.parts.add: field offset '" .. tostring(offset) .. "' is not a number")
      local kind, v = next(value)
      if kind == "str" then parts._field_text(id, at, tostring(v)) else parts._field(id, at, kind, v) end
    end
    if def.button ~= nil then
      local control = def.button == false and 0 or BUTTONS[def.button]
      assert(control, "renut.parts.add: button is \"A\", \"B\", \"X\" or false")
      parts._field(id, BUTTON_CONTROL, "u32", control)
    end
    local behaviour = false
    for _, name in ipairs(CALLBACKS) do behaviour = behaviour or def[name] ~= nil end
    if behaviour then parts._behaviour(id, def.base_action == true) end
    parts._commit(id)
  end
  owners[id] = mod
  defs[id] = def
  stopped[id], errors[id] = nil, nil
  return id
end

function parts.list()
  local list = {}
  for i = 0, parts._count_all() - 1 do
    local id = parts._info(i, "id")
    list[#list + 1] = {id = id, mod = parts._info(i, "mod"), name = parts._info(i, "name"),
                       base = parts._info(i, "base"), lua = defs[id] ~= nil}
  end
  return list
end

function parts.get(instance) return instances[instance] end

-- Runs callback 'name' of part 'id''s table; one failing too often is stopped.
local function run(id, name, ...)
  local def = defs[id]
  local fn = def and def[name]
  if not fn or stopped[id] then return end
  local ok, err = xpcall(fn, debug.traceback, ...)
  if ok then return end
  errors[id] = (errors[id] or 0) + 1
  rex.error("[" .. owners[id] .. "] " .. id .. "." .. name .. ": " .. tostring(err))
  if errors[id] >= MAX_ERRORS then
    stopped[id] = true
    rex.error("[" .. owners[id] .. "] " .. id .. ": its callbacks are off after " .. MAX_ERRORS .. " errors")
  end
end

local function part_of(id, instance, vehicle)
  local part = instances[instance]
  if not part then
    local def = defs[id]
    part = setmetatable({id = id, instance = instance, params = def and def.params or {}}, Part)
    instances[instance] = part
  end
  if vehicle and vehicle ~= 0 then part._vehicle = vehicle end
  return part
end

rex.on("renut.part_spawn", function(id, instance)
  if defs[id] then run(id, "on_spawn", part_of(id, instance)) end
end)
rex.on("renut.part_attach", function(id, instance, vehicle)
  if defs[id] then run(id, "on_attach", part_of(id, instance, vehicle)) end
end)
rex.on("renut.part_detach", function(id, instance, vehicle)
  if not defs[id] then return end
  local part = part_of(id, instance)
  run(id, "on_detach", part)
  part._vehicle = nil
end)
rex.on("renut.part_tick", function(id, instance, vehicle, dt)
  local def = defs[id]
  if not def then return end
  local part = part_of(id, instance, vehicle)
  run(id, "on_tick", part, dt)
  if def.on_throttle and vehicle ~= 0 then
    run(id, "on_throttle", part, vehicles._input(vehicle, ACCELERATE) - vehicles._input(vehicle, BRAKE), dt)
  end
end)
rex.on("renut.part_physics", function(id, instance, vehicle, dt)
  if defs[id] then run(id, "on_physics", part_of(id, instance, vehicle), dt) end
end)
rex.on("renut.part_power", function(id, instance, vehicle, on)
  if defs[id] then run(id, on and "on_driver_in" or "on_driver_out", part_of(id, instance, vehicle)) end
end)
rex.on("renut.part_button", function(id, instance, vehicle, down)
  if not defs[id] then return end
  local part = part_of(id, instance, vehicle)
  if down then
    run(id, "on_use", part)
    run(id, "on_button_down", part)
  else
    run(id, "on_release", part)
    run(id, "on_button_up", part)
  end
end)
rex.on("renut.part_damage", function(instance, amount)
  local part = instances[instance]
  if part then run(part.id, "on_damage", part, amount) end
end)
rex.on("renut.part_break", function(instance)
  local part = instances[instance]
  if part then run(part.id, "on_break", part) end
end)
-- The frame's strongest hit on a vehicle: the part of yours it hit hears it.
rex.on("renut.vehicle_collide", function(vehicle, instance, strength, x, y, z, other)
  local part = instances[instance]
  if part then run(part.id, "on_collide", part, strength, vec(x, y, z), other ~= 0 and other or nil) end
end)
rex.on("renut.part_destroy", function(id, instance)
  if defs[id] then run(id, "on_destroy", part_of(id, instance)) end
  instances[instance] = nil
end)

-- Every part and vehicle -----------------------------------------------------------
-- renut.parts.on(event, fn) / renut.vehicle.on(event, fn): fn gets objects, not addresses.
local PART_EVENTS = {
  damage = function(fn) return function(instance, amount) fn(wrap_part(instance), amount) end end,
  ["break"] = function(fn) return function(instance) fn(wrap_part(instance)) end end,
}
PART_EVENTS.use = function(fn) return function(instance, control, down) if down then fn(wrap_part(instance), control) end end end
PART_EVENTS.release = function(fn) return function(instance, control, down) if not down then fn(wrap_part(instance), control) end end end
local PART_RAW = {use = "part_use", release = "part_use"}
function parts.on(name, fn)
  local wrap = assert(PART_EVENTS[name], "renut.parts.on: unknown event '" .. tostring(name) .. "'")
  return rex.on("renut." .. (PART_RAW[name] or "part_" .. name), wrap(fn))
end

local VEHICLE_EVENTS = {
  spawn = function(fn) return function(v) fn(vehicles.at(v)) end end,
  removed = function(fn) return function(v) fn(vehicles.at(v)) end end,
  tick = function(fn) return function(v, dt) fn(vehicles.at(v), dt) end end,
  damage = function(fn)
    return function(v, part, attacker, amount) fn(vehicles.at(v), amount, wrap_part(part), attacker) end
  end,
  health = function(fn) return function(v, health) fn(vehicles.at(v), health) end end,
  part_added = function(fn) return function(v, part) fn(vehicles.at(v), wrap_part(part)) end end,
  part_removed = function(fn) return function(v, part) fn(vehicles.at(v), wrap_part(part)) end end,
  collide = function(fn)
    return function(v, part, strength, x, y, z, other)
      fn(vehicles.at(v), strength, wrap_part(part), vec(x, y, z), other ~= 0 and other or nil)
    end
  end,
  enter = function(fn) return function(v, driver, player) fn(vehicles.at(v), driver, player) end end,
  exit = function(fn) return function(v, driver, player) fn(vehicles.at(v), driver, player) end end,
}
function vehicles.on(name, fn)
  local wrap = assert(VEHICLE_EVENTS[name], "renut.vehicle.on: unknown event '" .. tostring(name) .. "'")
  return rex.on("renut.vehicle_" .. name, wrap(fn))
end

-- The game's state ------------------------------------------------------------------
local game = renut.game
function game.world()
  local number = game._world()
  if number == 0 then return nil end
  return number, game._world_name(number), game._scene()
end
function game.challenge()
  local challenge = game._challenge()
  return challenge ~= 0 and challenge or nil
end
game.challenge_id = game._challenge_id
function game.challenge_start(challenge) game._challenge_start(challenge) end
function game.challenge_end(won) game._challenge_end(won ~= false) end
function game.challenge_time(limit, elapsed) game._challenge_time(limit, elapsed or 0) end
function game.challenge_hold(held) game._challenge_hold(held ~= false) end
function game.award(counter, amount, show) game._award(counter, amount or 1, show ~= false) end
function renut.worlds.go(id) renut.worlds._go(id) end
function renut.world.fog(def)
  local w = renut.world
  if def == nil then
    return {near = w._fog_get(0), far = w._fog_get(1), most = w._fog_get(2), colour = math.tointeger(w._fog_get(3))}
  end
  local now = w.fog()
  w._fog(def.near or now.near, def.far or now.far, def.most or now.most, def.colour or now.colour)
end
function renut.world.env(offset) return renut.world._env(offset) end
function renut.world.set_env(offset, value) renut.world._set_env(offset, value) end
game.paused = game._paused
game.flag = game._flag
function game.set_flag(name, on) game._set_flag(name, on ~= false) end
game.counter = game._counter
game.set_counter = game._set_counter
function game.add_counter(name, amount) game._set_counter(name, game._counter(name) + math.tointeger(amount)) end

local GAME_EVENTS = {
  world_enter = function(fn) return function(number, scene, name) fn(number, name, scene) end end,
  world_leave = function(fn) return function(number, scene, name) fn(number, name, scene) end end,
  challenge_start = function(fn) return function(challenge) fn(challenge) end end,
  challenge_finish = function(fn) return function(challenge, outcome) fn(challenge, outcome == 1, outcome) end end,
  paused = function(fn) return function() fn() end end,
  resumed = function(fn) return function() fn() end end,
  tick = function(fn) return function() fn() end end,
  save = function(fn) return function() fn() end end,
  load = function(fn) return function() fn() end end,
}
local GAME_RAW = {paused = "game_paused", resumed = "game_resumed", tick = "game_tick", save = "game_save",
                  load = "game_load"}
function game.on(name, fn)
  local wrap = assert(GAME_EVENTS[name], "renut.game.on: unknown event '" .. tostring(name) .. "'")
  return rex.on("renut." .. (GAME_RAW[name] or name), wrap(fn))
end

-- The player --------------------------------------------------------------------------
local player = renut.player
function player.avatar()
  local avatar = player._avatar()
  return avatar ~= 0 and avatar or nil
end
function player.vehicle()
  local v = player._vehicle()
  return v ~= 0 and vehicles.at(v) or nil
end
function player.teleport(at) player._teleport(at.x, at.y, at.z) end
player.play = player._play
function player.set_model(model) player._set_model(model or "") end

-- A mod's camera ------------------------------------------------------------------------
function camera.set(at, look_at)
  return camera._set(at.x, at.y, at.z, look_at.x, look_at.y, look_at.z)
end
camera.release = camera._release
function camera.set_fov(degrees) camera._set_fov(degrees or 0) end

-- Paint and mass -------------------------------------------------------------------------
-- 0xRRGGBB, a {r, g, b} table of 0-1, or a vector of them.
local function rgba(colour)
  if math.type(colour) == "integer" then return ((colour & 0xFFFFFF) << 8) | 0xFF end
  assert(type(colour) == "table", "a colour is 0xRRGGBB or {r, g, b} from 0 to 1")
  local function byte(v) return math.max(0, math.min(255, math.floor((v or 0) * 255 + 0.5))) end
  local r, g, b = colour.r or colour.x or colour[1], colour.g or colour.y or colour[2], colour.b or colour.z or colour[3]
  return (byte(r) << 24) | (byte(g) << 16) | (byte(b) << 8) | 0xFF
end
function Part:paint()
  local value = parts._paint(self.instance)
  return ((value >> 24) & 0xFF) / 255, ((value >> 16) & 0xFF) / 255, ((value >> 8) & 0xFF) / 255
end
function Part:set_paint(colour, effect) parts._set_paint(self.instance, rgba(colour), effect == true) end
function Vehicle:scale_mass(scale) vehicles._scale_mass(self.address, scale) end

-- Crates and the parts owned -------------------------------------------------------------
local crates = renut.crates
local function lines(text)
  local list = {}
  for line in text:gmatch("[^\n]+") do list[#list + 1] = line end
  return list
end
function crates.list() return lines(crates._list()) end
function crates.contents(flag)
  local contents = {}
  for _, line in ipairs(lines(crates._contents(flag))) do
    local aid, count = line:match("^(%d+):(%d+)$")
    contents[#contents + 1] = {aid = math.tointeger(tonumber(aid)), count = math.tointeger(tonumber(count))}
  end
  return contents
end
function crates.swap(flag, other) crates._swap(flag, other or "") end
function crates.give(flag, show) crates._give(flag, show ~= false) end
function crates.in_world()
  local list = {}
  for _, line in ipairs(lines(crates._in_world())) do
    local flag, x, y, z, object = line:match("^(.-);([^;]+);([^;]+);([^;]+);(%d+)$")
    if flag then
      list[#list + 1] = {flag = flag, position = vec(tonumber(x), tonumber(y), tonumber(z)), object = math.tointeger(tonumber(object))}
    end
  end
  return list
end
function crates.spawn(flag, at)
  local where, x, y, z = where_of(at, "front")
  local object = crates._spawn(flag, where, x, y, z)
  return object ~= 0 and object or nil
end
function crates.on(name, fn)
  assert(name == "award", "renut.crates.on: the event is \"award\"")
  return rex.on("renut.crate_award", function(flag) fn(flag, crates.contents(flag)) end)
end

-- Objects and characters ---------------------------------------------------------------
-- Each calls its _ function by name, so a call from outside the renut events waits for the next game tick.
local objects, actors = renut.objects, renut.actors
local function objects_of(text)
  local list = {}
  for _, line in ipairs(lines(text)) do
    local object, kind, x, y, z = line:match("^(%d+);(.-);([^;]+);([^;]+);([^;]+)$")
    if object then
      list[#list + 1] = {object = math.tointeger(tonumber(object)), kind = kind, position = vec(tonumber(x), tonumber(y), tonumber(z))}
    end
  end
  return list
end
function objects.kinds()
  local list = {}
  for _, line in ipairs(lines(objects._kinds())) do
    local name, class = line:match("^(.-);(.*)$")
    if name then list[#list + 1] = {name = name, class = class} end
  end
  return list
end
function objects.list() return objects_of(objects._list()) end
function objects.spawn(kind, at, yaw)
  local where, x, y, z = where_of(at, "front")
  local object = objects._spawn(kind, where, x, y, z, yaw or 0)
  return object ~= 0 and object or nil
end
function objects.remove(object) objects._remove(object) end
function objects.on(event, fn)
  if event == "spawn" then return rex.on("renut.object_spawn", fn) end
  if event == "removed" then return rex.on("renut.object_removed", fn) end
  assert(event == "collected", "renut.objects.on: the events are \"collected\", \"spawn\" and \"removed\"")
  return rex.on("renut.collected", fn)
end
function objects.kind(object) return objects._kind(object) end
function objects.is_character(object) return objects._is_character(object) end
function objects.position(object)
  return vec(objects._position(object, 0), objects._position(object, 1), objects._position(object, 2))
end

-- A character as the _ functions take it: a character (object or number), or who (actors.Who) as text.
local function who_of(actor)
  if actor == nil or type(actor) == "string" then return 0, actor or "" end
  return actor, ""
end
function actors.list() return objects_of(actors._list()) end
function actors.find(who)
  local ok, actor = pcall(actors._find, who or "")
  return ok and actor or nil
end
function actors.spawn(character, at, yaw)
  local where, x, y, z = where_of(at, "front")
  local object = actors._spawn(character, where, x, y, z, yaw or 0)
  return object ~= 0 and object or nil
end
function actors.place(actor, at)
  local a, w = who_of(actor)
  local where, x, y, z = where_of(at, "banjo")
  actors._place(a, w, where, x, y, z)
end
function actors.turn(actor, yaw) local a, w = who_of(actor) actors._turn(a, w, yaw) end
function actors.play(actor, animation) local a, w = who_of(actor) actors._play(a, w, animation) end
function actors.state(actor) local a, w = who_of(actor) return actors._state(a, w) end
function actors.set_state(actor, state) local a, w = who_of(actor) actors._set_state(a, w, state) end
function actors.body_state(actor, state) local a, w = who_of(actor) return actors._body_state(a, w, state) end
function actors.action(actor, action) local a, w = who_of(actor) return actors._action(a, w, action) end
function actors.walk_to(actor, at)
  local a, w = who_of(actor)
  local where, x, y, z = where_of(at, "banjo")
  actors._walk_to(a, w, where, x, y, z)
end
function actors.stop(actor) local a, w = who_of(actor) actors._stop(a, w) end
function actors.remove(actor) local a, w = who_of(actor) actors._remove(a, w) end
function actors.vehicle(actor)
  local a, w = who_of(actor)
  local vehicle = actors._vehicle(a, w)
  return vehicle ~= 0 and renut.vehicle.at(vehicle) or nil
end
function actors.talk(actor, dialog) local a, w = who_of(actor) return actors._talk(a, w, dialog) end
function actors.health(actor)
  local a, w = who_of(actor)
  return actors._health(a, w, 0), actors._health(a, w, 1)
end
function actors.set_health(actor, health) local a, w = who_of(actor) actors._set_health(a, w, health) end
function actors.damage(actor, amount) local a, w = who_of(actor) return actors._damage(a, w, amount) end
function actors.defeat(actor) local a, w = who_of(actor) actors._defeat(a, w) end
function actors.follow(actor, target)
  local a, w = who_of(actor)
  local t, tw = who_of(target or "banjo")
  if type(target) == "table" and target.address then t = target.address end
  actors._follow(a, w, t, tw)
end
function actors.unfollow(actor) local a, w = who_of(actor) actors._unfollow(a, w) end
function actors.target(actor)
  local a, w = who_of(actor)
  local t = actors._target(a, w)
  return t ~= 0 and t or nil
end
function actors.set_model(actor, model) local a, w = who_of(actor) actors._model(a, w, model) end
function actors.on(event, fn)
  if event == "spawn" then
    return rex.on("renut.object_spawn", function(object, kind, character)
      if character then fn(object, kind) end
    end)
  end
  assert(event == "hit" or event == "defeated", "renut.actors.on: the events are \"hit\", \"defeated\" and \"spawn\"")
  return rex.on("renut.actor_" .. event, fn)
end
function renut.npcs.spawn(world, id, at, yaw)
  local where, x, y, z = where_of(at, "front")
  local object = renut.npcs._spawn(world, id, where, x, y, z, yaw or 0)
  return object ~= 0 and object or nil
end

-- Whole vehicles ----------------------------------------------------------------------
local SEATS = {driver = 2, passenger = 4, other = 8}
function vehicles.spawn(blueprint, at, yaw)
  local where, x, y, z = where_of(at, "front")
  local v = vehicles._spawn(blueprint, where, x, y, z, yaw or 0)
  return v and v ~= 0 and renut.vehicle.at(v) or nil
end
function vehicles.seat(v, actor, seat)
  local a, w = who_of(actor or "banjo")
  vehicles._seat(type(v) == "table" and v.address or v, a, w, SEATS[seat or "driver"] or seat)
end

-- The HUD ---------------------------------------------------------------------------
local hud = renut.hud
local choices = {}
function hud.message(text, seconds) hud._message(tostring(text), seconds or 3) end
function hud.close() hud._close() end
function hud.choice(text, fn)
  local id = hud._choice(tostring(text))
  if id and fn then choices[id] = fn end
  return id
end
rex.on("renut.hud_choice", function(id, confirmed)
  local fn = choices[id]
  choices[id] = nil
  if fn then fn(confirmed) end
end)
-- The panel's parts are chosen as it's shown: hud.panel{timer = true, counter = true} for both.
local function panel_with(timer, score)
  local shown = hud._shown()
  if shown == 0 then hud._panel(timer, score) return end
  if (timer and shown & 1 == 0) or (score and shown & 2 == 0) then
    error("the panel is up without that part: hide it, then show it with both (renut.hud.panel)", 3)
  end
end
function hud.timer(seconds, run)
  hud._timer(seconds or 0, run or "down")
  panel_with(true, false)
end
function hud.counter(value, target)
  hud._counter(value or 0, target or 0)
  panel_with(false, true)
end
function hud.panel(def) hud._panel(def.timer == true, def.counter == true) end
function hud.set_timer(seconds, run) hud._timer(seconds, run or "down") end
function hud.set_counter(value, target) hud._counter(value, target or 0) end
function hud.time() return hud._time() end
function hud.hide() hud._panel_close() end
function hud.on(event, fn)
  assert(event == "timer_done" or event == "choice", "renut.hud.on: the events are \"timer_done\" and \"choice\"")
  return rex.on("renut.hud_" .. event, fn)
end

-- Trigger zones ---------------------------------------------------------------------
local zones = renut.zones
function zones.add(def)
  assert(type(def) == "table" and def.name, "renut.zones.add{name = ..., at = ..., radius or size = ...}")
  local where, x, y, z = where_of(def.at, "banjo")
  local box = def.size ~= nil
  local w, h, d = 0, 0, 0
  if box then w, h, d = def.size.x, def.size.y, def.size.z end
  local watch = def.watch or "banjo"
  if type(watch) == "table" then watch = table.concat(watch, ",") end
  zones._add(def.name, box and "box" or "sphere", where, x, y, z, def.radius or 0, w, h, d, watch, def.world or "")
  return def.name
end
function zones.move(name, at)
  local where, x, y, z = where_of(at, "banjo")
  zones._move(name, where, x, y, z)
end
function zones.remove(name) zones._remove(name) end
function zones.list() return lines(zones._list()) end
function zones.count(name) return zones._count(name) end
function zones.inside(name, object) return zones._inside(name, object or 0) end
function zones.on(event, fn)
  assert(event == "enter" or event == "exit", "renut.zones.on: the events are \"enter\" and \"exit\"")
  return rex.on("renut.zone_" .. event, fn)
end

parts.aid = parts._aid_of
function parts.owned(part)
  local aid = type(part) == "string" and parts._aid_of(part) or part
  return parts._owned(aid, false), parts._owned(aid, true)
end
)lua";

// A renut._ function called outside the renut events (a key press) that needs one waits for the next game_tick. Every
// rex.on callback runs as a coroutine, so the callback itself pauses there and goes on with the real result.
constexpr const char kWaitLua[] = R"lua(
local waiting = {}  -- in order: {co = paused callback} or {call = table.pack(fn, ...)}
local ours = setmetatable({}, {__mode = "k"})
local in_event = renut.game._in_event
local OUTSIDE = "works inside the renut events"
local DONE = {}
local function wait_for_game(fn)
  return function(...)
    if in_event() then return fn(...) end
    local result = table.pack(pcall(fn, ...))
    if result[1] then return table.unpack(result, 2, result.n) end
    if type(result[2]) == "string" and result[2]:find(OUTSIDE, 1, true) then
      local co = coroutine.running()
      if ours[co] and coroutine.isyieldable() then
        coroutine.yield()
        return fn(...)
      end
      -- Not in a callback that can pause (inside a sort's comparer, a mod's own coroutine): just this call waits.
      waiting[#waiting + 1] = {call = table.pack(fn, ...)}
      return nil
    end
    error(result[2], 2)
  end
end
local function wrap(space)
  for name, fn in pairs(space) do
    if type(name) == "string" and name:sub(1, 1) == "_" and type(fn) == "function" then space[name] = wait_for_game(fn) end
  end
end
wrap(renut)
for _, space in pairs(renut) do
  if type(space) == "table" then wrap(space) end
end

-- Runs or resumes a callback's coroutine: true when it finished, false when it paused for the game.
local function step(co, ...)
  local ok, result = coroutine.resume(co, ...)
  if not ok then error(debug.traceback(co, tostring(result)), 0) end
  return result == DONE
end
-- What rex.on, the renut .on functions and the timers return: h:off() stops it (RexFabric keeps the listener, idle).
local Handle = {}
Handle.__index = Handle
function Handle:off() self.dead = true end
function Handle:active() return not self.dead end
function renut.off(handle) if handle then handle:off() end end

-- Runs fn(...) as a callback coroutine that may pause (renut.wait, a call waiting for the game).
local function start(fn, ...)
  local co = coroutine.create(function(...) fn(...) return DONE end)
  ours[co] = true
  if not step(co, ...) then waiting[#waiting + 1] = {co = co} end
end

local on = rex.on
function rex.on(name, fn)
  local handle = setmetatable({event = name}, Handle)
  local idle  -- a finished coroutine kept for the next call, as "frame" calls every frame
  on(name, function(...)
    if handle.dead then return end
    local co = idle
    idle = nil
    if not co then
      co = coroutine.create(function(...)
        local args = table.pack(...)
        while true do
          fn(table.unpack(args, 1, args.n))
          args = table.pack(coroutine.yield(DONE))
        end
      end)
      ours[co] = true
    end
    if step(co, ...) then idle = co else waiting[#waiting + 1] = {co = co} end
  end)
  return handle
end

-- RexFabric's timers (rex.after, rex.every: the node editor's Delay and timers) run their functions as callbacks too,
-- so a call into the game after a Delay waits for it and gives back its real result.
if rex.after and rex.every then
  local after, every = rex.after, rex.every
  function rex.after(seconds, fn)
    assert(type(fn) == "function", "a timer runs a function")
    return after(seconds, function(...) start(fn, ...) end)
  end
  function rex.every(seconds, fn)
    assert(type(fn) == "function", "a timer runs a function")
    return every(seconds, function(...) start(fn, ...) end)
  end
end

-- renut.once(renut.game, "world_enter", fn): the first time only.
function renut.once(space, event, fn)
  assert(type(space) == "table" and type(space.on) == "function", "renut.once(space, event, fn): space is e.g. renut.game")
  local handle
  handle = space.on(event, function(...)
    handle:off()
    return fn(...)
  end)
  return handle
end

-- Timers count the game's own time: game ticks, not while paused (a tick's gap is at most 0.1 s).
local clock, last_tick = 0, nil
local timers = {}  -- in order: {handle, at, every, fn}
local function add_timer(seconds, every, fn)
  assert(type(seconds) == "number" and seconds >= 0, "a timer's seconds are 0 or more")
  assert(type(fn) == "function", "a timer needs a function")
  local handle = setmetatable({}, Handle)
  timers[#timers + 1] = {handle = handle, at = clock + seconds, every = every and math.max(seconds, 0.001), fn = fn}
  return handle
end
function renut.after(seconds, fn) return add_timer(seconds, false, fn) end
function renut.every(seconds, fn) return add_timer(seconds, true, fn) end
function renut.time() return clock end
-- Pauses the callback it's called from for that long, in game time.
function renut.wait(seconds)
  assert(type(seconds) == "number" and seconds >= 0, "renut.wait: seconds are 0 or more")
  local co = coroutine.running()
  if not (ours[co] and coroutine.isyieldable()) then
    error("renut.wait works inside a renut callback (an event's, a timer's)", 2)
  end
  local until_ = clock + seconds
  repeat coroutine.yield() until clock >= until_
end

on("renut.game_tick", function()
  local t = rex.now()
  if last_tick and not renut.game._paused() then clock = clock + math.min(t - last_tick, 0.1) end
  last_tick = t
  local due = {}
  for i = #timers, 1, -1 do
    local timer = timers[i]
    if timer.handle.dead then
      table.remove(timers, i)
    elseif clock >= timer.at then
      due[#due + 1] = timer
      if timer.every then timer.at = clock + timer.every else table.remove(timers, i) end
    end
  end
  local now = waiting
  waiting = {}
  local first_error
  for i = #due, 1, -1 do
    local ok, err = pcall(start, due[i].fn)
    if not ok and not first_error then first_error = err end
  end
  for _, entry in ipairs(now) do
    local ok, err = pcall(function()
      if entry.co then
        if not step(entry.co) then waiting[#waiting + 1] = entry end
      else
        entry.call[1](table.unpack(entry.call, 2, entry.call.n))
      end
    end)
    if not ok and not first_error then first_error = err end
  end
  if first_error then error(first_error, 0) end
end)
)lua";

} // namespace

void Declare() {
  rexfabric::Namespace renut("renut");
  // The node editor's add menu, by what each thing acts on.
  renut.Category("parts.", "Parts").Category("part_", "Parts");
  renut.Category("vehicle.", "Vehicles").Category("vehicle_", "Vehicles").Category("set_amount", "Vehicles");
  renut.Category("fx.", "Effects").Category("camera.", "Camera").Category("mouse.", "Mouse");
  renut.Category("world.", "Physics");
  renut.Category("world._fog", "World look").Category("world._env", "World look").Category("world._set_env", "World look");
  renut.Category("collected", "Objects").Category("object_", "Objects").Category("pad.", "Pad");
  renut.Category("player_", "Characters");
  // Parts and vehicles as objects for the node editor's pins ("part:Part"), so a part goes where a part is asked for.
  renut.Type("Part", 0x400);
  renut.Type("Vehicle", 0x1900);
  renut.Type("Character", 0xD00);
  renut.Category("Part", "Parts").Category("Vehicle", "Vehicles").Category("Character", "Characters");
  // Where things go, so a spawn or a move needs no position: the first of each list is a pin's default.
  renut.Choices("objects.Where", {{"In front of Banjo", "front"}, {"At Banjo", "banjo"}, {"At X, Y, Z", "xyz"}});
  renut.Choices("fx.Where", {{"At Banjo", "banjo"}, {"In front of Banjo", "front"}, {"At X, Y, Z", "xyz"}});
  renut.Choices("actors.To", {{"Banjo", "banjo"}, {"In front of Banjo", "front"}, {"X, Y, Z", "xyz"}});
  // The node editor's pins: each function's parameters and each event's values, in order.
  using Names = std::vector<std::string>;
  const Names fired_at = {"position_x", "position_y", "position_z", "velocity_x", "velocity_y", "velocity_z",
                          "rotation_x", "rotation_y", "rotation_z", "rotation_w"};
  Names fire = {"part:Part"}, fire_as = {"part:Part", "projectile", "size"};
  fire.insert(fire.end(), fired_at.begin(), fired_at.end());
  fire_as.insert(fire_as.end(), fired_at.begin(), fired_at.end());
  for (const auto &[name, args] : std::initializer_list<std::pair<const char *, Names>>{
           {"parts._define", {"id", "base", "mod"}},
           {"parts._name", {"id", "name"}},
           {"parts._count", {"id", "count"}},
           {"parts._model", {"id", "model"}},
           {"parts._attach", {"id", "attach"}},
           {"parts._field", {"id", "offset", "type", "value"}},
           {"parts._field_text", {"id", "offset", "text"}},
           {"parts._type", {"id", "type"}},
           {"parts._block", {"id", "category", "mass", "health", "paint"}},
           {"parts._behaviour", {"id", "base_action"}},
           {"parts._commit", {"id"}},
           {"parts._info", {"index", "key"}},
           {"parts._vehicle_of", {"part:Part", "->Vehicle"}},
           {"parts._class", {"part:Part"}},
           {"parts._is_a", {"part:Part", "class_name"}},
           {"parts._aid", {"part:Part"}},
           {"parts._id_of", {"part:Part"}},
           {"parts._position", {"part:Part", "axis"}},
           {"parts._cell", {"part:Part", "axis"}},
           {"parts._orientation", {"part:Part"}},
           {"parts._health", {"part:Part", "which"}},
           {"parts._set_health", {"part:Part", "health"}},
           {"parts._damage", {"part:Part", "amount"}},
           {"parts._break", {"part:Part"}},
           {"parts._press", {"part:Part", "control"}},
           {"parts._release", {"part:Part", "control"}},
           {"parts._fire", fire},
           {"parts._fire_as", fire_as},
           {"parts._can_fire_as", {"part:Part", "projectile", "size"}},
           {"parts._projectile_aid", {"name"}},
           {"parts._hold", {"part:Part", "held"}},
           {"parts._axis", {"part:Part", "row", "axis"}},
           {"parts._turret", {"part:Part", "which"}},
           {"parts._muzzle", {"part:Part", "which", "axis"}},
           {"parts._aim", {"part:Part", "yaw", "pitch", "drawn_yaw"}},
           {"parts._shape_index", {"part:Part", "name"}},
           {"parts._shape", {"part:Part", "index"}},
           {"parts._set_shape", {"part:Part", "index", "weight"}},
           {"parts._bone_count", {"part:Part"}},
           {"parts._bone", {"part:Part", "bone", "angle", "axis_x", "axis_y", "axis_z"}},
           {"vehicle._has_body", {"vehicle:Vehicle"}},
           {"vehicle._get", {"vehicle:Vehicle", "what", "axis"}},
           {"vehicle._set", {"vehicle:Vehicle", "what", "x", "y", "z"}},
           {"vehicle._inverse_mass", {"vehicle:Vehicle"}},
           {"vehicle._force", {"vehicle:Vehicle", "dt", "x", "y", "z"}},
           {"vehicle._force_at", {"vehicle:Vehicle", "dt", "x", "y", "z", "at_x", "at_y", "at_z"}},
           {"vehicle._impulse_at", {"vehicle:Vehicle", "x", "y", "z", "at_x", "at_y", "at_z"}},
           {"vehicle._angular_impulse", {"vehicle:Vehicle", "x", "y", "z"}},
           {"vehicle._input", {"vehicle:Vehicle", "which"}},
           {"vehicle._driver", {"vehicle:Vehicle"}},
           {"vehicle._health", {"vehicle:Vehicle", "which"}},
           {"vehicle._set_health", {"vehicle:Vehicle", "health"}},
           {"vehicle._repair", {"vehicle:Vehicle"}},
           {"vehicle._damage", {"vehicle:Vehicle", "amount", "part", "attacker"}},
           {"vehicle._disabled", {"vehicle:Vehicle"}},
           {"vehicle._invulnerable", {"vehicle:Vehicle"}},
           {"vehicle._set_invulnerable", {"vehicle:Vehicle", "on"}},
           {"vehicle._resource", {"vehicle:Vehicle", "kind", "which"}},
           {"vehicle._set_resource", {"vehicle:Vehicle", "kind", "value"}},
           {"vehicle._set_infinite", {"vehicle:Vehicle", "kind", "on"}},
           {"vehicle._use", {"vehicle:Vehicle", "kind", "amount"}},
           {"vehicle._name", {"vehicle:Vehicle"}},
           {"vehicle._level", {"vehicle:Vehicle"}},
           {"vehicle._part_count", {"vehicle:Vehicle"}},
           {"vehicle._part", {"vehicle:Vehicle", "index", "->Part"}},
           {"vehicle._in_air", {"vehicle:Vehicle", "reach"}},
           {"camera._get", {"row", "axis"}},
           {"world._ray", {"vehicle:Vehicle", "from_x", "from_y", "from_z", "to_x", "to_y", "to_z"}},
           {"world._ray_past", {"vehicle:Vehicle", "from_x", "from_y", "from_z", "to_x", "to_y", "to_z"}},
           {"world._normal", {"axis"}},
           {"mouse._get", {"which"}},
           {"fx._play", {"effect:fx.Effect", "at:fx.Where", "x", "y", "z", "vehicle:Vehicle?", "seconds"}},
           {"set_amount", {"amount"}},
           {"part_spawn", {"id", "part:Part"}},
           {"part_destroy", {"id", "part:Part"}},
           {"part_attach", {"id", "part:Part", "vehicle:Vehicle"}},
           {"part_detach", {"id", "part:Part", "vehicle:Vehicle"}},
           {"part_tick", {"id", "part:Part", "vehicle:Vehicle", "dt"}},
           {"part_physics", {"id", "part:Part", "vehicle:Vehicle", "dt"}},
           {"part_power", {"id", "part:Part", "vehicle:Vehicle", "on"}},
           {"part_button", {"id", "part:Part", "vehicle:Vehicle", "down"}},
           {"part_damage", {"part:Part", "amount"}},
           {"part_break", {"part:Part"}},
           {"vehicle_spawn", {"vehicle:Vehicle"}},
           {"vehicle_removed", {"vehicle:Vehicle"}},
           {"vehicle_tick", {"vehicle:Vehicle", "dt"}},
           {"vehicle_damage", {"vehicle:Vehicle", "part:Part", "attacker", "amount"}},
           {"vehicle_health", {"vehicle:Vehicle", "health"}},
           {"vehicle_part_added", {"vehicle:Vehicle", "part:Part"}},
           {"vehicle_part_removed", {"vehicle:Vehicle", "part:Part"}},
           {"vehicle_collide", {"vehicle:Vehicle", "part:Part", "strength", "x", "y", "z", "other"}},
           {"vehicle_enter", {"vehicle:Vehicle", "driver", "player"}},
           {"vehicle_exit", {"vehicle:Vehicle", "driver", "player"}},
           {"world_enter", {"number", "scene", "name"}},
           {"world_leave", {"number", "scene", "name"}},
           {"challenge_start", {"challenge"}},
           {"challenge_finish", {"challenge", "outcome"}},
           {"game_paused", {}},
           {"game_resumed", {}},
           {"game_tick", {}},
           {"game._in_event", {}},
           {"game._world", {}},
           {"game._world_name", {"number"}},
           {"game._scene", {}},
           {"game._challenge", {}},
           {"game._challenge_id", {"name"}},
           {"game._challenge_start", {"challenge"}},
           {"worlds._go", {"world:worlds.Id"}},
           {"game._challenge_end", {"won"}},
           {"game._challenge_time", {"limit", "elapsed"}},
           {"game._challenge_hold", {"held"}},
           {"game._award", {"counter:game.Counter", "amount", "show"}},
           {"world._fog", {"near", "far", "most", "colour"}},
           {"world._fog_get", {"which"}},
           {"world._env", {"offset"}},
           {"world._set_env", {"offset", "value"}},
           {"collected", {"object", "kind"}},
           {"game._paused", {}},
           {"game._flag", {"name:game.Flag"}},
           {"game._set_flag", {"name:game.Flag", "on"}},
           {"game._counter", {"name:game.Counter"}},
           {"game._set_counter", {"name:game.Counter", "value"}},
           {"player._avatar", {}},
           {"player._vehicle", {"->Vehicle"}},
           {"player._teleport", {"x", "y", "z"}},
           {"player._play", {"animation"}},
           {"player._set_model", {"model"}},
           {"camera._set", {"x", "y", "z", "look_x", "look_y", "look_z"}},
           {"camera._release", {}},
           {"camera._set_fov", {"degrees"}},
           {"parts._paint", {"part:Part"}},
           {"parts._set_paint", {"part:Part", "rgba", "effect"}},
           {"vehicle._scale_mass", {"vehicle:Vehicle", "scale"}},
           {"crate_award", {"flag"}},
           {"crates._list", {}},
           {"crates._contents", {"flag:crates.Flag"}},
           {"crates._swap", {"flag:crates.Flag", "other:crates.Flag"}},
           {"crates._give", {"flag:crates.Flag", "show"}},
           {"crates._in_world", {}},
           {"crates._spawn", {"flag:crates.Flag", "at:objects.Where", "x", "y", "z"}},
           {"objects._kinds", {}},
           {"objects._list", {}},
           {"objects._kind", {"object"}},
           {"objects._position", {"object", "axis"}},
           {"objects._is_character", {"object"}},
           {"objects._spawn", {"kind:objects.Kind", "at:objects.Where", "x", "y", "z", "yaw"}},
           {"objects._remove", {"object"}},
           {"actors._list", {}},
           {"actors._find", {"who:actors.Who", "->Character"}},
           {"actors._spawn", {"character:actors.Character", "at:objects.Where", "x", "y", "z", "yaw", "->Character"}},
           {"actors._place", {"character:Character?", "who:actors.Who", "at:actors.To", "x", "y", "z"}},
           {"actors._turn", {"character:Character?", "who:actors.Who", "yaw"}},
           {"actors._play", {"character:Character?", "who:actors.Who", "animation"}},
           {"actors._state", {"character:Character?", "who:actors.Who"}},
           {"actors._set_state", {"character:Character?", "who:actors.Who", "state:actors.State"}},
           {"actors._body_state", {"character:Character?", "who:actors.Who", "state:actors.BodyState"}},
           {"actors._action", {"character:Character?", "who:actors.Who", "action"}},
           {"actors._walk_to", {"character:Character?", "who:actors.Who", "to:actors.To", "x", "y", "z"}},
           {"actors._stop", {"character:Character?", "who:actors.Who"}},
           {"actors._remove", {"character:Character?", "who:actors.Who"}},
           {"actors._vehicle", {"character:Character?", "who:actors.Who", "->Vehicle"}},
           {"actors._talk", {"character:Character?", "who:actors.Who", "dialog"}},
           {"actors._health", {"character:Character?", "who:actors.Who", "which:actors.Health"}},
           {"actors._set_health", {"character:Character?", "who:actors.Who", "health"}},
           {"actors._damage", {"character:Character?", "who:actors.Who", "amount"}},
           {"actors._defeat", {"character:Character?", "who:actors.Who"}},
           {"actors._follow", {"character:Character?", "who:actors.Who", "target", "target_who:actors.Who"}},
           {"actors._unfollow", {"character:Character?", "who:actors.Who"}},
           {"actors._target", {"character:Character?", "who:actors.Who", "->Character"}},
           {"actors._model", {"character:Character?", "who:actors.Who", "model"}},
           {"actor_hit", {"character:Character", "attacker", "damage"}},
           {"actor_defeated", {"character:Character", "killer"}},
           {"npcs._spawn", {"world:npcs.World", "id:npcs.Id", "at:objects.Where", "x", "y", "z", "yaw", "->Character"}},
           {"vehicle._spawn", {"blueprint:vehicle.Blueprint", "at:objects.Where", "x", "y", "z", "yaw", "->Vehicle"}},
           {"vehicle._seat", {"vehicle:Vehicle", "character:Character?", "who:actors.Who", "seat:vehicle.Seat"}},
           {"hud._message", {"text", "seconds"}},
           {"hud._close", {}},
           {"hud._choice", {"text"}},
           {"hud._panel", {"timer", "score"}},
           {"hud._panel_close", {}},
           {"hud._timer", {"seconds", "run:hud.Timer"}},
           {"hud._time", {}},
           {"hud._shown", {}},
           {"hud._counter", {"value", "target"}},
           {"hud_choice", {"choice", "confirmed"}},
           {"hud_timer_done", {}},
           {"zones._add", {"name", "shape:zones.Shape", "at:fx.Where", "x", "y", "z", "radius", "width", "height",
                           "depth", "watch:zones.Watch", "world:zones.World"}},
           {"zones._move", {"name:zones.Zone", "at:fx.Where", "x", "y", "z"}},
           {"zones._remove", {"name:zones.Zone"}},
           {"zones._list", {}},
           {"zones._count", {"name:zones.Zone"}},
           {"zones._inside", {"name:zones.Zone", "object"}},
           {"zone_enter", {"zone", "object", "what"}},
           {"zone_exit", {"zone", "object", "what"}},
           {"parts._aid_of", {"name"}},
           {"parts._owned", {"aid", "available"}},
           {"world._ray_level", {"from_x", "from_y", "from_z", "to_x", "to_y", "to_z"}},
           {"camera._facing", {}},
           {"camera._shake", {"strength", "seconds"}},
           {"player._avatar_of", {"player", "->Character"}},
           {"actors._velocity", {"character:Character?", "who:actors.Who", "axis"}},
           {"actors._push", {"character:Character?", "who:actors.Who", "x", "y", "z"}},
           {"actors._on_ground", {"character:Character?", "who:actors.Who"}},
           {"actors._scale", {"character:Character?", "who:actors.Who"}},
           {"actors._exit_vehicle", {"character:Character?", "who:actors.Who"}},
           {"actors._yaw", {"character:Character?", "who:actors.Who"}},
           {"actors._body_now", {"character:Character?", "who:actors.Who"}},
           {"objects._move", {"object", "x", "y", "z"}},
           {"objects._turn", {"object", "yaw", "pitch", "roll"}},
           {"objects._yaw", {"object"}},
           {"vehicle._list", {}},
           {"vehicle._blueprints", {}},
           {"vehicle._remove", {"vehicle:Vehicle"}},
           {"vehicle._rotation", {"vehicle:Vehicle", "which"}},
           {"vehicle._set_rotation", {"vehicle:Vehicle", "x", "y", "z", "w"}},
           {"vehicle._set_name", {"vehicle:Vehicle", "name"}},
           {"game._save", {}},
           {"game._set_paused", {"paused"}},
           {"game._set_time_scale", {"scale"}},
           {"game._time_scale", {}},
           {"sound._cue", {"cue", "volume"}},
           {"sound._cue_stop", {"cue"}},
           {"sound._cue_playing", {"cue"}},
           {"pad._get", {"player", "which"}},
           {"player_state", {"state", "old"}},
           {"game_save", {}},
           {"game_load", {}},
           {"part_use", {"part:Part", "control", "down"}},
           {"object_spawn", {"object", "kind", "character"}},
           {"object_removed", {"object", "kind"}}})
    renut.Args(name, args);

  // Adding parts (renut.parts.add).
  renut.Function("parts._define", Define);
  renut.Function("parts._name", [](std::string id, std::string name) { Pending(id).name = std::move(name); });
  renut.Function("parts._count", [](std::string id, int64_t count) {
    Pending(id).count = uint32_t(count < 1 ? 1 : count > 99 ? 99 : count);
  });
  renut.Function("parts._model", [](std::string id, std::string model) {
    Pending(id).model = renut::mods::PartModelName(std::move(model));
  });
  renut.Function("parts._attach", [](std::string id, std::string attach) {
    Pending(id).attach = renut::mods::PartAttachName(std::move(attach));
  });
  renut.Function("parts._field", [](std::string id, int64_t offset, std::string type, double value) {
    AddField(id, offset, FieldBytes(type, value));
  });
  renut.Function("parts._field_text", [](std::string id, int64_t offset, std::string text) {
    std::vector<uint8_t> bytes(text.begin(), text.end());
    bytes.push_back(0);
    AddField(id, offset, std::move(bytes));
  });
  renut.Function("parts._type", [](std::string id, std::string type) { Pending(id).garage_type = std::move(type); });
  renut.Function("parts._block", [](std::string id, std::string category, double mass, double health, std::string paint) {
    CustomPart &part = Pending(id);
    part.objparams = BlockRecord(category, mass, health, paint);
    part.own_record = true;
  });
  renut.Function("parts._behaviour", [](std::string id, bool base_action) {
    CustomPart &part = Pending(id);
    part.lua = true;
    part.base_action = base_action;
  });
  renut.Function("parts._commit", [](std::string id) {
    CustomPart part = std::move(Pending(id));
    g_pending.erase(id);
    std::vector<CustomPart> one;
    one.push_back(std::move(part));
    renut::mods::AddCustomParts(std::move(one));
  });
  renut.Function("parts._count_all", [] { return int64_t(renut::mods::CustomParts().size()); });
  renut.Function("parts._info", [](int64_t index, std::string key) {
    const auto &all = renut::mods::CustomParts();
    if (index < 0 || size_t(index) >= all.size())
      throw Error("no part " + std::to_string(index));
    const CustomPart &part = all[size_t(index)];
    if (key == "id") return part.id;
    if (key == "mod") return part.mod;
    if (key == "name") return part.name;
    if (key == "base") return part.base;
    throw Error("unknown part info '" + key + "'");
  });

  // Any part.
  renut.Function("parts._vehicle_of", [](int64_t instance) -> int64_t {
    const uint32_t vehicle = Load32(Address(instance, "part") + kPartVehicle);
    return Heap(vehicle) ? vehicle : 0;
  });
  renut.Function("parts._class", [](int64_t instance) {
    return renut::game::InstanceClassName(Base(), Address(instance, "part"));
  });
  renut.Function("parts._is_a", [](int64_t instance, std::string class_name) {
    return renut::game::InstanceIsA(Base(), Address(instance, "part"), class_name);
  });
  renut.Function("parts._aid", [](int64_t instance) -> int64_t {
    return Load32(Address(instance, "part") + kPartAid);
  });
  // A custom part's id by its record's aid ("" for a shipped part).
  renut.Function("parts._id_of", [](int64_t instance) -> std::string {
    const renut::mods::KnownCustomPart *known =
        renut::mods::FindKnownCustomPart(Load32(Address(instance, "part") + kPartAid));
    return known && renut::mods::FindCustomPart(known->id) ? known->id : std::string();
  });
  renut.Function("parts._position", [](int64_t instance, int64_t axis) -> double {
    if (axis < 0 || axis > 2)
      throw Error("axis out of range");
    return LoadFloat(Address(instance, "part") + kPartWorldPosition + 4 * uint32_t(axis));
  });
  renut.Function("parts._cell", [](int64_t instance, int64_t axis) -> int64_t {
    const uint32_t cell = Load32(Address(instance, "part") + kPartCell);
    const auto signed_bits = [](uint32_t v, int bits) { return int64_t(int32_t(v << (32 - bits)) >> (32 - bits)); };
    switch (axis) {
    case 0: return signed_bits(cell & 0x7FF, 11);
    case 1: return signed_bits((cell >> 11) & 0x3FF, 10);
    case 2: return signed_bits(cell >> 21, 11);
    }
    throw Error("axis out of range");
  });
  renut.Function("parts._orientation", [](int64_t instance) -> int64_t {
    return Load32(Address(instance, "part") + kPartOrientation);
  });
  renut.Function("parts._health", [](int64_t instance, int64_t which) -> double {
    const uint32_t part = Address(instance, "part");
    switch (which) {
    case 0: return LoadFloat(part + kPartHealth);
    case 1: return LoadFloat(part + kPartMaxHealth);
    case 2: return LoadFloat(part + kPartHealthFraction);
    case 3: return Load32(part + kPartDamageLevel);
    }
    throw Error("unknown health value");
  });
  renut.Function("parts._set_health", [](int64_t instance, double health) {
    const uint32_t part = Address(instance, "part");
    const double max = LoadFloat(part + kPartMaxHealth);
    health = std::clamp(health, 0.0, max);
    StoreFloat(part + kPartHealth, health);
    StoreFloat(part + kPartHealthFraction, max > 0 ? health / max : 0);
    Store32(part + kPartHealthChanged, 1);  // the game works out its damage level again
  });
  renut.Function("parts._damage", [](int64_t instance, double amount) {
    const uint32_t part = Address(instance, "part");
    RequireGame();
    const bool breaks = DamagePart(part, amount) & 0xFF;
    if (breaks)
      BreakOffPart(part, 1, 0);
    return breaks;
  });
  renut.Function("parts._break", [](int64_t instance) {
    const uint32_t part = Address(instance, "part");
    RequireGame();
    BreakOffPart(part, 1, 0);
  });
  renut.Function("parts._press", [](int64_t instance, int64_t control) {
    const uint32_t part = Address(instance, "part");
    const uint32_t vehicle = Load32(part + kPartVehicle);
    RequireGame();
    if (Heap(vehicle))
      PressPart(part, vehicle, uint32_t(control));
  });
  renut.Function("parts._release", [](int64_t instance, int64_t control) {
    const uint32_t part = Address(instance, "part");
    const uint32_t vehicle = Load32(part + kPartVehicle);
    RequireGame();
    if (Heap(vehicle))
      ReleasePart(part, vehicle, uint32_t(control));
  });
  renut.Function("parts._fire", [](int64_t instance, double px, double py, double pz, double vx, double vy, double vz,
                                   double qx, double qy, double qz, double qw) -> int64_t {
    const uint32_t part = Weapon(instance);
    RequireGame();
    if (!Load32(part + kWeaponProjectilesMade))
      return 0;
    const uint32_t s = Scratch();
    StoreVec4(s, px, py, pz, 1);
    StoreVec4(s + 16, qx, qy, qz, qw);
    StoreVec4(s + 32, vx, vy, vz, 0);
    return FireProjectile(part, s, s + 16, s + 32);
  });
  // Any projectile, from any part with a behaviour; 'size' of them can fly at once.
  renut.Function("parts._fire_as", [](int64_t instance, int64_t aid, int64_t size, double px, double py, double pz,
                                      double vx, double vy, double vz, double qx, double qy, double qz,
                                      double qw) -> int64_t {
    const ProjectilePool &pool = PoolFor(instance, aid, size);
    if (pool.age < kPoolReadyTicks)
      return 0;
    const uint32_t s = Scratch();
    StoreVec4(s, px, py, pz, 1);
    StoreVec4(s + 16, qx, qy, qz, qw);
    StoreVec4(s + 32, vx, vy, vz, 0);
    // sub_82626070 reads only a weapon's pool, so the pool stands in for a weapon.
    return FireProjectile(pool.slots - kWeaponPool, s, s + 16, s + 32);
  });
  renut.Function("parts._can_fire_as", [](int64_t instance, int64_t aid, int64_t size) {
    return PoolCanFire(PoolFor(instance, aid, size));
  });
  renut.Function("parts._projectile_aid", [](std::string name) -> int64_t { return ProjectileAid(name); });
  renut.Function("parts._hold", [](int64_t instance, bool held) {
    Store32(Weapon(instance) + kWeaponHeld, held ? 1 : 0);
  });

  // Vehicles: motion.
  renut.Function("vehicle._has_body", [](int64_t vehicle) {
    return Heap(Load32(Address(vehicle, "vehicle") + kVehicleBody));
  });
  renut.Function("vehicle._get", [](int64_t vehicle, int64_t what, int64_t axis) -> double {
    if (axis < 0 || axis > 2)
      throw Error("axis out of range");
    return LoadFloat(uint32_t(int64_t(Motion(vehicle)) + QuantityOffset(what) + 4 * axis));
  });
  renut.Function("vehicle._set", [](int64_t vehicle, int64_t what, double x, double y, double z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
      throw Error("not a finite vector");
    const uint32_t motion = Motion(vehicle);
    if (what == kPosition) {
      // Its position and both centres of mass move together.
      const double d[] = {x - LoadFloat(motion), y - LoadFloat(motion + 4), z - LoadFloat(motion + 8)};
      for (uint32_t at : {0u, kMotionCentres[0], kMotionCentres[1]})
        for (uint32_t i = 0; i < 3; ++i)
          StoreFloat(motion + at + 4 * i, LoadFloat(motion + at + 4 * i) + d[i]);
      return;
    }
    if (what != kVelocity && what != kAngularVelocity)
      throw Error("only position, velocity and angular velocity can be set");
    const uint32_t at = motion + uint32_t(QuantityOffset(what));
    StoreFloat(at, x);
    StoreFloat(at + 4, y);
    StoreFloat(at + 8, z);
  });
  renut.Function("vehicle._inverse_mass", [](int64_t vehicle) -> double {
    return LoadFloat(Motion(vehicle) + kMotionInverseMass);
  });
  renut.Function("vehicle._force", [](int64_t vehicle, double dt, double x, double y, double z) {
    const uint32_t body = Body(vehicle);
    RequireGame();
    const uint32_t s = Scratch();
    StoreVec4(s, x, y, z, 0);
    PushBody(body, dt, s);
  });
  renut.Function("vehicle._force_at",
                 [](int64_t vehicle, double dt, double x, double y, double z, double px, double py, double pz) {
                   const uint32_t s = Scratch();
                   StoreVec4(s, x, y, z, 0);
                   StoreVec4(s + 16, px, py, pz, 1);
                   CallMotion(vehicle, kForceAtPoint, {0, s, s + 16}, dt);
                 });
  renut.Function("vehicle._impulse_at",
                 [](int64_t vehicle, double x, double y, double z, double px, double py, double pz) {
                   const uint32_t s = Scratch();
                   StoreVec4(s, x, y, z, 0);
                   StoreVec4(s + 16, px, py, pz, 1);
                   CallMotion(vehicle, kPointImpulse, {s, s + 16});
                 });
  renut.Function("vehicle._angular_impulse", [](int64_t vehicle, double x, double y, double z) {
    const uint32_t s = Scratch();
    StoreVec4(s, x, y, z, 0);
    CallMotion(vehicle, kAngularImpulse, {s});
  });

  // Vehicles: driver, health, resources, parts.
  renut.Function("vehicle._input", DriverInput);
  renut.Function("vehicle._driver", [](int64_t vehicle) -> int64_t {
    const uint32_t v = Address(vehicle, "vehicle");
    RequireGame();
    return VehicleDriver(v);
  });
  renut.Function("vehicle._health", [](int64_t vehicle, int64_t which) -> double {
    const uint32_t v = Address(vehicle, "vehicle");
    switch (which) {
    case 0: return LoadFloat(v + kVehicleHealth);
    case 1: return LoadFloat(v + kVehicleMaxHealth);
    case 2: return LoadFloat(v + kVehicleHealthFraction);
    case 3: return Load32(v + kVehicleDamageLevel);
    }
    throw Error("unknown health value");
  });
  renut.Function("vehicle._set_health", [](int64_t vehicle, double health) {
    const uint32_t v = Address(vehicle, "vehicle");
    RequireGame();
    SetVehicleHealth(v, std::clamp(health, 0.0, double(LoadFloat(v + kVehicleMaxHealth))));
  });
  renut.Function("vehicle._repair", [](int64_t vehicle) {
    const uint32_t v = Address(vehicle, "vehicle");
    RequireGame();
    RepairVehicle(v);
  });
  renut.Function("vehicle._damage", [](int64_t vehicle, double amount, int64_t part, int64_t attacker) {
    const uint32_t v = Address(vehicle, "vehicle");
    RequireGame();
    DamageVehicle(v, amount, part ? Address(part, "part") : 0, uint32_t(attacker));
  });
  renut.Function("vehicle._disabled", [](int64_t vehicle) {
    return int32_t(Load32(Address(vehicle, "vehicle") + kVehicleDisabled)) > 0;
  });
  renut.Function("vehicle._invulnerable", [](int64_t vehicle) {
    return (Load32(Address(vehicle, "vehicle") + kVehicleFlags) & kVehicleInvulnerable) != 0;
  });
  renut.Function("vehicle._set_invulnerable", [](int64_t vehicle, bool on) {
    const uint32_t v = Address(vehicle, "vehicle");
    const uint32_t flags = Load32(v + kVehicleFlags);
    Store32(v + kVehicleFlags, on ? flags | kVehicleInvulnerable : flags & ~kVehicleInvulnerable);
  });
  renut.Function("vehicle._resource", [](int64_t vehicle, int64_t kind, int64_t which) -> double {
    const uint32_t v = Address(vehicle, "vehicle");
    const uint32_t k = 4 * ResourceKind(kind);
    switch (which) {
    case 0: return LoadFloat(v + kVehicleResource + k);
    case 1: return LoadFloat(v + kVehicleResourceMax + k);
    case 2: return Load32(v + kVehicleResourceInfinite + k) ? 1 : 0;
    }
    throw Error("unknown resource value");
  });
  renut.Function("vehicle._set_resource", [](int64_t vehicle, int64_t kind, double value) {
    const uint32_t v = Address(vehicle, "vehicle");
    const uint32_t k = 4 * ResourceKind(kind);
    StoreFloat(v + kVehicleResource + k, std::clamp(value, 0.0, double(LoadFloat(v + kVehicleResourceMax + k))));
  });
  renut.Function("vehicle._set_infinite", [](int64_t vehicle, int64_t kind, bool on) {
    Store32(Address(vehicle, "vehicle") + kVehicleResourceInfinite + 4 * ResourceKind(kind), on ? 1 : 0);
  });
  renut.Function("vehicle._use", [](int64_t vehicle, int64_t kind, double amount) {
    const uint32_t v = Address(vehicle, "vehicle");
    const uint32_t k = ResourceKind(kind);
    RequireGame();
    return (UseResource(v, k, amount) & 0xFF) != 0;
  });
  renut.Function("vehicle._name", [](int64_t vehicle) {
    return Utf8(Address(vehicle, "vehicle") + kVehicleName, kVehicleNameLength);
  });
  renut.Function("vehicle._level", [](int64_t vehicle) -> int64_t {
    return Load32(Address(vehicle, "vehicle") + kVehicleLevel);
  });
  renut.Function("vehicle._part_count", [](int64_t vehicle) -> int64_t {
    const uint32_t v = Address(vehicle, "vehicle");
    const uint32_t begin = Load32(v + kVehicleParts), end = Load32(v + kVehicleParts + 4);
    return end > begin ? (end - begin) / kVehiclePartEntry : 0;
  });
  renut.Function("vehicle._part", [](int64_t vehicle, int64_t index) -> int64_t {
    const uint32_t v = Address(vehicle, "vehicle");
    const uint32_t begin = Load32(v + kVehicleParts), end = Load32(v + kVehicleParts + 4);
    if (index < 0 || begin + uint64_t(index + 1) * kVehiclePartEntry > end)
      throw Error("no part " + std::to_string(index));
    return Load32(begin + uint32_t(index) * kVehiclePartEntry + kVehiclePartEntryPart);
  });

  // Effects, and a damage event's amount.
  renut.Function("camera._get", [](int64_t row, int64_t axis) -> double {
    if (row < 0 || row > 3 || axis < 0 || axis > 2)
      throw Error("no such camera value");
    return LoadFloat(kCameraMatrix + 16 * uint32_t(row) + 4 * uint32_t(axis));
  });
  renut.Function("camera._fov", []() -> double { return LoadFloat(kCameraFieldOfView); });

  // A ray through the physics world the vehicle is in: how far along it hits (1: nothing).
  renut.Function("world._ray", [](int64_t vehicle, double fx, double fy, double fz, double tx, double ty,
                                  double tz) -> double {
    const uint32_t world = Load32(Body(vehicle) + kBodyWorld);
    if (!Heap(world))
      throw Error("the vehicle is in no physics world");
    RequireGame();
    return RayFraction(world, fx, fy, fz, tx, ty, tz, 0);
  });
  // The same, past the vehicle itself: it carries the vehicle's filter info, as _in_air's rays do.
  renut.Function("world._ray_past", [](int64_t vehicle, double fx, double fy, double fz, double tx, double ty,
                                       double tz) -> double {
    const uint32_t body = Body(vehicle);
    const uint32_t world = Load32(body + kBodyWorld);
    if (!Heap(world))
      throw Error("the vehicle is in no physics world");
    RequireGame();
    return RayFraction(world, fx, fy, fz, tx, ty, tz, Load32(body + kBodyFilterInfo));
  });
  // Nothing within `reach` below the vehicle's lowest layer of parts. The rays carry the vehicle's own filter
  // info, which hkpGroupFilter (sub_8293AF50) never lets collide with itself, so they pass through the vehicle.
  renut.Function("vehicle._in_air", [](int64_t vehicle, double reach) {
    if (!std::isfinite(reach))
      throw Error("not a finite reach");
    if (reach <= 0)
      reach = kInAirReach;
    const uint32_t v = Address(vehicle, "vehicle");
    const uint32_t body = Body(vehicle);
    const uint32_t world = Load32(body + kBodyWorld);
    if (!Heap(world))
      throw Error("the vehicle is in no physics world");
    RequireGame();
    const uint32_t filter = Load32(body + kBodyFilterInfo);
    struct Point {
      double x, y, z;
    };
    std::vector<Point> parts;
    const uint32_t begin = Load32(v + kVehicleParts), end = Load32(v + kVehicleParts + 4);
    for (uint32_t entry = begin; Heap(entry) && entry < end && parts.size() < 4096; entry += kVehiclePartEntry) {
      const uint32_t part = Load32(entry + kVehiclePartEntryPart);
      if (Heap(part))
        parts.push_back({LoadFloat(part + kPartWorldPosition), LoadFloat(part + kPartWorldPosition + 4),
                         LoadFloat(part + kPartWorldPosition + 8)});
    }
    if (parts.empty())
      return false;
    double lowest = parts.front().y;
    for (const Point &p : parts)
      lowest = std::min(lowest, p.y);
    for (const Point &p : parts) {
      if (p.y <= lowest + kLowestLayer && RayFraction(world, p.x, p.y, p.z, p.x, p.y - reach, p.z, filter) < 1.0)
        return false;
    }
    return true;
  });
  renut.Function("world._normal", [](int64_t axis) -> double {
    if (axis < 0 || axis > 2)
      throw Error("axis out of range");
    return LoadFloat(RayScratch() + kRayInputSize + 4 * uint32_t(axis));
  });
  renut.Function("mouse._get", [](int64_t which) -> double {
    float x, y, aspect;
    const bool inside = renut::game::CursorPosition(x, y, aspect);
    switch (which) {
    case 0: return inside ? x : 0.5;
    case 1: return inside ? y : 0.5;
    case 2: return inside ? aspect : 16.0 / 9.0;
    case 3: return inside ? 1 : 0;
    }
    throw Error("no such mouse value");
  });

  // The game's state (renut.game): the running world and challenge, pausing, game flags and counters.
  using renut::game::flags::Table;
  renut.Category("game.", "Game State").Category("game_", "Game State").Category("challenge_", "Challenges");
  renut.Category("world_", "Worlds");
  // The node editor's lists of the game's flags and counters, as the game has them once its tables are up.
  const auto names_of = [](std::initializer_list<Table> tables) {
    std::vector<std::pair<std::string, std::string>> list;
    for (const Table table : tables) {
      const std::vector<std::string> &names = renut::game::flags::Names(table);
      for (size_t i = 1; i < names.size(); ++i)
        list.emplace_back(names[i], names[i]);
    }
    return list;
  };
  renut.Choices("game.Flag", std::function([names_of] { return names_of({Table::kNormal, Table::kGlobal, Table::kVolatile}); }));
  renut.Choices("game.Counter", std::function([names_of] { return names_of({Table::kCounters}); }));
  // Whether this runs inside a renut event, where calls into the game work now (outside, they wait for game_tick).
  renut.Function("game._in_event", [] { return t_events > 0; });
  renut.Function("game._world", []() -> int64_t {
    const uint32_t world = RunningWorld();
    return world ? Load32(world + kWorldNumber) : 0;
  });
  renut.Function("game._world_name", [](int64_t number) { return WorldName(uint32_t(number)); });
  renut.Function("game._scene", []() -> int64_t {
    const uint32_t world = RunningWorld();
    return world ? Load32(world + kWorldScene) : 0;
  });
  renut.Function("game._challenge", []() -> int64_t {
    const uint32_t man = ChallengeMan();
    return man ? Load32(man + kChallengeRunning) : 0;
  });
  renut.Function("game._challenge_id", [](std::string name) -> int64_t {
    return renut::mods::AssetId(kTypeChallenge, name);
  });
  // A challenge from code: started by its asset, won or lost (challengeSuccess 1, 2; while played, state 13), its
  // stage's time limit, its timer held. As the game's own fail path does: the player's outcome, the stage's, settle.
  constexpr uint32_t kChallengeSlots = 96, kChallengeResult = 636, kChallengeStageId = 632, kChallengeHeld = 3044;
  constexpr uint32_t kChallengePlaying = 13;
  constexpr uint32_t kStageTimed = 164, kStageLimit = 168, kStageElapsed = 172;
  renut.Function("game._challenge_start", [](std::string challenge) {
    const uint32_t man = ChallengeMan();
    if (!man)
      throw Error("there's no world to start a challenge in");
    if (Load32(man + kChallengeRunning))
      throw Error("a challenge is running already");
    const uint32_t aid = AidByName(kTypeChallenge, "challenge", std::move(challenge));
    RequireGame();
    {
      SpawnGuard guard;  // one this world hasn't loaded is refused, not the dirty-disc screen
      if (!AssetOpens(aid))
        throw Error("that challenge isn't loaded in this world");
    }
    const uint32_t s = Scratch() + 44;
    Store32(s, aid);
    BeginChallenge(man, s, 0);
  });
  // Another mod world, by its id: the game unloads this one and loads it, as through one of its doors.
  renut.Function("worlds._go", [](std::string id) {
    const uint32_t scene = renut::game::custom_worlds::WorldScene(id);
    if (!scene)
      throw Error("no world '" + id + "' (renut.worlds.add it first)");
    const uint32_t world = RunningWorld();
    if (!world)
      throw Error("there's no world to go from");
    RequireGame();
    const uint32_t s = Scratch() + 44;
    Store32(s, scene);
    GoToScene(s, world, 1, 0);
  });
  renut.Function("game._challenge_end", [](bool won) {
    const uint32_t man = ChallengeMan();
    if (!man || Load32(man + kChallengeState) != kChallengePlaying)
      throw Error("a challenge is won or lost while it's being played");
    RequireGame();
    const uint32_t outcome = won ? 1 : 2;
    Store32(man + kChallengeOutcome + kChallengeSlots * PlayerIndex(Load32(kPlayerProfile)), outcome);
    Store32(man + kChallengeResult, outcome);
    SetChallengeState(man, kChallengeSettling);
  });
  renut.Function("game._challenge_time", [](double limit, double elapsed) {
    const uint32_t man = ChallengeMan();
    if (!man || !Load32(man + kChallengeRunning))
      throw Error("there's no challenge running");
    if (!std::isfinite(limit) || !std::isfinite(elapsed) || limit < 0 || elapsed < 0)
      throw Error("a challenge's times are 0 or more");
    RequireGame();
    const uint32_t stage = ChallengeStage(man, Load32(man + kChallengeStageId));
    if (!Heap(stage))
      throw Error("the challenge has no stage being played");
    Base()[stage + kStageTimed] = limit > 0 ? 1 : 0;
    StoreFloat(stage + kStageLimit, limit);
    StoreFloat(stage + kStageElapsed, elapsed);
  });
  renut.Function("game._challenge_hold", [](bool held) {
    const uint32_t man = ChallengeMan();
    if (!man)
      throw Error("there's no world");
    Store32(man + kChallengeHeld, held ? 1 : 0);
  });
  // Awards a counter as the game does, its pop-up unless quiet (Jiggies, notes: renut.game.Counter's names).
  renut.Function("game._award", [](std::string counter, int64_t amount, bool show) {
    const auto [table, index] = FlagByName(counter);
    if (table != Table::kCounters)
      throw Error("'" + counter + "' isn't a counter");
    if (amount <= 0)
      throw Error("an award is 1 or more");
    RequireGame();
    AddCounter(Load32(kPlayerProfile), index, uint32_t(amount), show ? 0 : 1);
  });

  // The world's look (renut.world.fog / env): its environment's active set (sub_8226A858 copies it to the renderer
  // each frame). Fog is block A (as script action 82 writes it): mode + 112, colour + 144, near/far/most + 192..200.
  constexpr uint32_t kWorldEnvironment = 0x67C, kEnvSets = 16, kEnvSetSize = 448, kEnvActive = 912;
  constexpr uint32_t kFogMode = 112, kFogColour = 144, kFogNear = 192, kFogFar = 196, kFogMost = 200;
  const auto env_set = [] {
    const uint32_t world = RunningWorld();
    const uint32_t env = world ? Load32(world + kWorldEnvironment) : 0;
    if (!Heap(env))
      throw Error("this world has no environment");
    const uint32_t active = Load32(env + kEnvActive);
    if (active > 3)
      throw Error("this world's environment isn't set up");
    return env + kEnvSets + kEnvSetSize * active;
  };
  renut.Function("world._fog", [env_set](double near_, double far_, double most, int64_t rgba) {
    for (const double value : {near_, far_, most})
      if (!std::isfinite(value))
        throw Error("not a finite fog value");
    const uint32_t set = env_set();
    StoreFloat(set + kFogNear, near_);
    StoreFloat(set + kFogFar, far_);
    StoreFloat(set + kFogMost, most);
    Store32(set + kFogColour, uint32_t(rgba));
  });
  renut.Function("world._fog_get", [env_set](int64_t which) -> double {
    const uint32_t set = env_set();
    switch (which) {
    case 0: return LoadFloat(set + kFogNear);
    case 1: return LoadFloat(set + kFogFar);
    case 2: return LoadFloat(set + kFogMost);
    case 3: return double(Load32(set + kFogColour));
    default: return double(Load32(set + kFogMode));
    }
  });
  renut.Function("world._env", [env_set](int64_t offset) -> double {
    if (offset < 0 || offset > int64_t(kEnvSetSize) - 4 || offset % 4)
      throw Error("an environment offset is a multiple of 4 under 448");
    return LoadFloat(env_set() + uint32_t(offset));
  });
  renut.Function("world._set_env", [env_set](int64_t offset, double value) {
    if (offset < 0 || offset > int64_t(kEnvSetSize) - 4 || offset % 4 || !std::isfinite(value))
      throw Error("an environment offset is a multiple of 4 under 448, its value finite");
    StoreFloat(env_set() + uint32_t(offset), value);
  });
  renut.Function("game._paused", [] { return Load32(kPauseMenu) != 0xFFFFFFFFu; });
  renut.Function("game._flag", [](std::string name) {
    const auto [table, index] = FlagByName(name);
    if (table == Table::kCounters)
      throw Error("'" + name + "' is a counter: renut.game.counter");
    if (!renut::game::flags::HasProfile())
      throw Error("no save is loaded");
    return renut::game::flags::Get(table, index) != 0;
  });
  renut.Function("game._set_flag", [](std::string name, bool on) {
    const auto [table, index] = FlagByName(name);
    if (table == Table::kCounters)
      throw Error("'" + name + "' is a counter: renut.game.set_counter");
    if (!renut::game::flags::HasProfile())
      throw Error("no save is loaded");
    if (!renut::game::flags::Set(table, index, on ? 1 : 0))
      throw Error("this online session's host doesn't allow changing the game");
  });
  renut.Function("game._counter", [](std::string name) -> int64_t {
    const auto [table, index] = FlagByName(name);
    if (table != Table::kCounters)
      throw Error("'" + name + "' is a flag: renut.game.flag");
    if (!renut::game::flags::HasProfile())
      throw Error("no save is loaded");
    return renut::game::flags::Get(table, index);
  });
  renut.Function("game._set_counter", [](std::string name, int64_t value) {
    const auto [table, index] = FlagByName(name);
    if (table != Table::kCounters)
      throw Error("'" + name + "' is a flag: renut.game.set_flag");
    if (!renut::game::flags::HasProfile())
      throw Error("no save is loaded");
    if (value < INT32_MIN || value > INT32_MAX)
      throw Error("a counter is a 32-bit number");
    if (!renut::game::flags::Set(table, index, int32_t(value)))
      throw Error("this online session's host doesn't allow changing the game");
  });

  // The player (renut.player): Banjo, on foot or driving.
  renut.Function("player._avatar", []() -> int64_t { return PlayerAvatar(); });
  renut.Function("player._vehicle", []() -> int64_t {
    const uint32_t avatar = PlayerAvatar();
    const uint32_t vehicle = avatar ? Load32(avatar + kAvatarVehicle) : 0;
    return Heap(vehicle) ? vehicle : 0;
  });
  renut.Function("player._teleport", [](double x, double y, double z) {
    const uint32_t avatar = PlayerAvatar();
    if (!avatar)
      throw Error("there's no player in this world");
    if (Heap(Load32(avatar + kAvatarVehicle)))
      throw Error("the player is in a vehicle: move the vehicle instead (v:set_position)");
    RequireGame();
    const uint32_t s = Scratch();
    StoreVec4(s, x, y, z, 1);
    MoveActor(avatar, s, 0, 0, 0);
  });
  renut.Function("player._play", [](std::string animation) {
    const uint32_t avatar = PlayerAvatar();
    if (!avatar)
      throw Error("there's no player in this world");
    if (animation.empty())
      throw Error("an animation's name, like the game's");
    RequireGame();
    PlayAnimation(avatar, TextScratch(animation), 0);
  });
  // The player model Banjo is made with from now on, in every world (over a world's player_model); "" for his own.
  renut.Function("player._set_model", [](std::string model) {
    g_player_model.store(renut::game::custom_worlds::PlayerModelId(model));
  });

  // The camera: placed and pointed by a mod until released; the field of view.
  renut.Function("camera._set", [](double x, double y, double z, double lx, double ly, double lz) {
    for (const double value : {x, y, z, lx, ly, lz}) {
      if (!std::isfinite(value))
        throw Error("not a finite camera position");
    }
    if (std::abs(lx - x) + std::abs(ly - y) + std::abs(lz - z) < 1e-3)
      throw Error("the camera can't look at where it is");
    std::lock_guard lock(g_camera_mutex);
    g_camera = {true, {float(x), float(y), float(z)}, {float(lx), float(ly), float(lz)}};
    return g_camera_known.load(std::memory_order_acquire);
  });
  renut.Function("camera._release", [] {
    std::lock_guard lock(g_camera_mutex);
    g_camera.active = false;
  });
  renut.Function("camera._set_fov", [](double degrees) {
    if (!std::isfinite(degrees))
      throw Error("not a finite field of view");
    renut::game::SetFieldOfView(float(degrees));
  });

  // A part's paint, and a vehicle's mass.
  renut.Function("parts._paint", [](int64_t instance) -> int64_t { return Load32(Address(instance, "part") + kPartPaint); });
  renut.Function("parts._set_paint", [](int64_t instance, int64_t rgba, bool effect) {
    const uint32_t part = Address(instance, "part");
    RequireGame();
    const uint32_t s = Scratch();
    Store32(s, uint32_t(rgba));
    PaintPart(part, s, effect ? 1 : 0);
  });
  renut.Function("vehicle._scale_mass", [](int64_t vehicle, double scale) {
    if (!std::isfinite(scale) || scale <= 0)
      throw Error("a mass scale is a number above 0");
    // hkpMotion: inverse inertia (x, y, z) then inverse mass, all of which a heavier vehicle divides.
    const uint32_t inverse = Motion(vehicle) + kMotionInverseInertia;
    for (uint32_t i = 0; i < 4; ++i)
      StoreFloat(inverse + 4 * i, LoadFloat(inverse + 4 * i) / scale);
  });

  // Crates (renut.crates): what each gives, swapping contents, giving one, those in the world; the parts owned.
  renut.Category("crates.", "Crates").Category("crate_", "Crates");
  // The crates' flags, as the game names them (all in Showdown Town), for the node editor's flag pins to pick from.
  {
    constexpr char kCrateFlag[] = "gameFlag_Normal_ShowdownTown_CollectedCrate_";
    std::vector<std::pair<std::string, std::string>> flags;
    const auto add = [&flags, &kCrateFlag](const std::string &label, const std::string &suffix) {
      flags.emplace_back(label, kCrateFlag + suffix);
    };
    // The world's crates (_01 to _70) first, then Humba's, LOG's, the keys', Jinjo Bingo's and Stop 'n' Swop's.
    struct Group {
      const char *label, *suffix;
      int count;
    };
    for (const Group &group : {Group{"Crate_", "", 70}, Group{"Humba_", "Humba_", 70}, Group{"LOG_", "LOG_", 40},
                               Group{"Key_", "Key_", 6}, Group{"JinjoBingo_", "JinjoBingo_", 8}}) {
      if (group.label == std::string_view("LOG_"))
        add("LOG_00_StartPack", "LOG_00_StartPack");
      for (int i = 1; i <= group.count; ++i) {
        const std::string number = (i < 10 ? "0" : "") + std::to_string(i);
        add(group.label + number, group.suffix + number);
      }
    }
    for (const char *colour : {"Yellow", "Red", "Pink", "LightBlue", "Ice", "Green", "DarkBlue"})
      add(std::string("StopnSwop_") + colour, std::string("StopnSwop_") + colour);
    renut.Choices("crates.Flag", std::move(flags));
  }
  renut.Function("crates._list", [] {
    std::string list;
    for (const Crate &crate : Crates(false))
      list += (list.empty() ? "" : "\n") + crate.flag;
    return list;
  });
  renut.Function("crates._contents", [](std::string flag) {
    const Crate *crate = FindCrate(Crates(false), flag);
    if (!crate)
      throw Error("no crate with the flag '" + flag + "'");
    std::string list;
    for (const auto &[aid, count] : crate->parts)
      list += (list.empty() ? "" : "\n") + std::to_string(aid) + ":" + std::to_string(count);
    return list;
  });
  renut.Function("crates._swap", [](std::string flag, std::string other) {
    if (flag.rfind("gameFlag_", 0) != 0 || (!other.empty() && other.rfind("gameFlag_", 0) != 0))
      throw Error("a crate is named by its flag, gameFlag_...");
    std::lock_guard lock(g_crates_mutex);
    // Checked when the crates are known already; otherwise as they're given.
    if (!g_crates.empty() && (!FindCrate(g_crates, flag) || (!other.empty() && !FindCrate(g_crates, other))))
      throw Error("no crate with the flag '" + (FindCrate(g_crates, flag) ? other : flag) + "'");
    if (other.empty() || other == flag)
      g_crate_swaps.erase(flag);
    else
      g_crate_swaps[flag] = other;
  });
  renut.Function("crates._give", [](std::string flag, bool show) {
    const auto [table, index] = FlagByName(flag);
    if (table != Table::kNormal)
      throw Error("a crate's flag is a gameFlag_Normal_ one");
    const uint32_t world = RunningWorld();
    const uint32_t scene = world ? Load32(world + kWorldSceneObject) : 0;
    if (!Heap(scene))
      throw Error("crates are given in a world");
    RequireGame();
    Crates(false);
    AwardCrate(scene + kSceneCrateList, index, show ? 1 : 0);
  });
  renut.Function("crates._in_world", [] {
    std::string list;
    const uint32_t world = RunningWorld();
    if (!world)
      return list;
    const std::vector<std::string> &names = renut::game::flags::Names(Table::kNormal);
    uint32_t node = Load32(world + kWorldObjects);
    for (uint32_t n = 0; Heap(node) && n < 50000; ++n, node = Load32(node + 4)) {
      const uint32_t object = node - kObjectNode;
      if (Load32(object) != kCrateVtable)
        continue;
      const uint32_t flag = Load32(object + kCrateObjectFlag);
      const std::string name = flag < names.size() ? names[flag] : std::string();
      list += name + ";" + std::to_string(LoadFloat(object + kAvatarPosition)) + ";" +
              std::to_string(LoadFloat(object + kAvatarPosition + 4)) + ";" +
              std::to_string(LoadFloat(object + kAvatarPosition + 8)) + ";" + std::to_string(object) + "\n";
    }
    return list;
  });
  renut.Function("crates._spawn", [](std::string flag, std::string where, double x, double y, double z) -> int64_t {
    const auto [table, index] = FlagByName(flag);
    const Place at = Where(where, x, y, z, 0);
    if (table != Table::kNormal || !FindCrate(Crates(false), flag))
      throw Error("'" + flag + "' isn't a crate's flag (renut.crates.list)");
    if (flag.size() >= kCopyCrateFlagLength)
      throw Error("a crate's flag is under 64 characters");
    const uint32_t aid = g_crate_objparams.load(std::memory_order_acquire);
    if (!aid)
      throw Error("reNut hasn't seen a crate yet: they're learned as Showdown Town loads its own");
    const uint32_t world = RunningWorld();
    if (!world)
      throw Error("crates are spawned in a world");
    RequireGame();
    const uint32_t s = Scratch();
    Store32(s, aid);
    SpawnGuard guard;
    const uint32_t copy = CopyObjparams(s);
    if (!Heap(copy))
      throw Error("the crate's objparams aren't loaded here");
    // Filled in as a level's placement fills a copy: its world, where it is, unturned, its scale and its flag.
    Store32(copy + kCopyWorld, world);
    const double position[3] = {at.x, at.y, at.z};
    for (uint32_t i = 0; i < 3; ++i) {
      StoreFloat(copy + kCopyPosition + 4 * i, position[i]);
      StoreFloat(copy + kCopyRotation + 4 * i, 0.0);
    }
    StoreFloat(copy + kCopyScale, 1.0);
    std::memset(Base() + copy + kCopyCrateFlag, 0, kCopyCrateFlagLength);
    std::memcpy(Base() + copy + kCopyCrateFlag, flag.c_str(), flag.size());
    // A crate isn't made while its flag is on (sub_823FF108 checks the profile's bit), so a collected one's bit is
    // off just while it's made.
    RequireAssets(copy, false);
    const uint32_t profile = Load32(kPlayerProfile);
    const uint32_t flag_set = Heap(profile) ? Load32(profile + kProfileFlags) : 0;
    const uint32_t bits = Heap(flag_set) ? Load32(flag_set + kFlagBits) : 0;
    uint8_t *const byte = Heap(bits) ? Base() + bits + (index >> 3) : nullptr;
    const uint8_t mask = uint8_t(1u << (index & 7));
    const bool collected = byte && (*byte & mask);
    if (collected)
      *byte &= uint8_t(~mask);
    const uint32_t object = CreateObject(ClassIdByName(copy + kCopyClassName), copy, 0);
    if (collected)
      *byte |= mask;
    FreeObject(copy);
    ThrowIfMissing(object);
    if (!object)
      throw Error("the game didn't make the crate");
    return object;
  });
  // Objects (renut.objects): the kinds the game has made this session, and the objects in the running world.
  renut.Category("objects.", "Objects");
  renut.Choices("objects.Kind", std::function([] {
    std::vector<std::pair<std::string, std::string>> list;
    std::lock_guard lock(g_kinds_mutex);
    for (const auto &[name, kind] : g_kinds)
      list.emplace_back(name, name);
    return list;
  }));
  renut.Function("objects._kinds", [] {
    std::string list;
    std::lock_guard lock(g_kinds_mutex);
    for (const auto &[name, kind] : g_kinds)
      list += name + ";" + kind.cls + "\n";
    return list;
  });
  renut.Function("objects._list", [] {
    std::string list;
    const uint32_t world = RunningWorld();
    if (!world)
      return list;
    std::lock_guard lock(g_kinds_mutex);
    uint32_t node = Load32(world + kWorldObjects);
    for (uint32_t n = 0; Heap(node) && n < 50000; ++n, node = Load32(node + 4)) {
      const uint32_t object = node - kObjectNode;
      const auto made = g_made.find(object);
      list += std::to_string(object) + ";" + (made != g_made.end() ? made->second : std::string()) + ";" +
              std::to_string(LoadFloat(object + kAvatarPosition)) + ";" +
              std::to_string(LoadFloat(object + kAvatarPosition + 4)) + ";" +
              std::to_string(LoadFloat(object + kAvatarPosition + 8)) + "\n";
    }
    return list;
  });
  renut.Function("objects._kind", [](int64_t object) {
    std::lock_guard lock(g_kinds_mutex);
    const auto made = g_made.find(Address(object, "object"));
    return made != g_made.end() ? made->second : std::string();
  });
  renut.Function("objects._position", [](int64_t object, int64_t axis) -> double {
    const uint32_t o = Address(object, "object");
    if (!InWorld(o))
      throw Error("not an object in this world");
    return LoadFloat(o + kAvatarPosition + 4 * uint32_t(std::clamp<int64_t>(axis, 0, 2)));
  });
  renut.Function("objects._is_character", [](int64_t object) {
    const uint32_t o = Address(object, "object");
    return InWorld(o) && IsActor(o);
  });
  renut.Function("objects._spawn", [](std::string kind, std::string where, double x, double y, double z,
                                      double yaw) -> int64_t {
    uint32_t aid = 0;
    {
      std::lock_guard lock(g_kinds_mutex);
      if (const auto it = g_kinds.find(kind); it != g_kinds.end())
        aid = it->second.aid;
    }
    // Not a kind made this session: an objparams asset by name ("aid_objparams_...", its name or "0x1F...").
    const bool learned = aid != 0;
    if (!learned)
      aid = AidByName(kTypeObjparams, "objparams", kind);
    const Place at = Where(where, x, y, z, yaw);
    uint32_t object = 0;
    try {
      object = SpawnObject(aid, at.x, at.y, at.z, at.yaw);
    } catch (const Error &error) {
      if (learned)
        throw;
      throw Error("the game hasn't made a '" + kind + "' this session (renut.objects.kinds lists what it has), and " +
                  "as an objparams asset: " + error.what());
    }
    Spawned(object, kind, aid);
    return object;
  });
  renut.Function("objects._remove", [](int64_t object) {
    const uint32_t o = Address(object, "object");
    if (!InWorld(o))
      throw Error("not an object in this world");
    RequireGame();
    RemoveObject(o, 0);
    std::lock_guard lock(g_kinds_mutex);
    g_made.erase(o);
    g_made_aid.erase(o);
    g_mod_spawned.erase(o);
  });

  // Characters (renut.actors): any actor in the world, the game's (Mumbo, the Jinjos, the Gruntbots) or spawned.
  renut.Category("actors.", "Characters");
  renut.Choices("actors.State", std::function([] {
    std::vector<std::pair<std::string, std::string>> list;
    for (const std::string &name : GameNames(kStrategyStateNames, kStrategyPrefix))
      list.emplace_back(name, name);
    return list;
  }));
  renut.Choices("actors.BodyState", std::function([] {
    std::vector<std::pair<std::string, std::string>> list;
    for (const std::string &name : GameNames(kBodyStateNames, kBodyPrefix))
      if (name != "Null")
        list.emplace_back(name, name);
    return list;
  }));
  // Who a character node acts on when no character is linked: these, then the characters in the world by name.
  renut.Choices("actors.Who", std::function([] {
    std::vector<std::pair<std::string, std::string>> list = {
        {"Nearest character", "nearest"}, {"Last spawned", "last"}, {"Banjo", "banjo"}};
    const uint32_t world = RunningWorld(), avatar = PlayerAvatar();
    if (!world)
      return list;
    std::set<std::string> names;
    std::lock_guard lock(g_kinds_mutex);
    uint32_t node = Load32(world + kWorldObjects);
    for (uint32_t n = 0; Heap(node) && n < 50000; ++n, node = Load32(node + 4)) {
      const uint32_t object = node - kObjectNode;
      if (object != avatar && IsActor(object))
        if (std::string name = ActorName(object); !name.empty())
          names.insert(std::move(name));
    }
    for (const std::string &name : names)
      list.emplace_back(name, name);
    return list;
  }));
  // Every character the game has, to spawn in any world, then the mods' own ("world/id").
  renut.Choices("actors.Character", std::function([] {
    std::vector<std::pair<std::string, std::string>> list;
    for (const GameCharacter &character : kGameCharacters)
      list.emplace_back(character.name, character.name);
    for (const renut::game::custom_npcs::NpcEntry &npc : renut::game::custom_npcs::Npcs())
      list.emplace_back("Mod: " + (npc.name.empty() ? npc.id : npc.name) + " (" + npc.world + "/" + npc.id + ")",
                        npc.world + "/" + npc.id);
    return list;
  }));
  renut.Function("actors._list", [] {
    std::string list;
    const uint32_t world = RunningWorld();
    if (!world)
      return list;
    std::lock_guard lock(g_kinds_mutex);
    uint32_t node = Load32(world + kWorldObjects);
    for (uint32_t n = 0; Heap(node) && n < 50000; ++n, node = Load32(node + 4)) {
      const uint32_t object = node - kObjectNode;
      if (!IsActor(object))
        continue;
      list += std::to_string(object) + ";" + ActorName(object) + ";" +
              std::to_string(LoadFloat(object + kAvatarPosition)) + ";" +
              std::to_string(LoadFloat(object + kAvatarPosition + 4)) + ";" +
              std::to_string(LoadFloat(object + kAvatarPosition + 8)) + "\n";
    }
    return list;
  });
  renut.Function("actors._find", [](std::string who) -> int64_t { return Who(0, who); });
  // A game character in the running world, whichever it is: its bundle is loaded into this one (until it goes).
  renut.Function("actors._spawn", [](std::string character, std::string where, double x, double y, double z,
                                     double yaw) -> int64_t {
    // A mod's character as "world/id".
    if (const size_t slash = character.find('/'); slash != std::string::npos)
      return SpawnModNpc(character.substr(0, slash), character.substr(slash + 1), Where(where, x, y, z, yaw));
    const GameCharacter *found = GameCharacterNamed(character);
    if (!found)
      throw Error("no game character '" + character + "' (the Character list has them all)");
    const uint32_t running = RunningWorld();
    if (!running)
      throw Error("characters are spawned in a world");
    const Place at = Where(where, x, y, z, yaw);
    RequireGame();
    LoadCharacterBundles(running, found->bundle, found->name);
    const uint32_t aid = renut::mods::AssetId(kTypeObjparams, std::string("banjox_actor_") + found->name);
    const uint32_t object = SpawnObject(aid, at.x, at.y, at.z, at.yaw);
    Spawned(object, found->name, aid);
    return object;
  });
  renut.Function("actors._place", [](int64_t character, std::string who, std::string where, double x, double y,
                                     double z) {
    const uint32_t a = Who(character, who);
    const Place at = Where(where.empty() ? "banjo" : where, x, y, z, 0);
    RequireGame();
    const uint32_t s = Scratch();
    StoreVec4(s, at.x, at.y, at.z, 1);
    MoveActor(a, s, 0, 0, 0);
  });
  renut.Function("actors._turn", [](int64_t character, std::string who, double yaw) {
    const uint32_t a = Who(character, who);
    RequireGame();
    const uint32_t s = Scratch();
    StoreVec4(s, 0, yaw * 3.14159265358979 / 180.0, 0, 0);
    SetRotation(a, s, 1);
  });
  renut.Function("actors._play", [](int64_t character, std::string who, std::string animation) {
    const uint32_t a = Who(character, who);
    if (animation.empty())
      throw Error("an animation's name, like the game's");
    RequireGame();
    PlayAnimation(a, TextScratch(animation), 0);
  });
  renut.Function("actors._state", [](int64_t character, std::string who) {
    const uint32_t strategy = ActorPart(Who(character, who), kActorStrategy, "strategy (it doesn't act on its own)");
    const uint32_t id = Load32(strategy + kStrategyState);
    const std::vector<std::string> names = GameNames(kStrategyStateNames, kStrategyPrefix);
    return id >= kFirstStrategyState && id - kFirstStrategyState < names.size() ? names[id - kFirstStrategyState]
                                                                                 : std::string();
  });
  renut.Function("actors._set_state", [](int64_t character, std::string who, std::string state) {
    const uint32_t strategy = ActorPart(Who(character, who), kActorStrategy, "strategy (it doesn't act on its own)");
    const uint32_t id = StateId(kStrategyStateNames, kStrategyPrefix, kFirstStrategyState, state);
    RequireGame();
    SetStrategyState(strategy, id);
  });
  renut.Function("actors._body_state", [](int64_t character, std::string who, std::string state) {
    const uint32_t body = ActorPart(Who(character, who), kActorBody, "body");
    const uint32_t id = StateId(kBodyStateNames, kBodyPrefix, 0, state);
    RequireGame();
    return SetBodyState(body, id) != 0;
  });
  renut.Function("actors._action", [](int64_t character, std::string who, std::string action) {
    const uint32_t mind = ActorPart(Who(character, who), kActorMind, "mind");
    if (action.empty())
      throw Error("an action's name, like the game's (\"npc_idle\")");
    RequireGame();
    const uint32_t aid = FindMindAction(mind, TextScratch(action));
    if (!Heap(aid) && (aid < 0x82000000 || aid >= 0x83000000))
      return false;
    PlayMindAction(mind, aid);
    return true;
  });
  renut.Function("actors._walk_to", [](int64_t character, std::string who, std::string to, double x, double y,
                                       double z) {
    const uint32_t mind = ActorPart(Who(character, who), kActorMind, "mind");
    const Place at = Where(to.empty() ? "banjo" : to, x, y, z, 0);
    for (const double value : {at.x, at.y, at.z})
      if (!std::isfinite(value))
        throw Error("not a finite position");
    RequireGame();
    ClearGoals(mind, 0);
    const uint32_t s = Scratch();
    StoreVec4(s, at.x, at.y, at.z, 1);
    WalkToPoint(mind, s);
  });
  renut.Function("actors._stop", [](int64_t character, std::string who) {
    const uint32_t mind = ActorPart(Who(character, who), kActorMind, "mind");
    RequireGame();
    ClearGoals(mind, 0);
  });
  renut.Function("actors._remove", [](int64_t character, std::string who) {
    const uint32_t a = Who(character, who);
    if (a == PlayerAvatar())
      throw Error("Banjo can't be removed");
    RequireGame();
    RemoveObject(a, 0);
    std::lock_guard lock(g_kinds_mutex);
    g_made.erase(a);
    g_made_aid.erase(a);
    g_mod_spawned.erase(a);
  });
  renut.Function("actors._vehicle", [](int64_t character, std::string who) -> int64_t {
    const uint32_t vehicle = Load32(Who(character, who) + kActorVehicle);
    return Heap(vehicle) ? vehicle : 0;
  });
  renut.Function("actors._talk", [](int64_t character, std::string who, std::string dialog) {
    const uint32_t a = Who(character, who);
    const uint32_t aid = AidByName(kTypeDialog, "dialog", std::move(dialog));
    const uint32_t world = RunningWorld();
    const uint32_t scene = world ? Load32(world + kWorldSceneObject) : 0;
    if (!Heap(scene))
      throw Error("there's no scene up to talk in");
    const uint32_t manager = Load32(scene + kSceneDialogs);
    if (!Heap(manager))
      throw Error("this world's scene has no dialogs");
    RequireGame();
    // Not over a cutscene or another dialog, as the game's own lines aren't.
    if (Load32(scene + kSceneBusy) || (DialogPlaying(manager) & 0xFF))
      return false;
    Store32(manager + kDialogTalking, a);
    const uint32_t s = Scratch();
    Store32(s, aid);
    CallWithFrame(kStartDialog, {manager, s, 0, Load32(kPlayerProfile), kEmptyText, 1, 0, 0});
    return true;
  });

  // A character's health (its body's, sub_82279988), defeat, the one it follows, its model.
  constexpr uint32_t kBodyHealth = 0xEC, kBodyMaxHealth = 0xF0, kEntityTargetRecord = 0xA80, kMindTarget = 56;
  constexpr uint32_t kStrategyFollow = 1046;
  renut.Choices("actors.Health", {{"Current", "0"}, {"Most", "1"}});
  renut.Function("actors._health", [](int64_t character, std::string who, int64_t which) -> double {
    const uint32_t body = ActorPart(Who(character, who), kActorBody, "body");
    return LoadFloat(body + (which ? kBodyMaxHealth : kBodyHealth));
  });
  renut.Function("actors._set_health", [](int64_t character, std::string who, double health) {
    const uint32_t body = ActorPart(Who(character, who), kActorBody, "body");
    if (!std::isfinite(health) || health < 0)
      throw Error("health is 0 or more");
    RequireGame();
    if (health == 0) {
      DamageBody(body, LoadFloat(body + kBodyHealth));
      return;
    }
    ReviveBody(body, std::max<double>(health, LoadFloat(body + kBodyMaxHealth)));  // alive, its most kept
    StoreFloat(body + kBodyHealth, health);
  });
  renut.Function("actors._damage", [](int64_t character, std::string who, double amount) -> double {
    const uint32_t body = ActorPart(Who(character, who), kActorBody, "body");
    if (!std::isfinite(amount) || amount < 0)
      throw Error("damage is 0 or more");
    RequireGame();
    DamageBody(body, amount);
    return LoadFloat(body + kBodyHealth);
  });
  renut.Function("actors._defeat", [](int64_t character, std::string who) {
    const uint32_t a = Who(character, who);
    RequireGame();
    DefeatActor(a);
  });
  // The target: a character's or a vehicle's targeting record (entity + 0xA80, its first word the entity).
  renut.Function("actors._follow", [](int64_t character, std::string who, int64_t target, std::string target_who) {
    const uint32_t a = Who(character, who);
    const uint32_t mind = ActorPart(a, kActorMind, "mind");
    const uint32_t t = target ? Address(target, "target") : Who(0, target_who);
    if (!InWorld(t) || Load32(t + kEntityTargetRecord) != t)
      throw Error("that can't be followed: it's not a character or a vehicle in this world");
    if (t == a)
      throw Error("a character can't follow itself");
    RequireGame();
    ClearGoals(mind, 0);
    SetMindTarget(mind, t + kEntityTargetRecord, 0);
    if (const uint32_t strategy = Load32(a + kActorStrategy); Heap(strategy))
      SetStrategyState(strategy, kStrategyFollow);
  });
  renut.Function("actors._unfollow", [](int64_t character, std::string who) {
    const uint32_t mind = ActorPart(Who(character, who), kActorMind, "mind");
    RequireGame();
    SetMindTarget(mind, 0, 0);
    ClearGoals(mind, 0);
  });
  renut.Function("actors._target", [](int64_t character, std::string who) -> int64_t {
    const uint32_t mind = ActorPart(Who(character, who), kActorMind, "mind");
    const uint32_t record = Load32(mind + kMindTarget);
    return Heap(record) && Heap(Load32(record)) ? Load32(record) : 0;
  });
  renut.Function("actors._model", [](int64_t character, std::string who, std::string model) {
    const uint32_t a = Who(character, who);
    const uint32_t aid = AidByName(0x04, "model", std::move(model));
    RequireGame();
    {
      SpawnGuard guard;  // a model this world hasn't loaded is refused, not the dirty-disc screen
      if (!AssetOpens(aid))
        throw Error("that model isn't loaded in this world");
    }
    const uint32_t s = Scratch() + 40;
    Store32(s, aid);
    SetEntityModel(a, s, RunningWorld());
  });
  renut.Function("npcs._spawn", [](std::string world, std::string id, std::string where, double x, double y, double z,
                                   double yaw) -> int64_t {
    return SpawnModNpc(world, id, Where(where, x, y, z, yaw));
  });
  // Whole vehicles (renut.vehicle.spawn): the enabled mods' vehicles/*.blueprint ("mod: name"), the save's vehicles
  // ("save: name", as the garage saved them), or a blueprint file's path.
  const auto mod_blueprints = [] {
    std::vector<std::pair<std::string, std::string>> list;
    std::error_code ec;
    for (const std::filesystem::path &dir : rexfabric::EnabledModDirs())
      for (const auto &entry : std::filesystem::directory_iterator(dir / "vehicles", ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".blueprint")
          list.emplace_back(rex::string::PathToUTF8(dir.filename()) + ": " +
                                rex::string::PathToUTF8(entry.path().stem()),
                            rex::string::PathToUTF8(entry.path()));
    if (const std::filesystem::path save = renut::mods::SaveFolder(); !save.empty())
      for (const renut::workshop::LocalVehicle &vehicle : renut::workshop::LocalVehicles(save))
        list.emplace_back("save: " + vehicle.name, rex::string::PathToUTF8(vehicle.file));
    return list;
  };
  renut.Function("vehicle._blueprints", [mod_blueprints] {
    std::string list;
    for (const auto &[label, file] : mod_blueprints())
      list += label + "\t" + file + "\n";
    return list;
  });
  renut.Choices("vehicle.Blueprint", std::function(mod_blueprints));
  renut.Choices("vehicle.Seat", {{"Driver", "2"}, {"Passenger", "4"}, {"Other seat", "8"}});
  renut.Function("vehicle._spawn", [mod_blueprints](std::string blueprint, std::string where, double x, double y,
                                                    double z, double yaw) -> int64_t {
    constexpr uint32_t kBuilderSize = 0x80, kBuilderLift = 0x35, kBuilderProfile = 0x40, kBuilderData = 0x60,
                       kBuilderEveryPart = 0x68;
    constexpr uint32_t kBlueprintBuilder = 0x821461B0, kBlueprintFileHeader = 8;
    // A name is a mod's vehicle ("mod: name" or "name"), else a path.
    std::string path = blueprint;
    for (const auto &[label, file] : mod_blueprints())
      if (label == blueprint || label.substr(label.find(": ") + 2) == blueprint)
        path = file;
    std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!renut::workshop::IsBlueprint(data))
      throw Error("'" + blueprint + "' isn't a vehicle blueprint (a mod's vehicles/*.blueprint, or its path)");
    const uint32_t world = RunningWorld();
    if (!world)
      throw Error("vehicles are spawned in a world");
    Place at = Where(where, x, y, z, yaw);
    RequireGame();
    // The builder's lift (sub_8254B478) puts the vehicle's lowest part at the spot's height. Near Banjo that's his
    // height, which slopes leave off the ground, so it goes onto the ground; a position given goes no lower than it.
    try {
      constexpr double kAbove = 6, kBelow = 40;  // not so high it lands on an awning
      const double f = RayFraction(LevelPhysics(), at.x, at.y + kAbove, at.z, at.x, at.y - kBelow, at.z, 0);
      if (f < 1.0) {
        const double ground = at.y + kAbove - (kAbove + kBelow) * f;
        at.y = where == "xyz" ? std::max(at.y, ground) : ground;
      }
    } catch (const Error &) {
      // No physics world to look in: the height it was given.
    }
    static uint32_t builder = 0;  // the builder, then its position and rotation
    if (!builder && !(builder = SystemAlloc(kBuilderSize + 32, 16)))
      throw Error("no guest memory");
    const uint32_t bytes = uint32_t(data.size() - kBlueprintFileHeader);
    const uint32_t copy = SystemAlloc(bytes, 16);
    if (!copy)
      throw Error("no guest memory");
    std::memcpy(Base() + copy, data.data() + kBlueprintFileHeader, bytes);
    const double half = at.yaw * 3.14159265358979 / 360.0;
    StoreVec4(builder + kBuilderSize, at.x, at.y, at.z, 1);
    StoreVec4(builder + kBuilderSize + 16, 0, std::sin(half), 0, std::cos(half));
    std::memset(Base() + builder, 0, kBuilderSize);
    InitVehicleBuilder(builder, world, builder + kBuilderSize, builder + kBuilderSize + 16);
    Store32(builder, kBlueprintBuilder);
    Base()[builder + kBuilderLift] = 1;  // onto the ground
    Store32(builder + kBuilderProfile, Load32(kPlayerProfile));
    Store32(builder + kBuilderData, copy);
    Store32(builder + kBuilderEveryPart, 1);
    uint32_t vehicle = 0;
    {
      SpawnGuard guard;
      vehicle = BuildVehicle(builder);
      SystemFree(copy);
      ThrowIfMissing(vehicle);
    }
    if (!vehicle)
      throw Error("the game didn't build it");
    EnableEntity(vehicle, 1);
    ShowEntity(vehicle);
    return vehicle;
  });
  renut.Function("vehicle._seat", [](int64_t vehicle, int64_t character, std::string who, int64_t seat) {
    const uint32_t v = Address(vehicle, "vehicle");
    const uint32_t a = Who(character, who);
    if (seat != 2 && seat != 4 && seat != 8)
      throw Error("a seat is the driver's (2), a passenger's (4) or another (8)");
    RequireGame();
    SeatActor(v, a, uint32_t(seat), 0, 1);  // on the vehicle's next pass
  });

  // The HUD (renut.hud).
  renut.Category("hud.", "HUD").Category("hud_", "HUD");
  renut.Choices("hud.Timer", {{"Count down", "down"}, {"Count up", "up"}, {"Hold", "hold"}});
  renut.Function("hud._message", [](std::string text, double seconds) {
    RequireGame();
    const HudOf of = Hud();
    ShowMessageBox(of.profile, Text16(text, 0), 0);
    std::lock_guard lock(g_hud_mutex);
    g_message_left = std::isfinite(seconds) && seconds > 0 ? seconds : 0;
  });
  renut.Function("hud._close", [] {
    RequireGame();
    CloseMessageBox(Hud().profile, 1);
    std::lock_guard lock(g_hud_mutex);
    g_message_left = 0;
  });
  renut.Function("hud._choice", [](std::string text) -> int64_t {
    static uint32_t keys = 0;  // the buttons' text keys, "confirm" and "cancel"
    static uint32_t next = 0;
    RequireGame();
    if (!keys && !(keys = SystemAlloc(16, 16)))
      throw Error("no guest memory");
    std::memcpy(Base() + keys, "confirm\0cancel\0", 15);
    const HudOf of = Hud();
    const uint32_t number = (++next & 0xFFFF) ? next & 0xFFFF : ++next & 0xFFFF;
    ShowChoiceBox(of.profile, Text16(text, 0), keys, 0, kChoiceToken | number, keys + 8, 0, kChoiceToken | number);
    std::lock_guard lock(g_hud_mutex);
    g_message_left = 0;
    return number;
  });
  renut.Function("hud._panel", [](bool timer, bool score) {
    static uint32_t params = 0;
    RequireGame();
    if (!params && !(params = SystemAlloc(kPanelParams + 16, 16)))
      throw Error("no guest memory");
    const HudOf of = Hud();
    std::memset(Base() + params, 0, kPanelParams + 16);
    Store32(params + kPanelShowTime, timer ? 1 : 0);
    Store32(params + kPanelShowScore, score ? 1 : 0);
    Store32(params + kPanelThresholds, params + kPanelParams);  // four thresholds, all 0
    {
      std::lock_guard lock(g_hud_mutex);
      Store32(params + kPanelScore, uint32_t(g_panel.value));
      StoreFloat(params + kPanelTime, g_panel.seconds);
    }
    // As the game's own opening does (sub_82553C78): the player's slot in the HUD's list is cleared first.
    Store32(of.last + 4 * (ProfileIndex(of.profile) + 8), 0);
    OpenScorePanel(of.container, of.profile, params);
    std::lock_guard lock(g_hud_mutex);
    g_panel.open = true;
    g_panel.timer = timer;
    g_panel.score = score;
  });
  renut.Function("hud._panel_close", [] {
    RequireGame();
    ClosePanels(Hud().profile);
    std::lock_guard lock(g_hud_mutex);
    g_panel = Panel{};
  });
  renut.Function("hud._timer", [](double seconds, std::string run) {
    if (!std::isfinite(seconds) || seconds < 0)
      throw Error("a timer's seconds are 0 or more");
    if (run != "down" && run != "up" && run != "hold")
      throw Error("a timer counts down, up, or holds");
    std::lock_guard lock(g_hud_mutex);
    g_panel.seconds = seconds;
    g_panel.rate = run == "down" ? -1 : run == "up" ? 1 : 0;
    g_panel.done = false;
  });
  renut.Function("hud._shown", []() -> int64_t {
    std::lock_guard lock(g_hud_mutex);
    return g_panel.open ? (g_panel.timer ? 1 : 0) | (g_panel.score ? 2 : 0) | 4 : 0;
  });
  renut.Function("hud._time", []() -> double {
    std::lock_guard lock(g_hud_mutex);
    return g_panel.seconds;
  });
  renut.Function("hud._counter", [](int64_t value, int64_t target) {
    std::lock_guard lock(g_hud_mutex);
    g_panel.value = value;
    g_panel.counter = target > 0 ? std::to_string(value) + "/" + std::to_string(target) : std::string();
  });

  // Trigger zones (renut.zones).
  renut.Category("zones.", "Zones").Category("zone_", "Zones");
  renut.Choices("zones.Shape", {{"Sphere", "sphere"}, {"Box", "box"}});
  renut.Choices("zones.Watch", {{"Banjo", "banjo"}, {"Banjo or his vehicle", "banjo,vehicle"},
                                {"His vehicle", "vehicle"}, {"Characters", "characters"}, {"Everything", "all"}});
  renut.Choices("zones.Zone", std::function([] {
    std::vector<std::pair<std::string, std::string>> list;
    std::lock_guard lock(g_zones_mutex);
    for (const auto &[name, zone] : g_zones)
      list.emplace_back(name, name);
    return list;
  }));
  renut.Choices("zones.World", std::function([] {
    std::vector<std::pair<std::string, std::string>> list = {{"Any world", ""}};
    for (uint32_t number = 1; number <= kWorldNumbers; ++number)
      if (const std::string name = WorldName(number); !name.empty())
        list.emplace_back(name, name);
    return list;
  }));
  renut.Function("zones._add", [](std::string name, std::string shape, std::string at, double x, double y, double z,
                                  double radius, double width, double height, double depth, std::string watch,
                                  std::string world) {
    if (name.empty())
      throw Error("a zone needs a name");
    if (shape != "sphere" && shape != "box")
      throw Error("a zone is a sphere or a box");
    Zone zone;
    zone.box = shape == "box";
    const Place place = Where(at.empty() ? "banjo" : at, x, y, z, 0);
    zone.at[0] = place.x, zone.at[1] = place.y, zone.at[2] = place.z;
    // A size left 0 (a node's pin) is 5 units.
    zone.size[0] = zone.box ? width : radius, zone.size[1] = height, zone.size[2] = depth;
    for (double &value : zone.size) {
      if (!std::isfinite(value) || value < 0)
        throw Error("a zone's sizes are 0 (5 units) or more");
      if (value == 0)
        value = 5;
    }
    zone.watch = WatchBits(watch);
    zone.world = std::move(world);
    std::lock_guard lock(g_zones_mutex);
    g_zones[name] = std::move(zone);
  });
  renut.Function("zones._move", [](std::string name, std::string at, double x, double y, double z) {
    const Place place = Where(at.empty() ? "banjo" : at, x, y, z, 0);
    std::lock_guard lock(g_zones_mutex);
    const auto found = g_zones.find(name);
    if (found == g_zones.end())
      throw Error("no zone '" + name + "'");
    found->second.at[0] = place.x, found->second.at[1] = place.y, found->second.at[2] = place.z;
  });
  renut.Function("zones._remove", [](std::string name) {
    std::lock_guard lock(g_zones_mutex);
    g_zones.erase(name);
  });
  renut.Function("zones._list", [] {
    std::string list;
    std::lock_guard lock(g_zones_mutex);
    for (const auto &[name, zone] : g_zones)
      list += name + "\n";
    return list;
  });
  renut.Function("zones._count", [](std::string name) -> int64_t {
    std::lock_guard lock(g_zones_mutex);
    const auto found = g_zones.find(name);
    return found != g_zones.end() ? int64_t(found->second.inside.size()) : 0;
  });
  renut.Function("zones._inside", [](std::string name, int64_t object) {
    std::lock_guard lock(g_zones_mutex);
    const auto found = g_zones.find(name);
    if (found == g_zones.end())
      return false;
    if (!object)
      return std::any_of(found->second.inside.begin(), found->second.inside.end(),
                         [](const auto &entry) { return std::string_view(entry.second) == "banjo"; });
    return found->second.inside.contains(uint32_t(object));
  });
  renut.Function("parts._aid_of", [](std::string name) -> int64_t { return PartAid(std::move(name)); });
  renut.Function("parts._owned", [](int64_t aid, bool available) -> int64_t {
    const uint32_t entry = InventoryEntry(uint32_t(aid));
    return entry ? int32_t(Load32(entry + (available ? kInventoryAvailable : kInventoryTotal))) : 0;
  });

  // Turrets and a part's axes.
  renut.Function("parts._axis", [](int64_t instance, int64_t row, int64_t axis) -> double {
    if (row < 0 || row > 2 || axis < 0 || axis > 2)
      throw Error("no such axis");
    return LoadFloat(Address(instance, "part") + kPartWorldMatrix + 16 * uint32_t(row) + 4 * uint32_t(axis));
  });
  renut.Function("parts._turret", [](int64_t instance, int64_t which) -> double {
    const uint32_t part = Weapon(instance);
    switch (which) {
    case 0: return LoadFloat(part + kWeaponYaw);
    case 1: return LoadFloat(part + kWeaponPitch);
    case 2: return LoadFloat(part + kWeaponPitchLimits);
    case 3: return LoadFloat(part + kWeaponPitchLimits + 4);
    case 4: return LoadFloat(part + kWeaponAmmoPerShot);
    }
    throw Error("no such weapon value");
  });
  // which: 0 the muzzle's position, 1 its direction; NaN before the weapon's first tick.
  renut.Function("parts._muzzle", [](int64_t instance, int64_t which, int64_t axis) -> double {
    const uint32_t part = Weapon(instance);
    if (which < 0 || which > 1 || axis < 0 || axis > 2)
      throw Error("no such muzzle value");
    if (!Load32(part + kWeaponMuzzleSet))
      return std::nan("");
    return LoadFloat(part + (which ? kWeaponMuzzleDirection : kWeaponMuzzle) + 4 * uint32_t(axis));
  });
  renut.Function("parts._aim", [](int64_t instance, double yaw, double pitch, double drawn_yaw) {
    const uint32_t part = Weapon(instance);
    if (!std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(drawn_yaw))
      throw Error("not a finite angle");
    constexpr double kPi = 3.14159265358979323846;
    yaw = std::remainder(yaw, 2 * kPi);
    drawn_yaw = std::remainder(drawn_yaw, 2 * kPi);
    const double a = LoadFloat(part + kWeaponPitchLimits) * kPi / 180;
    const double b = LoadFloat(part + kWeaponPitchLimits + 4) * kPi / 180;
    if (a != b)
      pitch = std::clamp(pitch, std::min(a, b), std::max(a, b));
    // The turret's own angles, and the copies drawn this frame (sub_821D0B30's last step), whose
    // yaw the seat's animation takes the other way round (seen in game).
    StoreFloat(part + kWeaponYaw, yaw);
    StoreFloat(part + kWeaponPitch, pitch);
    StoreFloat(part + kWeaponDrawnYaw, drawn_yaw);
    StoreFloat(part + kWeaponDrawnPitch, pitch);
    StoreFloat(part + kWeaponDrawnPitchDown, std::min(pitch, 0.0));
    StoreFloat(part + kWeaponDrawnPitchUp, std::max(pitch, 0.0));
  });

  // A part's own shape key weights (shape_weights.h), its model's table named by its model's aid.
  renut.Function("parts._shape_index", [](int64_t instance, std::string name) -> int64_t {
    const uint32_t part = Address(instance, "part");
    return renut::gpu::ShapeIndex(Load32(part + kPartModelAid), name);
  });
  renut.Function("parts._shape", [](int64_t instance, int64_t index) -> double {
    const uint32_t part = Address(instance, "part");
    return renut::gpu::InstanceShapeWeight(part, Load32(part + kPartModel), Load32(part + kPartModelAid), uint32_t(index));
  });
  renut.Function("parts._set_shape", [](int64_t instance, int64_t index, double weight) {
    const uint32_t part = Address(instance, "part");
    if (index < 0 || !std::isfinite(weight))
      throw Error("not a shape index or a finite weight");
    renut::gpu::SetInstanceShapeWeight(part, Load32(part + kPartModel), Load32(part + kPartModelAid), uint32_t(index),
                                       float(weight));
  });

  // Bones of a part's model (indices as NutTool's model-export lists them).
  renut.Function("parts._bone_count", [](int64_t instance) -> int64_t {
    const uint32_t model = Load32(Address(instance, "part") + kPartModel);
    return model ? ModelBoneCount(model) : 0;
  });
  renut.Function("parts._bone", [](int64_t instance, int64_t bone, double angle, double ax, double ay, double az) {
    const uint32_t part = Address(instance, "part");
    if (!t_ctx)
      throw Error("part:bone works inside the renut events (a part's or a vehicle's callbacks)");
    if (!std::isfinite(angle) || !std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az))
      throw Error("not a finite angle or axis");
    // The model instance and its bone matrices (uploaded for skinning) can be in physical memory,
    // above the heap.
    const uint32_t model = Load32(part + kPartModel), vehicle = Load32(part + kPartVehicle);
    if (!model || !Heap(vehicle))
      return;
    if (bone < 0 || bone >= int64_t(ModelBoneCount(model)))
      throw Error("the part's model has no bone " + std::to_string(bone));
    const uint32_t level = Load32(vehicle + kVehicleLevel);
    if (!Heap(level))
      return;
    const uint32_t graph = Load32(model + 4 * (Load32(level + kLevelView) + 3));
    ::PPCFunc *set = renut::game::GuestFunction(kSetBoneRotation);
    if (!graph || !set)
      return;
    t_ctx->r3.u64 = model;
    t_ctx->r4.u64 = graph;
    t_ctx->r5.u64 = uint64_t(bone);
    t_ctx->f1.f64 = angle;
    // A guest vector (x, y, z, w) sits reversed in the host register (lvx swaps all 16 bytes).
    t_ctx->v1.f32[3] = float(ax);
    t_ctx->v1.f32[2] = float(ay);
    t_ctx->v1.f32[1] = float(az);
    t_ctx->v1.f32[0] = 0.0f;
    set(*t_ctx, t_base);
  });

  // renut.fx.Effect.<name>: every known effect's id, which the node editor offers as Play's choices.
  std::vector<std::pair<std::string, int64_t>> effects;
  for (const KnownEffect &effect : kKnownEffects)
    effects.emplace_back(effect.name, effect.id);
  renut.Enum("fx.Effect", std::move(effects));
  renut.Function("fx._play", [](std::string effect, std::string where, double x, double y, double z, int64_t vehicle,
                                double seconds) {
    if (!t_ctx)
      throw Error("renut.fx.play works inside the renut events (a part's or a vehicle's callbacks)");
    // Only its level is used: without a vehicle, Banjo's.
    const uint32_t entity = vehicle ? Address(vehicle, "vehicle") : PlayerAvatar();
    if (!entity)
      throw Error("renut.fx.play: no vehicle given and Banjo isn't in a level to play it in");
    const Place at = Where(where.empty() ? "banjo" : where, x, y, z, 0);
    std::string error;
    const auto played =
        renut::game::part_scripts::PlayEffect(*t_ctx, t_base, entity, effect, at.x, at.y, at.z, seconds, &error);
    if (!played)
      throw Error(error);
    return *played;
  });
  renut.Function("set_amount", [](double amount) {
    if (!t_amount)
      throw Error("renut.set_amount works inside a damage event");
    if (!std::isfinite(amount))
      throw Error("not a finite amount");
    *t_amount = std::max(0.0, amount);
  });

  // A ray through the running world's physics, on foot or driving: how far along it hits (1: nothing).
  renut.Function("world._ray_level", [](double fx, double fy, double fz, double tx, double ty, double tz) -> double {
    const uint32_t havok = LevelPhysics();
    RequireGame();
    return RayFraction(havok, fx, fy, fz, tx, ty, tz, 0);
  });

  // The camera: which way its matrix's third row faces (+1 where it looks, -1 the other, 0 not known yet); a shake.
  renut.Function("camera._facing", []() -> double {
    return g_camera_known.load(std::memory_order_acquire) ? g_camera_frame.facing : 0.0;
  });
  renut.Function("camera._shake", [](double strength, double seconds) {
    if (!std::isfinite(strength) || !std::isfinite(seconds) || strength < 0 || seconds < 0)
      throw Error("a shake's strength and seconds are 0 or more");
    std::lock_guard lock(g_camera_mutex);
    g_shake = {std::min(strength, 50.0), seconds,
               std::chrono::steady_clock::now() +
                   std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds))};
  });

  // The players: each one's avatar (1 to 4), as split screen has them.
  renut.Function("player._avatar_of", [](int64_t number) -> int64_t { return PlayerAvatarOf(number); });

  // Characters through the game's own messages: velocity, a push, standing on the floor, scale, getting out.
  const auto velocity = [](uint32_t actor, int64_t axis) -> double {
    if (axis < 0 || axis > 2)
      throw Error("axis out of range");
    return LoadFloat(SendObjectMessage(actor, kMsgGetVelocity) + kMessagePayload + 4 * uint32_t(axis));
  };
  renut.Function("actors._velocity", [velocity](int64_t character, std::string who, int64_t axis) -> double {
    return velocity(Who(character, who), axis);
  });
  // As sub_82481830 sends it: the change of velocity, 1, who it's from (the character itself), 0.
  renut.Function("actors._push", [](int64_t character, std::string who, double x, double y, double z) {
    const uint32_t a = Who(character, who);
    SendObjectMessage(a, kMsgAddVelocity, [&](uint32_t message) {
      StoreVec4(message + kMessagePayload, x, y, z, 0);
      Store32(message + 32, 1);
      Store32(message + 36, a);
    });
  });
  renut.Function("actors._on_ground", [](int64_t character, std::string who) {
    return Load32(SendObjectMessage(Who(character, who), kMsgContactingFloor) + 4) != 0;
  });
  renut.Function("actors._scale", [](int64_t character, std::string who) -> double {
    return LoadFloat(SendObjectMessage(Who(character, who), kMsgGetScale) + 4);
  });
  renut.Function("actors._exit_vehicle", [](int64_t character, std::string who) {
    const uint32_t a = Who(character, who);
    const bool driving = Heap(Load32(a + kActorVehicle)) || (a == PlayerAvatar() && Heap(Load32(a + kAvatarVehicle)));
    if (driving)
      SendObjectMessage(a, kMsgKnockedOut);
    return driving;
  });
  renut.Function("actors._yaw", [](int64_t character, std::string who) -> double { return EntityYaw(Who(character, who)); });
  renut.Function("actors._body_now", [](int64_t character, std::string who) { return BodyStateNow(Who(character, who)); });

  // Any object: moved (message 46, as a character is placed), turned, its yaw.
  renut.Function("objects._move", [](int64_t object, double x, double y, double z) {
    const uint32_t o = Address(object, "object");
    if (!InWorld(o))
      throw Error("not an object in this world");
    RequireGame();
    const uint32_t s = Scratch();
    StoreVec4(s, x, y, z, 1);
    MoveActor(o, s, 0, 0, 0);
  });
  renut.Function("objects._turn", [](int64_t object, double yaw, double pitch, double roll) {
    const uint32_t o = Address(object, "object");
    if (!InWorld(o))
      throw Error("not an object in this world");
    RequireGame();
    constexpr double kRadians = 3.14159265358979 / 180.0;
    const uint32_t s = Scratch();
    StoreVec4(s, pitch * kRadians, yaw * kRadians, roll * kRadians, 0);
    SetRotation(o, s, 1);
  });
  renut.Function("objects._yaw", [](int64_t object) -> double {
    const uint32_t o = Address(object, "object");
    if (!InWorld(o))
      throw Error("not an object in this world");
    return EntityYaw(o);
  });

  // Vehicles: every one in the game, removing one, its rotation and its name.
  renut.Function("vehicle._list", [] {
    std::string list;
    std::lock_guard lock(g_vehicles_mutex);
    for (const uint32_t vehicle : g_vehicles)
      list += std::to_string(vehicle) + "\n";
    return list;
  });
  renut.Function("vehicle._remove", [](int64_t vehicle) {
    const uint32_t v = Address(vehicle, "vehicle");
    {
      std::lock_guard lock(g_vehicles_mutex);
      if (!g_vehicles.contains(v))
        throw Error("not a vehicle in the game");
    }
    const uint32_t avatar = PlayerAvatar();
    if (avatar && Load32(avatar + kAvatarVehicle) == v)
      throw Error("the player is in it: get him out first (renut.player.exit_vehicle)");
    RequireGame();
    RemoveObject(v, 0);
  });
  renut.Function("vehicle._rotation", [](int64_t vehicle, int64_t which) -> double {
    if (which < 0 || which > 3)
      throw Error("a quaternion has 4 values");
    const uint32_t motion = Motion(vehicle);
    float m[3][3];
    for (uint32_t column = 0; column < 3; ++column)
      for (uint32_t row = 0; row < 3; ++row)
        m[row][column] = float(LoadFloat(motion + uint32_t(kMotionRotation) + 16 * column + 4 * row));
    return MatrixQuaternion(m)[size_t(which)];
  });
  // Its rotation matrix and both of its swept rotations, and its centres of mass turned with it about its position.
  renut.Function("vehicle._set_rotation", [](int64_t vehicle, double x, double y, double z, double w) {
    const double length = std::sqrt(x * x + y * y + z * z + w * w);
    if (!std::isfinite(length) || length < 1e-6)
      throw Error("not a rotation (a quaternion x, y, z, w)");
    x /= length, y /= length, z /= length, w /= length;
    const uint32_t body = Body(vehicle);
    const uint32_t motion = body + kBodyMotion;
    RequireGame();
    const std::array<V3, 3> columns = QuaternionColumns(float(x), float(y), float(z), float(w));
    for (uint32_t i = 0; i < 3; ++i)
      StoreVec4(motion + uint32_t(kMotionRotation) + 16 * i, columns[i].x, columns[i].y, columns[i].z, 0);
    for (const uint32_t at : kMotionRotations)
      StoreVec4(motion + at, x, y, z, w);
    const V3 local = LoadV3(motion + kMotionLocalCentre), p = LoadV3(motion);
    const V3 centre{p.x + columns[0].x * local.x + columns[1].x * local.y + columns[2].x * local.z,
                    p.y + columns[0].y * local.x + columns[1].y * local.y + columns[2].y * local.z,
                    p.z + columns[0].z * local.x + columns[1].z * local.y + columns[2].z * local.z};
    for (const uint32_t at : kMotionCentres) {
      StoreFloat(motion + at, centre.x);
      StoreFloat(motion + at + 4, centre.y);
      StoreFloat(motion + at + 8, centre.z);
    }
    ActivateBody(body);
  });
  renut.Function("vehicle._set_name", [](int64_t vehicle, std::string name) {
    StoreUtf16(Address(vehicle, "vehicle") + kVehicleName, name, kVehicleNameLength);
  });

  // The game: saving, pausing, its speed.
  renut.Function("game._save", [] {
    const uint32_t profile = Load32(kPlayerProfile);
    if (!Heap(profile))
      throw Error("no save is loaded");
    RequireGame();
    return (SaveGame(profile) & 0xFF) != 0;
  });
  // The pause menu, as a pad's Start opens it (sub_8255B428 (profile, profile)) and its Resume closes it.
  renut.Function("game._set_paused", [](bool paused) {
    const uint32_t profile = Load32(kPlayerProfile);
    if (!Heap(profile))
      throw Error("there's no player to pause for");
    RequireGame();
    const bool now = Load32(kPauseMenu) != 0xFFFFFFFFu;
    if (paused && !now)
      CallWithFrame(0x8255B428, {profile, profile});
    else if (!paused && now)
      CallWithFrame(0x8255B598, {0, 0});
  });
  renut.Function("game._set_time_scale", [](double scale) {
    if (!std::isfinite(scale) || scale < 0.05 || scale > 4)
      throw Error("a time scale is 0.05 to 4");
    float base = g_frame_delta.load();
    if (base == 0.0f) {
      base = float(LoadFloat(kFrameDelta));
      if (!(base > 0.0f && base < 1.0f))
        throw Error("the game's frame time isn't set up yet");
      g_frame_delta.store(base);
    }
    g_time_scale.store(scale);
    StoreFloat(kFrameDelta, base * scale);
    StoreFloat(kFrameDeltaCopy, base * scale);
  });
  renut.Function("game._time_scale", []() -> double { return g_time_scale.load(); });

  // The game's sound cues by id (as the Stereo plays its songs), without a position.
  renut.Function("sound._cue", [](int64_t cue, double volume) -> int64_t {
    if (cue <= 0 || cue > 0xFFFFFFFFll)
      throw Error("a cue is the game's 32-bit cue id");
    if (!std::isfinite(volume) || volume < 0)
      throw Error("a volume is 0 or more");
    RequireGame();
    // Records of cues that ended go first, so a cue played often doesn't use them up.
    for (auto it = g_cues.begin(); it != g_cues.end();) {
      if (CuePlaying(it->second) & 0xFF) {
        ++it;
        continue;
      }
      SystemFree(it->second);
      it = g_cues.erase(it);
    }
    if (g_cues.size() >= kMaxCues)
      throw Error("64 cues are playing already");
    const uint32_t record = SystemAlloc(kCueRecordBytes, 16);
    if (!record)
      throw Error("no guest memory");
    std::memset(Base() + record, 0, kCueRecordBytes);
    Store32(record + kCueRecordId, uint32_t(cue));
    const uint32_t handle = PlayCue(record + kCueRecordId, 0, 0, std::min(volume, 4.0));
    if (!handle) {
      SystemFree(record);
      return 0;
    }
    Store32(record, handle);
    Store32(record + 4, 0xFFFFFFFFu);
    Store32(record + 8, 0xFFFFFFFFu);
    g_cues[g_next_cue] = record;
    return g_next_cue++;
  });
  renut.Function("sound._cue_stop", [](int64_t handle) {
    const auto found = g_cues.find(handle);
    if (found == g_cues.end())
      return;
    RequireGame();
    StopCue(found->second, 1, 0);
  });
  renut.Function("sound._cue_playing", [](int64_t handle) {
    const auto found = g_cues.find(handle);
    if (found == g_cues.end())
      return false;
    RequireGame();
    return (CuePlaying(found->second) & 0xFF) != 0;
  });

  // A player's pad, as the game read it this frame: which 0 held, 1 pressed, 2 released (buttons); 3-8 the left
  // stick x, y, the right stick x, y, the left and right triggers.
  renut.Function("pad._get", [](int64_t player, int64_t which) -> double {
    if (which < 0 || which > 8)
      throw Error("no such pad value");
    uint8_t entry[kPadEntry];
    if (!PadEntry(player, entry))
      return 0;
    constexpr uint32_t kAt[] = {12, 0, 4, 16, 20, 24, 28, 32, 36};
    const uint32_t at = kAt[which];
    const uint32_t bits = uint32_t(entry[at]) << 24 | uint32_t(entry[at + 1]) << 16 | uint32_t(entry[at + 2]) << 8 | entry[at + 3];
    if (which < 3)
      return double(bits);
    float f;
    std::memcpy(&f, &bits, 4);
    return std::isfinite(f) ? f : 0.0;
  });

  g_events = Events{renut.Event("part_spawn"),         renut.Event("part_destroy"),
                    renut.Event("part_attach"),        renut.Event("part_detach"),
                    renut.Event("part_tick"),          renut.Event("part_physics"),
                    renut.Event("part_power"),         renut.Event("part_button"),
                    renut.Event("part_damage"),        renut.Event("part_break"),
                    renut.Event("vehicle_spawn"),      renut.Event("vehicle_removed"),
                    renut.Event("vehicle_tick"),       renut.Event("vehicle_damage"),
                    renut.Event("vehicle_health"),     renut.Event("vehicle_part_added"),
                    renut.Event("vehicle_part_removed"), renut.Event("vehicle_collide"),
                    renut.Event("vehicle_enter"),        renut.Event("vehicle_exit"),
                    renut.Event("world_enter"),          renut.Event("world_leave"),
                    renut.Event("challenge_start"),      renut.Event("challenge_finish"),
                    renut.Event("game_paused"),          renut.Event("game_resumed"),
                    renut.Event("crate_award"),          renut.Event("game_tick"),
                    renut.Event("zone_enter"),           renut.Event("zone_exit"),
                    renut.Event("hud_choice"),           renut.Event("hud_timer_done"),
                    renut.Event("actor_hit"),            renut.Event("actor_defeated"),
                    renut.Event("collected"),            renut.Event("player_state"),
                    renut.Event("game_save"),            renut.Event("game_load"),
                    renut.Event("part_use"),             renut.Event("object_spawn"),
                    renut.Event("object_removed")};
  renut.Lua(kLua);
  renut.Lua(kWaitLua);
}

void Spawn(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance) {
  if (g_events)
    Fire(&ctx, base, g_events->part_spawn, part.id, int64_t(instance));
}

void Destroy(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance) {
  if (g_events)
    Fire(&ctx, base, g_events->part_destroy, part.id, int64_t(instance));
  ReleasePools(instance);
  renut::gpu::ReleaseShapeInstance(instance);
}

void Attach(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle) {
  if (g_events)
    Fire(&ctx, base, g_events->part_attach, part.id, int64_t(instance), int64_t(vehicle));
}

void Detach(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle) {
  if (g_events)
    Fire(&ctx, base, g_events->part_detach, part.id, int64_t(instance), int64_t(vehicle));
  // Its projectiles know the vehicle they were made for; another vehicle makes new ones.
  ReleasePools(instance);
}

void Tick(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle, double dt) {
  for (ProjectilePool &pool : g_pools)
    if (pool.part == instance && pool.age < kPoolReadyTicks)
      ++pool.age;
  if (g_events)
    Fire(&ctx, base, g_events->part_tick, part.id, int64_t(instance), int64_t(vehicle), dt);
}

void Physics(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle,
             double dt) {
  if (g_events)
    Fire(&ctx, base, g_events->part_physics, part.id, int64_t(instance), int64_t(vehicle), dt);
}

void Power(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle, bool on) {
  if (g_events)
    Fire(&ctx, base, g_events->part_power, part.id, int64_t(instance), int64_t(vehicle), on);
}

void Button(PPCContext &ctx, uint8_t *base, const CustomPart &part, uint32_t instance, uint32_t vehicle,
            bool down) {
  if (g_events)
    Fire(&ctx, base, g_events->part_button, part.id, int64_t(instance), int64_t(vehicle), down);
}

void GameTick(PPCContext &ctx, uint8_t *base) {
  ZonesTick(ctx, base);
  HudTick(ctx, base);
  if (g_events) {
    // A save is up once player 1's counters are (a new one when they move): renut.game_load.
    const uint32_t profile = Load32(kPlayerProfile);
    const uint32_t counters = Heap(profile) ? Load32(profile + kProfileCounters) : 0;
    if (counters != g_loaded_counters) {
      g_loaded_counters = counters;
      if (Heap(counters))
        Fire(&ctx, base, g_events->game_load);
    }
    // Player 1's body state changing: renut.player_state (new, old).
    const uint32_t avatar = PlayerAvatar();
    const std::string state = avatar ? BodyStateNow(avatar) : std::string();
    if (avatar != g_player_state_avatar) {
      g_player_state_avatar = avatar;
      g_player_state = state;
    } else if (state != g_player_state) {
      const std::string old = std::exchange(g_player_state, state);
      Fire(&ctx, base, g_events->player_state, state, old);
    }
    Fire(&ctx, base, g_events->game_tick);
  }
}

bool QuietFailedOpen(uint32_t aid) {
  if (g_spawning.load() <= 0)
    return false;
  uint32_t none = 0;
  g_missing_asset.compare_exchange_strong(none, aid);
  return true;
}

bool Spawning() { return g_spawning.load() > 0; }

bool ModSpawned(uint32_t object) {
  std::lock_guard lock(g_kinds_mutex);
  return g_mod_spawned.contains(object);
}

} // namespace renut::game::part_api

// --- Events for every vehicle and part, from the game's own functions ------------
// Raw hooks: the original runs with the context the game called it with, every argument kept.

namespace part_api = renut::game::part_api;

// Boot (sub_82284968, after "li r3, 0x400"): the pool every loaded model takes an entry of, 1024 x 76 bytes. A world
// bundle loaded for a spawned character emptied it (sub_822DA088 then wrote through null); entries are u16 indices.
void ModelPool_size(PPCRegister &r3) { r3.u64 = 4096; }

// u32 sub_825CA310(box, pressed element, -, -): a message box's button. A reNut choice's box (its arguments
// kChoiceToken | number) notes which was pressed, for renut.hud_choice on the next tick; it closes as without one.
uint32_t BoxButton_hook(uint32_t box, uint32_t element, uint32_t a3, uint32_t a4) {
  namespace api = part_api;
  if (api::Heap(box)) {
    const uint32_t token = api::Load32(box + api::kBoxArgA);
    if ((token & 0xFFFF0000u) == api::kChoiceToken && api::Load32(box + api::kBoxArgB) == token) {
      const bool swapped = api::Load32(box + api::kBoxSwapped) != 0;
      const uint32_t a = api::Load32(box + (swapped ? api::kBoxButtonB : api::kBoxButtonA));
      const bool b = !swapped && api::Load32(box + api::kBoxTwoChoices) && element == api::Load32(box + api::kBoxButtonB);
      if (element == a || b) {
        std::lock_guard lock(api::g_hud_mutex);
        api::g_answers.emplace_back(int64_t(token & 0xFFFF), element == a);
      }
    }
  }
  return BoxButton(box, element, a3, a4);
}
REX_HOOK(sub_825CA310, BoxButton_hook);

// u32 sub_822B0BC8(entity, &model aid, world or 0): makes an entity's model; Banjo's aid is swapped for the call. Raw,
// so it runs in the game's context: sub_82559790's camera later reads the vector registers it leaves (v40 | v72).
REX_HOOK_RAW(sub_822B0BC8) {
  namespace api = part_api;
  const uint32_t at = ctx.r4.u32;
  const uint32_t world = ctx.r5.u32;
  const uint32_t aid = api::Heap(at) ? api::Load32(at) : 0;
  const bool banjo =
      aid && std::find(std::begin(api::kBanjoModels), std::end(api::kBanjoModels), aid) != std::end(api::kBanjoModels);
  const uint32_t model = banjo ? api::PlayerModelIn(api::Heap(world) ? world : api::RunningWorld()) : 0;
  if (!model) {
    __imp__sub_822B0BC8(ctx, base);
    return;
  }
  api::Store32(at, model);
  __imp__sub_822B0BC8(ctx, base);
  api::Store32(at, aid);
}

// (body, hit, out): every hit a character's body gets (a vtable method). One it took fires renut.actor_hit with the
// character (body + 48), the attacker (hit + 68, else its source at + 64) and the damage (out + 0).
REX_HOOK_RAW(sub_823CD070) {
  const uint32_t body = ctx.r3.u32, hit = ctx.r4.u32, out = ctx.r5.u32;
  __imp__sub_823CD070(ctx, base);
  if (!part_api::g_events || !part_api::Heap(out) || !part_api::Load32(out + 4) || !part_api::Heap(body))
    return;
  uint32_t attacker = part_api::Heap(hit) ? part_api::Load32(hit + 68) : 0;
  if (!attacker && part_api::Heap(hit))
    attacker = part_api::Load32(hit + 64);
  part_api::Fire(&ctx, base, part_api::g_events->actor_hit, int64_t(part_api::Load32(body + 48)), int64_t(attacker),
                 double(part_api::LoadFloat(out)));
}

// (collectable, world): an entityPropsCollectable's tick; + 3008 goes 0 to 1 as something collects it (msg 252).
// renut.collected with it and its kind.
REX_HOOK_RAW(sub_824400C0) {
  constexpr uint32_t kCollected = 3008;
  const uint32_t object = ctx.r3.u32;
  const bool before = part_api::Heap(object) && part_api::Load32(object + kCollected);
  __imp__sub_824400C0(ctx, base);
  if (before || !part_api::g_events || !part_api::Heap(object) || !part_api::Load32(object + kCollected))
    return;
  std::string kind;
  {
    std::lock_guard lock(part_api::g_kinds_mutex);
    kind = part_api::ActorName(object);
  }
  part_api::Fire(&ctx, base, part_api::g_events->collected, int64_t(object), kind);
}

// (actor): a character dies, whichever way. renut.actor_defeated with it and its killer (its body's + 0x44C).
REX_HOOK_RAW(sub_823A31B8) {
  const uint32_t actor = ctx.r3.u32;
  __imp__sub_823A31B8(ctx, base);
  if (!part_api::g_events || !part_api::Heap(actor))
    return;
  const uint32_t body = part_api::Load32(actor + part_api::kActorBody);
  const uint32_t killer = part_api::Heap(body) ? part_api::Load32(body + 0x44C) : 0;
  part_api::Fire(&ctx, base, part_api::g_events->actor_defeated, int64_t(actor), int64_t(killer));
}

// init(vehicle, init data, ...): a vehicle's descriptor slot 1.
REX_HOOK_RAW(sub_825DBA20) {
  const uint32_t vehicle = ctx.r3.u32;
  __imp__sub_825DBA20(ctx, base);
  if (part_api::Heap(vehicle)) {
    std::lock_guard lock(part_api::g_vehicles_mutex);
    part_api::g_vehicles.insert(vehicle);
  }
  if (part_api::g_events)
    part_api::Fire(&ctx, base, part_api::g_events->vehicle_spawn, int64_t(vehicle));
}

// (vehicle, level): it leaves the level.
REX_HOOK_RAW(sub_825EF480) {
  {
    std::lock_guard lock(part_api::g_vehicles_mutex);
    part_api::g_vehicles.erase(ctx.r3.u32);
  }
  if (part_api::g_events)
    part_api::Fire(&ctx, base, part_api::g_events->vehicle_removed, int64_t(ctx.r3.u32));
  __imp__sub_825EF480(ctx, base);
}

// gameSaveSave (profile): a save of the game begins (1) or doesn't (0). renut.game_save once it has.
REX_HOOK_RAW(rex_gameSaveSave) {
  REXFABRIC_ORIGINAL(rex_gameSaveSave)(ctx, base);
  if ((ctx.r3.u32 & 0xFF) && part_api::g_events)
    part_api::Fire(&ctx, base, part_api::g_events->game_save);
}

// (part, vehicle, control): a part's button goes down or up, the driver's or a script's. renut.part_use.
REX_HOOK_RAW(sub_8260FC20) {
  const uint32_t part = ctx.r3.u32, control = ctx.r5.u32;
  __imp__sub_8260FC20(ctx, base);
  if (part_api::g_events)
    part_api::Fire(&ctx, base, part_api::g_events->part_use, int64_t(part), int64_t(control), true);
}

REX_HOOK_RAW(sub_8260FD50) {
  const uint32_t part = ctx.r3.u32, control = ctx.r5.u32;
  __imp__sub_8260FD50(ctx, base);
  if (part_api::g_events)
    part_api::Fire(&ctx, base, part_api::g_events->part_use, int64_t(part), int64_t(control), false);
}

// (object, 0): an object is removed, with what it holds. renut.object_removed before it goes.
REX_HOOK_RAW(sub_82384340) {
  const uint32_t object = ctx.r3.u32;
  if (part_api::g_events && part_api::Heap(object) && part_api::g_events->object_removed.HasListeners()) {
    std::string kind;
    {
      std::lock_guard lock(part_api::g_kinds_mutex);
      kind = part_api::ActorName(object);
    }
    part_api::Fire(&ctx, base, part_api::g_events->object_removed, int64_t(object), kind);
  }
  __imp__sub_82384340(ctx, base);
}

// (vehicle, -): after its parts' physics steps, each frame. It takes no dt: it reads its frame time
// from vehicle + 0xAAC (lfs f31, 0xAAC(r3)), so f1 on entry is whatever the caller left there.
REX_HOOK_RAW(sub_821ECA98) {
  constexpr uint32_t kVehicleFrameTime = 0xAAC;
  const uint32_t vehicle = ctx.r3.u32;
  uint32_t bits = 0;
  if (vehicle)
    std::memcpy(&bits, base + vehicle + kVehicleFrameTime, 4);
  bits = rex::byte_swap(bits);
  float frame_time;
  std::memcpy(&frame_time, &bits, 4);
  const double dt = std::isfinite(frame_time) ? double(frame_time) : 0.0;
  __imp__sub_821ECA98(ctx, base);
  if (part_api::g_events)
    part_api::Fire(&ctx, base, part_api::g_events->vehicle_tick, int64_t(vehicle), dt);
}

// (vehicle, double amount, -, part or 0, attacker, flag): vehicle-level damage.
REX_HOOK_RAW(sub_825D9C68) {
  double amount = ctx.f1.f64;
  if (part_api::g_events)
    part_api::FireDamage(ctx, base, part_api::g_events->vehicle_damage, amount, int64_t(ctx.r3.u32),
                         int64_t(ctx.r5.u32), int64_t(ctx.r6.u32));
  ctx.f1.f64 = amount;
  __imp__sub_825D9C68(ctx, base);
}

// (vehicle, double health).
REX_HOOK_RAW(sub_825D9F80) {
  const uint32_t vehicle = ctx.r3.u32;
  const double health = ctx.f1.f64;
  __imp__sub_825D9F80(ctx, base);
  if (part_api::g_events)
    part_api::Fire(&ctx, base, part_api::g_events->vehicle_health, int64_t(vehicle), health);
}

// (vehicle, part, packed cell, orientation, ...): whether it went on.
REX_HOOK_RAW(sub_825ED3E0) {
  const uint32_t vehicle = ctx.r3.u32, part = ctx.r4.u32;
  __imp__sub_825ED3E0(ctx, base);
  if ((ctx.r3.u32 & 0xFF) && part_api::g_events)
    part_api::Fire(&ctx, base, part_api::g_events->vehicle_part_added, int64_t(vehicle), int64_t(part));
}

// (vehicle, &impact): which side a hit came from. From the contact pass (kImpactReturn) it's the frame's strongest
// hit, the one the game sends as VehicleImpact; its other caller is left alone.
REX_HOOK_RAW(sub_82216C78) {
  const bool impact = ctx.lr == part_api::kImpactReturn;
  const uint32_t vehicle = ctx.r3.u32, push = ctx.r4.u32, record = ctx.r30.u32;
  __imp__sub_82216C78(ctx, base);
  if (!impact || !part_api::g_events || !part_api::Heap(vehicle) || !part_api::Heap(push) || !part_api::Heap(record))
    return;
  const double strength = part_api::LoadFloat(record + part_api::kContactStrength);
  const double x = part_api::LoadFloat(push), y = part_api::LoadFloat(push + 4), z = part_api::LoadFloat(push + 8);
  const uint32_t part = part_api::Load32(record + part_api::kContactPart), other = part_api::Load32(record + part_api::kContactOther);
  part_api::Fire(&ctx, base, part_api::g_events->vehicle_collide, int64_t(vehicle),
                 int64_t(part_api::Heap(part) ? part : 0), std::isfinite(strength) ? strength : 0.0, x, y, z,
                 int64_t(part_api::Heap(other) ? other : 0));
}

// (): a world becomes the running one (kRunningWorld).
REX_HOOK_RAW(sub_82289908) {
  __imp__sub_82289908(ctx, base);
  const uint32_t world = part_api::RunningWorld();
  if (!world || !part_api::g_events)
    return;
  const uint32_t number = part_api::Load32(world + part_api::kWorldNumber);
  part_api::Fire(&ctx, base, part_api::g_events->world_enter, int64_t(number),
                 int64_t(part_api::Load32(world + part_api::kWorldScene)), part_api::WorldName(number));
}

// (app scene stack entry): its world (+4) is torn down. A mod's camera goes with it.
REX_HOOK_RAW(sub_82289C30) {
  {
    std::lock_guard lock(part_api::g_camera_mutex);
    part_api::g_camera.active = false;
  }
  {
    std::lock_guard lock(part_api::g_kinds_mutex);
    part_api::g_made.clear();  // its objects go with it
    part_api::g_made_aid.clear();
    part_api::g_mod_spawned.clear();
    part_api::g_world_bundles.clear();  // they unload with it
  }
  {
    std::lock_guard lock(part_api::g_zones_mutex);
    for (auto &[name, zone] : part_api::g_zones)
      zone.inside.clear();  // no exits: the world went
  }
  {
    std::lock_guard lock(part_api::g_hud_mutex);
    part_api::g_panel = part_api::Panel{};  // the HUD goes with the world
    part_api::g_message_left = 0;
  }
  part_api::g_last_spawned.store(0);
  const uint32_t world = part_api::Heap(ctx.r3.u32) ? part_api::Load32(ctx.r3.u32 + 4) : 0;
  if (part_api::Heap(world) && part_api::g_events) {
    const uint32_t number = part_api::Load32(world + part_api::kWorldNumber);
    part_api::Fire(&ctx, base, part_api::g_events->world_leave, int64_t(number),
                   int64_t(part_api::Load32(world + part_api::kWorldScene)), part_api::WorldName(number));
  }
  __imp__sub_82289C30(ctx, base);
}

// avatarSetVehicle (avatar, vehicle or 0, seat part): an avatar, the player or anyone, gets in or out. Before it
// runs, the avatar's +0xC3C is still the vehicle it's leaving.
REX_HOOK_RAW(sub_823D0AF0) {
  const uint32_t avatar = ctx.r3.u32, vehicle = ctx.r4.u32;
  const uint32_t before = part_api::Heap(avatar) ? part_api::Load32(avatar + part_api::kAvatarVehicle) : 0;
  __imp__sub_823D0AF0(ctx, base);
  if (!part_api::g_events || before == vehicle)
    return;
  const bool player = avatar && avatar == part_api::PlayerAvatar();
  if (part_api::Heap(before))
    part_api::Fire(&ctx, base, part_api::g_events->vehicle_exit, int64_t(before), int64_t(avatar), player);
  if (part_api::Heap(vehicle))
    part_api::Fire(&ctx, base, part_api::g_events->vehicle_enter, int64_t(vehicle), int64_t(avatar), player);
}

// (challengeMan, &aid, flag): a challenge begins; its aid is at challengeMan + 12 after.
REX_HOOK_RAW(sub_824FDC38) {
  const uint32_t man = ctx.r3.u32;
  REXFABRIC_ORIGINAL(sub_824FDC38)(ctx, base);
  if (!part_api::g_events || !part_api::Heap(man))
    return;
  if (const uint32_t challenge = part_api::Load32(man + part_api::kChallengeRunning))
    part_api::Fire(&ctx, base, part_api::g_events->challenge_start, int64_t(challenge));
}

// (challengeMan): from challengeManTick, settles a finished challenge (state 14 to 15) with player 1's outcome.
REX_HOOK_RAW(sub_82503568) {
  const uint32_t man = ctx.r3.u32;
  const uint32_t before = part_api::Heap(man) ? part_api::Load32(man + part_api::kChallengeState) : 0;
  REXFABRIC_ORIGINAL(sub_82503568)(ctx, base);
  if (!part_api::g_events || before != part_api::kChallengeSettling ||
      part_api::Load32(man + part_api::kChallengeState) != part_api::kChallengeSettled)
    return;
  part_api::Fire(&ctx, base, part_api::g_events->challenge_finish,
                 int64_t(part_api::Load32(man + part_api::kChallengeRunning)),
                 int64_t(part_api::Load32(man + part_api::kChallengeOutcome)));
}

// Opens the pause menu (its handle at kPauseMenu); closes it.
REX_HOOK_RAW(sub_8255B428) {
  __imp__sub_8255B428(ctx, base);
  if (part_api::g_events && part_api::Load32(part_api::kPauseMenu) != 0xFFFFFFFFu)
    part_api::Fire(&ctx, base, part_api::g_events->game_paused);
}

REX_HOOK_RAW(sub_8255B598) {
  const bool was_paused = part_api::Load32(part_api::kPauseMenu) != 0xFFFFFFFFu;
  __imp__sub_8255B598(ctx, base);
  if (part_api::g_events && was_paused)
    part_api::Fire(&ctx, base, part_api::g_events->game_resumed);
}

// (): the view from the current camera node, once a frame. A mod's camera (renut.camera.set) stands in the player's
// view's node for this call only, so the game's camera carries on from where it was.
REX_HOOK_RAW(rex_meCalculateView) {
  using namespace part_api;
  const uint32_t node = Load32(kCurrentCamera);
  const bool player_view = Heap(node) && node == Load32(kViewportCamera);
  CameraOverride wanted;
  std::array<float, 3> shake{};
  {
    std::lock_guard lock(g_camera_mutex);
    wanted = g_camera;
    if (player_view)
      shake = ShakeOffset();
  }
  const bool shaking = shake[0] != 0 || shake[1] != 0 || shake[2] != 0;
  if (!player_view || !wanted.active || !g_camera_known.load(std::memory_order_acquire)) {
    // A shake moves the game's own camera for this view only.
    uint8_t kept[16];
    if (shaking) {
      std::memcpy(kept, base + node + kNodePosition, sizeof(kept));
      for (int i = 0; i < 3; ++i)
        StoreFloat(node + kNodePosition + 4 * i, LoadFloat(node + kNodePosition + 4 * i) + shake[size_t(i)]);
    }
    __imp__rex_meCalculateView(ctx, base);
    if (shaking)
      std::memcpy(base + node + kNodePosition, kept, sizeof(kept));
    if (player_view && !wanted.active)
      LearnCameraFrame(node);
    return;
  }
  for (int i = 0; i < 3; ++i)
    wanted.at[i] += shake[size_t(i)];
  uint8_t position[16], rotation[16];
  std::memcpy(position, base + node + kNodePosition, sizeof(position));
  std::memcpy(rotation, base + node + kNodeRotation, sizeof(rotation));
  const uint32_t flags = Load32(node + kNodeFlags);
  const std::array<float, 4> q = CameraQuaternion(g_camera_frame, {wanted.at[0], wanted.at[1], wanted.at[2]},
                                                  {wanted.look[0], wanted.look[1], wanted.look[2]});
  for (int i = 0; i < 3; ++i)
    StoreFloat(node + kNodePosition + 4 * i, wanted.at[i]);
  for (int i = 0; i < 4; ++i)
    StoreFloat(node + kNodeRotation + 4 * i, q[size_t(i)]);
  Store32(node + kNodeFlags, flags & ~1u);
  __imp__rex_meCalculateView(ctx, base);
  std::memcpy(base + node + kNodePosition, position, sizeof(position));
  std::memcpy(base + node + kNodeRotation, rotation, sizeof(rotation));
  Store32(node + kNodeFlags, flags);
}

// (HUD list, flag, show contents): every crate's award, collected, bought at LOG's or given by a script. Fired before
// it's given, so a listener's renut.crates.swap still changes what this one gives.
REX_HOOK_RAW(sub_82554410) {
  using namespace part_api;
  const uint32_t flag = ctx.r4.u32;
  if (g_events && flag && flag != kNoCrate) {
    const std::vector<std::string> &names = renut::game::flags::Names(renut::game::flags::Table::kNormal);
    if (flag < names.size())
      Fire(&ctx, base, g_events->crate_award, names[flag]);
  }
  __imp__sub_82554410(ctx, base);
}

// (HUD list, record, show, ..., add): what an award adds and shows. A swapped crate's record is stood in for by a word
// holding the other crate's blockset, which sub_824FA620 below then leaves alone.
REX_HOOK_RAW(sub_82554288) {
  using namespace part_api;
  if (const uint32_t blockset = SwappedBlockset(ctx.r4.u32)) {
    Store32(SwapSlot(0), blockset);
    ctx.r4.u64 = SwapSlot(0);
  }
  __imp__sub_82554288(ctx, base);
}

// (&blockset aid, ...): adds a blockset's parts. At each level load sub_824FB5D8 adds every collected crate's record
// here, so a swap is made again there and lasts.
REX_HOOK_RAW(sub_824FA620) {
  using namespace part_api;
  if (ctx.r3.u32 != SwapSlot(0)) {
    if (const uint32_t blockset = SwappedBlockset(ctx.r3.u32)) {
      Store32(SwapSlot(1), blockset);
      ctx.r3.u64 = SwapSlot(1);
    }
  }
  __imp__sub_824FA620(ctx, base);
}

// (class, objparams copy, net): makes an object. The first crate a level places gives reNut the crates'
// objparams aid, which the copy keeps (renut.crates.spawn).
REX_HOOK_RAW(sub_8236A570) {
  using namespace part_api;
  const uint32_t copy = ctx.r4.u32;
  std::string kind;
  uint32_t aid = 0;
  if (Heap(copy)) {
    aid = Load32(copy + kCopyAid);
    if (!g_crate_objparams.load(std::memory_order_relaxed) &&
        CString(copy + kCopyClassName, sizeof(kCrateClass)) == kCrateClass && aid)
      g_crate_objparams.store(aid, std::memory_order_release);
    kind = LearnKind(copy);
  }
  REXFABRIC_ORIGINAL(sub_8236A570)(ctx, base);
  const uint32_t object = ctx.r3.u32;
  if (Heap(object) && (!kind.empty() || aid)) {
    std::lock_guard lock(g_kinds_mutex);
    if (!kind.empty())
      g_made[object] = kind;
    if (aid)
      g_made_aid[object] = aid;
  }
  // renut.object_spawn: made, though it may join its world's list only on the world's next update.
  if (g_events && Heap(object) && g_events->object_spawn.HasListeners())
    Fire(&ctx, base, g_events->object_spawn, int64_t(object), kind, IsActor(object));
}

// (vehicle, part): it comes off.
REX_HOOK_RAW(sub_825EDDA0) {
  if (part_api::g_events)
    part_api::Fire(&ctx, base, part_api::g_events->vehicle_part_removed, int64_t(ctx.r3.u32), int64_t(ctx.r4.u32));
  __imp__sub_825EDDA0(ctx, base);
}

// (part, double amount): a part's health goes down; whether it should break off.
REX_HOOK_RAW(sub_821D3648) {
  double amount = ctx.f1.f64;
  if (part_api::g_events)
    part_api::FireDamage(ctx, base, part_api::g_events->part_damage, amount, int64_t(ctx.r3.u32));
  ctx.f1.f64 = amount;
  __imp__sub_821D3648(ctx, base);
}

// (part, sever, ...): it breaks off its vehicle.
REX_HOOK_RAW(sub_82616198) {
  if (part_api::g_events)
    part_api::Fire(&ctx, base, part_api::g_events->part_break, int64_t(ctx.r3.u32));
  __imp__sub_82616198(ctx, base);
}
