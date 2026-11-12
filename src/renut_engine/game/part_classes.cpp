#include "renut_engine/game/part_classes.h"

#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/guest_memory.h>
#include <rex/kernel/module.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/hook.h>
#include <rex/types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "renut_engine/game/part_api.h"
#include "renut_engine/game/part_scripts.h"
#include "renut_engine/mods/custom_parts.h"

namespace {

using renut::mods::CustomPart;
namespace part_api = renut::game::part_api;
namespace part_scripts = renut::game::part_scripts;

// Guest data.
constexpr uint32_t kEngineClasses = 0x82D99434;     // the engine's class record (ids 0-35)
constexpr uint32_t kGameClassesPtr = 0x82D99444;    // -> the game's class record
constexpr uint32_t kClassNameMap = 0x82D99448;      // -> {name, id, -} sorted by name
constexpr uint32_t kClassNameMapCount = 0x82D9944C;
constexpr uint32_t kClassEntrySize = 12;            // {name, parent id, getter}
constexpr uint32_t kNameMapEntrySize = 12;          // {name, id, -}
constexpr uint32_t kInstanceClass = 4;              // instance + 4: its class id
constexpr uint32_t kDescriptorSlots = 25;
constexpr uint32_t kSlotInit = 1, kSlotDestroy = 2, kSlotMessage = 3, kSlotAttach = 5, kSlotDetach = 6,
                   kSlotTick = 7, kSlotPhysics = 8, kSlotPowerOn = 13, kSlotPowerOff = 14,
                   kSlotButtonDown = 18, kSlotButtonUp = 19;
constexpr uint32_t kAttachTickFlag = 1;     // the vehicle runs its slot 7 each frame
constexpr uint32_t kAttachPhysicsFlag = 2;  // ... and slot 8 each physics step
constexpr uint32_t kEntrySeatFlag = 8;      // its buttons come from a gunner seat, not the driver
constexpr uint32_t kPartHasButtons = 952, kPartSeat = 832;
constexpr uint32_t kVehicleParts = 0x1498, kVehiclePartEntrySize = 176;  // {flags, part, ...}

uint32_t Load32(const uint8_t *base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return rex::byte_swap(v);
}

void Store32(uint8_t *base, uint32_t va, uint32_t value) {
  value = rex::byte_swap(value);
  std::memcpy(base + va, &value, 4);
}

std::string GuestString(const uint8_t *base, uint32_t va) {
  std::string s;
  while (va && base[va + s.size()] && s.size() < 256)
    s.push_back(char(base[va + s.size()]));
  return s;
}

// System heap memory, zeroed; 0 when there's none.
uint32_t SystemAlloc(uint32_t size) {
  rex::kernel::GuestMemory *memory = REX_GUEST_MEMORY();
  if (!memory)
    return 0;
  return memory->Allocate(size, 0x20, rex::kernel::GuestProtect::kReadWrite, rex::kernel::GuestHeap::kSystem)
      .value_or(0);
}

// Calls the guest function at 'va' with the context as it is.
void CallGuest(PPCContext &ctx, uint8_t *base, uint32_t va) {
  if (!va)
    return;
  if (::PPCFunc *fn = renut::game::GuestFunction(va))
    fn(ctx, base);
}

struct ScriptedClass {
  std::string name;
  uint32_t id = 0;
  uint32_t parent_id = 0;
  uint32_t getter = 0;      // its getter thunk
  uint32_t descriptor = 0;  // guest
  uint32_t parent_slots[kDescriptorSlots] = {};
  const CustomPart *part = nullptr;
};

// Game thread (registration and the callbacks); recursive: a parent's getter
// runs while registering.
std::recursive_mutex g_mutex;
std::vector<std::unique_ptr<ScriptedClass>> g_classes;  // (stable addresses)
uint32_t g_thunks[kDescriptorSlots] = {};  // the slot thunks
std::unordered_map<uint32_t, size_t> g_by_id;

const ScriptedClass *ClassOfInstance(const uint8_t *base, uint32_t instance) {
  if (!instance)
    return nullptr;
  std::lock_guard lock(g_mutex);
  auto it = g_by_id.find(Load32(base, instance + kInstanceClass));
  return it == g_by_id.end() ? nullptr : g_classes[it->second].get();
}

// The table entry of class 'id' (the engine's or the game's record), or 0.
uint32_t ClassEntry(const uint8_t *base, uint32_t id) {
  for (uint32_t record : {kEngineClasses, Load32(base, kGameClassesPtr)}) {
    if (!record)
      continue;
    const uint32_t first = Load32(base, record + 8), end = Load32(base, record + 12);
    if (id >= first && id < end)
      return Load32(base, record) + (id - first) * kClassEntrySize;
  }
  return 0;
}

uint32_t ClassIdByName(const uint8_t *base, const std::string &name) {
  const uint32_t map = Load32(base, kClassNameMap);
  const uint32_t count = Load32(base, kClassNameMapCount);
  for (uint32_t i = 0; map && i < count; ++i)
    if (GuestString(base, Load32(base, map + i * kNameMapEntrySize)) == name)
      return Load32(base, map + i * kNameMapEntrySize + 4);
  return 0;
}

// --- The behaviour's events: the part's script (part_scripts.h), then the
// RexFabric mods' (part_api.h) ------------------------------------------------

bool HasScript(const ScriptedClass &cls) {
  return cls.part && !cls.part->script.empty();
}

void OnInit(PPCContext &ctx, uint8_t *base, const ScriptedClass &cls, uint32_t instance) {
  if (HasScript(cls))
    part_scripts::Spawn(ctx, base, *cls.part, instance);
  if (cls.part)
    part_api::Spawn(ctx, base, *cls.part, instance);
}

void OnDestroy(PPCContext &ctx, uint8_t *base, const ScriptedClass &cls, uint32_t instance) {
  if (HasScript(cls))
    part_scripts::Destroy(ctx, base, *cls.part, instance);
  if (cls.part)
    part_api::Destroy(ctx, base, *cls.part, instance);
}

void OnAttach(PPCContext &ctx, uint8_t *base, const ScriptedClass &cls, uint32_t part, uint32_t vehicle) {
  if (HasScript(cls))
    part_scripts::Attach(ctx, base, *cls.part, part, vehicle);
  if (cls.part)
    part_api::Attach(ctx, base, *cls.part, part, vehicle);
}

void OnTick(PPCContext &ctx, uint8_t *base, const ScriptedClass &cls, uint32_t part, uint32_t vehicle,
            double dt) {
  if (HasScript(cls))
    part_scripts::Tick(ctx, base, *cls.part, part, vehicle, dt);
  if (cls.part)
    part_api::Tick(ctx, base, *cls.part, part, vehicle, dt);
}

void OnButton(PPCContext &ctx, uint8_t *base, const ScriptedClass &cls, uint32_t part, uint32_t vehicle,
              bool down) {
  if (HasScript(cls))
    part_scripts::Button(ctx, base, *cls.part, part, vehicle, down);
  if (cls.part)
    part_api::Button(ctx, base, *cls.part, part, vehicle, down);
}

void OnMessage(PPCContext &, uint8_t *, const ScriptedClass &, uint32_t, uint32_t) {}

// --- Thunks -----------------------------------------------------------------

// Class getters: a pool of kMaxClasses host functions, one per class (a thunk
// can't tell which guest address called it), each returning its descriptor.
constexpr size_t kMaxClasses = 64;
std::atomic<uint32_t> g_getter_descriptors[kMaxClasses] = {};

template <size_t I>
void ClassGetter(PPCContext &ctx, uint8_t *) {
  ctx.r3.u64 = g_getter_descriptors[I].load(std::memory_order_acquire);
}

template <size_t... I>
constexpr std::array<::PPCFunc *, sizeof...(I)> MakeGetters(std::index_sequence<I...>) {
  return {ClassGetter<I>...};
}
constexpr std::array<::PPCFunc *, kMaxClasses> kGetters = MakeGetters(std::make_index_sequence<kMaxClasses>());

// [1] init(instance, objparams): chained by the game; true.
void SlotInit(PPCContext &ctx, uint8_t *base) {
  if (const ScriptedClass *cls = ClassOfInstance(base, ctx.r3.u32))
    OnInit(ctx, base, *cls, ctx.r3.u32);
  ctx.r3.u64 = 1;
}

// [2] destroy(instance): chained by the game.
void SlotDestroy(PPCContext &ctx, uint8_t *base) {
  if (const ScriptedClass *cls = ClassOfInstance(base, ctx.r3.u32))
    OnDestroy(ctx, base, *cls, ctx.r3.u32);
}

// [3] message(instance, message).
void SlotMessage(PPCContext &ctx, uint8_t *base) {
  const uint32_t instance = ctx.r3.u32, message = ctx.r4.u32;
  const ScriptedClass *cls = ClassOfInstance(base, instance);
  if (!cls)
    return;
  CallGuest(ctx, base, cls->parent_slots[kSlotMessage]);
  OnMessage(ctx, base, *cls, instance, message);
}

// A part with both its own buttons and a gunner seat (an egg turret given a button)
// is read from the seat only; this has the driver's pad press it instead.
void UseDriverButtons(uint8_t *base, uint32_t part, uint32_t vehicle) {
  if (!Load32(base, part + kPartHasButtons) || !Load32(base, part + kPartSeat))
    return;
  const uint32_t end = Load32(base, vehicle + kVehicleParts + 4);
  for (uint32_t e = Load32(base, vehicle + kVehicleParts); e && e < end; e += kVehiclePartEntrySize) {
    if (Load32(base, e + 4) == part) {
      Store32(base, e, Load32(base, e) & ~kEntrySeatFlag);
      return;
    }
  }
}

// [5] attach(part, vehicle, ..., flags): the parent's, ticked each frame and
// physics step.
void SlotAttach(PPCContext &ctx, uint8_t *base) {
  const uint32_t part = ctx.r3.u32, vehicle = ctx.r4.u32;
  const ScriptedClass *cls = ClassOfInstance(base, part);
  if (!cls) {
    ctx.r3.u64 = 0;
    return;
  }
  ctx.r8.u64 |= kAttachTickFlag | kAttachPhysicsFlag;
  CallGuest(ctx, base, cls->parent_slots[kSlotAttach]);
  if (ctx.r3.u32) {
    UseDriverButtons(base, part, vehicle);
    OnAttach(ctx, base, *cls, part, vehicle);
  }
}

// [7] tick(part, vehicle, double dt).
void SlotTick(PPCContext &ctx, uint8_t *base) {
  const uint32_t part = ctx.r3.u32, vehicle = ctx.r4.u32;
  const double dt = ctx.f1.f64;
  const ScriptedClass *cls = ClassOfInstance(base, part);
  if (!cls)
    return;
  CallGuest(ctx, base, cls->parent_slots[kSlotTick]);
  OnTick(ctx, base, *cls, part, vehicle, dt);
}

// [6] detach(part, vehicle), [8] physics step(part, vehicle, double dt), [13] / [14]
// power on / off(part, vehicle) as the driver gets in or out: the parent's, then the event.
void SlotDetach(PPCContext &ctx, uint8_t *base) {
  const uint32_t part = ctx.r3.u32, vehicle = ctx.r4.u32;
  const ScriptedClass *cls = ClassOfInstance(base, part);
  if (!cls)
    return;
  CallGuest(ctx, base, cls->parent_slots[kSlotDetach]);
  if (cls->part)
    part_api::Detach(ctx, base, *cls->part, part, vehicle);
}

void SlotPhysics(PPCContext &ctx, uint8_t *base) {
  const uint32_t part = ctx.r3.u32, vehicle = ctx.r4.u32;
  const double dt = ctx.f1.f64;
  const ScriptedClass *cls = ClassOfInstance(base, part);
  if (!cls)
    return;
  CallGuest(ctx, base, cls->parent_slots[kSlotPhysics]);
  if (cls->part)
    part_api::Physics(ctx, base, *cls->part, part, vehicle, dt);
}

void Power(PPCContext &ctx, uint8_t *base, bool on) {
  const uint32_t part = ctx.r3.u32, vehicle = ctx.r4.u32;
  const ScriptedClass *cls = ClassOfInstance(base, part);
  if (!cls)
    return;
  CallGuest(ctx, base, cls->parent_slots[on ? kSlotPowerOn : kSlotPowerOff]);
  if (cls->part)
    part_api::Power(ctx, base, *cls->part, part, vehicle, on);
}

void SlotPowerOn(PPCContext &ctx, uint8_t *base) { Power(ctx, base, true); }
void SlotPowerOff(PPCContext &ctx, uint8_t *base) { Power(ctx, base, false); }

// [18] / [19]: the button belongs to the part's behaviour (the parent's action,
// a horn's honk, happens only for a part that keeps it: base_action).
void Button(PPCContext &ctx, uint8_t *base, bool down) {
  const uint32_t part = ctx.r3.u32, vehicle = ctx.r4.u32;
  const ScriptedClass *cls = ClassOfInstance(base, part);
  if (!cls)
    return;
  if (cls->part && cls->part->base_action)
    CallGuest(ctx, base, cls->parent_slots[down ? kSlotButtonDown : kSlotButtonUp]);
  OnButton(ctx, base, *cls, part, vehicle, down);
}

void SlotButtonDown(PPCContext &ctx, uint8_t *base) { Button(ctx, base, true); }
void SlotButtonUp(PPCContext &ctx, uint8_t *base) { Button(ctx, base, false); }

bool AllocateSlotThunks() {
  if (g_thunks[kSlotInit])
    return true;
  const std::pair<uint32_t, ::PPCFunc *> slots[] = {
      {kSlotInit, SlotInit},       {kSlotDestroy, SlotDestroy},       {kSlotMessage, SlotMessage},
      {kSlotAttach, SlotAttach},   {kSlotDetach, SlotDetach},         {kSlotTick, SlotTick},
      {kSlotPhysics, SlotPhysics}, {kSlotPowerOn, SlotPowerOn},       {kSlotPowerOff, SlotPowerOff},
      {kSlotButtonDown, SlotButtonDown}, {kSlotButtonUp, SlotButtonUp},
  };
  for (const auto &[slot, fn] : slots) {
    g_thunks[slot] = renut::game::AllocateGuestThunk(fn);
    if (!g_thunks[slot])
      return false;
  }
  return true;
}

// Adds class 'id' (table entry {name, parent, getter}) to the game's record
// and the name map.
bool AddToRegistry(uint8_t *base, uint32_t id, uint32_t name_va, uint32_t parent_id, uint32_t getter) {
  const uint32_t record = Load32(base, kGameClassesPtr);
  const uint32_t table = Load32(base, record), count = Load32(base, record + 4);
  const uint32_t first = Load32(base, record + 8), end = Load32(base, record + 12);
  if (id != end)
    return false;
  const uint32_t map = Load32(base, kClassNameMap), map_count = Load32(base, kClassNameMapCount);

  const uint32_t new_table = SystemAlloc((count + 1) * kClassEntrySize);
  const uint32_t new_record = SystemAlloc(16);
  const uint32_t new_map = SystemAlloc((map_count + 1) * kNameMapEntrySize);
  if (!new_table || !new_record || !new_map)
    return false;
  std::memcpy(base + new_table, base + table, count * kClassEntrySize);
  const uint32_t entry = new_table + count * kClassEntrySize;
  Store32(base, entry, name_va);
  Store32(base, entry + 4, parent_id);
  Store32(base, entry + 8, getter);

  // The name map stays sorted (nameCompare: bytes).
  const std::string name = GuestString(base, name_va);
  uint32_t at = 0;
  while (at < map_count && GuestString(base, Load32(base, map + at * kNameMapEntrySize)) < name)
    ++at;
  std::memcpy(base + new_map, base + map, at * kNameMapEntrySize);
  Store32(base, new_map + at * kNameMapEntrySize, name_va);
  Store32(base, new_map + at * kNameMapEntrySize + 4, id);
  Store32(base, new_map + at * kNameMapEntrySize + 8, 0);
  std::memcpy(base + new_map + (at + 1) * kNameMapEntrySize, base + map + at * kNameMapEntrySize,
              (map_count - at) * kNameMapEntrySize);

  Store32(base, new_record, new_table);
  Store32(base, new_record + 4, count + 1);
  Store32(base, new_record + 8, first);
  Store32(base, new_record + 12, end + 1);
  // The map first (a lookup finding the new id before the record has it
  // would miss), then the record.
  Store32(base, kClassNameMap, new_map);
  Store32(base, kClassNameMapCount, map_count + 1);
  Store32(base, kGameClassesPtr, new_record);
  return true;
}

} // namespace

namespace renut::game {

::PPCFunc *GuestFunction(uint32_t va) {
  rex::kernel::TitleRuntime *runtime = REX_RUNTIME();
  if (!runtime)
    return nullptr;
  const auto found = runtime->FindFunction(va);
  return found ? *found : nullptr;
}

uint32_t AllocateGuestThunk(::PPCFunc *fn) {
  // The executable's module holds the function dispatcher, as the runtime doesn't expose it.
  rex::kernel::TitleRuntime *runtime = REX_RUNTIME();
  const std::optional<rex::kernel::X_HANDLE> executable =
      runtime ? runtime->GetModuleRegistry().GetExecutable() : std::nullopt;
  if (!executable)
    return 0;
  const auto module = runtime->GetObjectTable().Get<rex::kernel::ModuleObject>(*executable);
  if (!module || !module->processor_module)
    return 0;
  rex::kernel::FunctionDispatcher *dispatcher = module->processor_module->GetFunctionDispatcher();
  return dispatcher ? dispatcher->AllocateThunk(fn, 0) : 0;
}

std::string InstanceClassName(const uint8_t *base, uint32_t instance) {
  const uint32_t entry = ClassEntry(base, Load32(base, instance + kInstanceClass));
  return entry ? GuestString(base, Load32(base, entry)) : std::string();
}

bool IsScriptedPart(const uint8_t *base, uint32_t instance) {
  const ScriptedClass *cls = ClassOfInstance(base, instance);
  return cls && cls->part;
}

bool InstanceIsA(const uint8_t *base, uint32_t instance, const std::string &class_name) {
  // Up the parents (a chain far shorter than this).
  uint32_t id = Load32(base, instance + kInstanceClass);
  for (int depth = 0; id && depth < 32; ++depth) {
    const uint32_t entry = ClassEntry(base, id);
    if (!entry)
      return false;
    if (GuestString(base, Load32(base, entry)) == class_name)
      return true;
    const uint32_t parent = Load32(base, entry + 4);
    if (parent == id)
      return false;
    id = parent;
  }
  return false;
}

uint32_t RegisterScriptedPartClass(PPCContext &ctx, uint8_t *base, const std::string &name,
                                   const std::string &parent, const CustomPart *part) {
  std::lock_guard lock(g_mutex);
  for (const auto &cls : g_classes)
    if (cls->name == name)
      return cls->id;
  // The game has a class of that name already, there's no parent class of
  // that name, or no guest thunks for the callbacks: none.
  if (ClassIdByName(base, name))
    return 0;
  const uint32_t parent_id = ClassIdByName(base, parent);
  const uint32_t parent_entry = parent_id ? ClassEntry(base, parent_id) : 0;
  if (!parent_entry || !AllocateSlotThunks())
    return 0;

  // The parent's descriptor, through its getter.
  const uint64_t r3 = ctx.r3.u64;
  ctx.r3.u64 = 0;
  CallGuest(ctx, base, Load32(base, parent_entry + 8));
  const uint32_t parent_descriptor = ctx.r3.u32;
  ctx.r3.u64 = r3;
  if (!parent_descriptor)
    return 0;

  ScriptedClass cls;
  cls.name = name;
  cls.parent_id = parent_id;
  cls.part = part;
  const size_t index = g_classes.size();
  if (index >= kMaxClasses)
    return 0;
  cls.descriptor = SystemAlloc(kDescriptorSlots * 4);
  const uint32_t name_va = SystemAlloc(uint32_t(name.size() + 1));
  cls.getter = AllocateGuestThunk(kGetters[index]);
  if (!cls.descriptor || !name_va || !cls.getter)
    return 0;
  std::memcpy(base + name_va, name.c_str(), name.size() + 1);
  for (uint32_t slot = 0; slot < kDescriptorSlots; ++slot) {
    cls.parent_slots[slot] = Load32(base, parent_descriptor + slot * 4);
    Store32(base, cls.descriptor + slot * 4, g_thunks[slot] ? g_thunks[slot] : cls.parent_slots[slot]);
  }

  const uint32_t record = Load32(base, kGameClassesPtr);
  cls.id = Load32(base, record + 12);
  if (!AddToRegistry(base, cls.id, name_va, parent_id, cls.getter))
    return 0;
  g_getter_descriptors[index].store(cls.descriptor, std::memory_order_release);
  g_by_id[cls.id] = index;
  g_classes.push_back(std::make_unique<ScriptedClass>(std::move(cls)));
  return g_classes.back()->id;
}

} // namespace renut::game
