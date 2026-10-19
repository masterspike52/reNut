/**
 * @file    game/part_classes.cpp
 * @brief   New vehicle part classes, whose behaviour runs in reNut.
 *
 * N&B's classes (IDA): ids 0-35 are the engine's (off_82E5EDC0), 36 and up
 * the game's, through the record dword_82E5EDD0 points at ({table, count,
 * first id, end id}; table entries {name, parent id, descriptor getter}).
 * sub_82277A68(id) calls the getter for the class's descriptor,
 * sub_82277BC8(id) is its parent, sub_8239D980(id) its name;
 * classIdFromName (0x82215F58) binary-searches the name map dword_82E5EDD4
 * (dword_82E5EDD8 12-byte {name, id} entries, byte order: nameCompare)
 * that banjoxScriptInit (0x82514530) builds. An objparams record names its
 * class at + 0x4B; sub_8239D9F0 creates an instance of it (instance + 0 the
 * descriptor, + 4 the class id; the class's sizes must match the record's).
 *
 * A vehicle part's descriptor is 25 guest function pointers, filled from its
 * parent's (vehicleBlock's: sub_8261E9D8):
 *   [0] sizes(out {instance size, objparams size})
 *   [1] init(instance, objparams): every class's in the chain, root first
 *       (sub_8239DC20, skipping one equal to its parent's)
 *   [2] destroy(instance): every class's likewise (sub_82277AD0)
 *   [3] message(instance, message): + 0 its id; handlers call their parent's
 *   [5] attach(part, vehicle, ..., flags r8): joins the vehicle's part list
 *       (vehicle + 5256, 176-byte entries {flags, part, ...}); flag 1 makes
 *       the vehicle's update (sub_82251568) call [7] each frame
 *   [7] tick(part, vehicle, double dt)
 *   [18] / [19] the part's action button down / up (the horn honks on 18, a
 *       weapon fires from 18 to 19)
 * the rest kept as the parent's.
 *
 * Here a new class is a copy of its parent's descriptor in guest memory with
 * 1, 2, 3, 5, 7, 18 and 19 pointing at thunks (FunctionDispatcher::
 * AllocateThunk) into this file: message, attach and tick call the parent's
 * first (attach with flag 1 set), then the part's behaviour gets the event;
 * the buttons are the behaviour's alone. Its getter is a thunk of its own (a
 * host function per class: a call into the code range doesn't record its
 * target), the table and the name map are copied with the class added, and
 * dword_82E5EDD0 points at the new record.
 * The part's script (part_scripts.h) gets the events.
 */
#include "renut_engine/game/part_classes.h"

#include <rex/hook.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <utility>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "renut_engine/app/renut_logging.h"
#include "renut_engine/game/part_scripts.h"
#include "renut_engine/mods/custom_parts.h"

namespace {

using renut::mods::CustomPart;
namespace part_scripts = renut::game::part_scripts;

// Guest data.
constexpr uint32_t kEngineClasses = 0x82E5EDC0;     // the engine's class record (ids 0-35)
constexpr uint32_t kGameClassesPtr = 0x82E5EDD0;    // -> the game's class record
constexpr uint32_t kClassNameMap = 0x82E5EDD4;      // -> {name, id, -} sorted by name
constexpr uint32_t kClassNameMapCount = 0x82E5EDD8;
constexpr uint32_t kClassEntrySize = 12;            // {name, parent id, getter}
constexpr uint32_t kNameMapEntrySize = 12;          // {name, id, -}
constexpr uint32_t kInstanceClass = 4;              // instance + 4: its class id
constexpr uint32_t kDescriptorSlots = 25;
constexpr uint32_t kSlotInit = 1, kSlotDestroy = 2, kSlotMessage = 3, kSlotAttach = 5, kSlotTick = 7,
                   kSlotButtonDown = 18, kSlotButtonUp = 19;
constexpr uint32_t kAttachTickFlag = 1;

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

rex::runtime::FunctionDispatcher *Dispatcher() {
  return REX_KERNEL_STATE()->function_dispatcher();
}

// Calls the guest function at 'va' with the context as it is.
void CallGuest(PPCContext &ctx, uint8_t *base, uint32_t va) {
  if (!va)
    return;
  if (::PPCFunc *fn = Dispatcher()->GetFunction(va))
    fn(ctx, base);
  else
    RNUT_ERROR("Part classes: no function at guest {:08X}", va);
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

// --- The behaviour's events: the part's script (part_scripts.h) -------------

void OnInit(PPCContext &ctx, uint8_t *base, const ScriptedClass &cls, uint32_t instance) {
  if (cls.part)
    part_scripts::Spawn(ctx, base, *cls.part, instance);
}

void OnDestroy(PPCContext &ctx, uint8_t *base, const ScriptedClass &cls, uint32_t instance) {
  if (cls.part)
    part_scripts::Destroy(ctx, base, *cls.part, instance);
}

void OnAttach(PPCContext &ctx, uint8_t *base, const ScriptedClass &cls, uint32_t part, uint32_t vehicle) {
  if (cls.part)
    part_scripts::Attach(ctx, base, *cls.part, part, vehicle);
}

void OnTick(PPCContext &ctx, uint8_t *base, const ScriptedClass &cls, uint32_t part, uint32_t vehicle,
            double dt) {
  if (cls.part)
    part_scripts::Tick(ctx, base, *cls.part, part, vehicle, dt);
}

void OnButton(PPCContext &ctx, uint8_t *base, const ScriptedClass &cls, uint32_t part, uint32_t vehicle,
              bool down) {
  if (cls.part)
    part_scripts::Button(ctx, base, *cls.part, part, vehicle, down);
}

void OnMessage(PPCContext &, uint8_t *, const ScriptedClass &, uint32_t, uint32_t) {}

// --- Thunks -----------------------------------------------------------------

// Class getters: one host function per class (a thunk can't tell which guest
// address called it: a call into the code range doesn't record its target),
// from a pool of kMaxClasses, each returning its slot's descriptor.
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

// [5] attach(part, vehicle, ..., flags): the parent's, ticked each frame.
void SlotAttach(PPCContext &ctx, uint8_t *base) {
  const uint32_t part = ctx.r3.u32, vehicle = ctx.r4.u32;
  const ScriptedClass *cls = ClassOfInstance(base, part);
  if (!cls) {
    ctx.r3.u64 = 0;
    return;
  }
  ctx.r8.u64 |= kAttachTickFlag;
  CallGuest(ctx, base, cls->parent_slots[kSlotAttach]);
  if (ctx.r3.u32)
    OnAttach(ctx, base, *cls, part, vehicle);
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

// [18] / [19]: the button belongs to the part's behaviour alone (the parent's
// action, a horn's honk, doesn't happen).
void Button(PPCContext &ctx, uint8_t *base, bool down) {
  const uint32_t part = ctx.r3.u32, vehicle = ctx.r4.u32;
  if (const ScriptedClass *cls = ClassOfInstance(base, part))
    OnButton(ctx, base, *cls, part, vehicle, down);
}

void SlotButtonDown(PPCContext &ctx, uint8_t *base) { Button(ctx, base, true); }
void SlotButtonUp(PPCContext &ctx, uint8_t *base) { Button(ctx, base, false); }

bool AllocateSlotThunks() {
  if (g_thunks[kSlotInit])
    return true;
  const std::pair<uint32_t, ::PPCFunc *> slots[] = {
      {kSlotInit, SlotInit},       {kSlotDestroy, SlotDestroy},       {kSlotMessage, SlotMessage},
      {kSlotAttach, SlotAttach},   {kSlotTick, SlotTick},             {kSlotButtonDown, SlotButtonDown},
      {kSlotButtonUp, SlotButtonUp},
  };
  for (const auto &[slot, fn] : slots) {
    g_thunks[slot] = Dispatcher()->AllocateThunk(fn, 0);
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

  auto *memory = REX_KERNEL_MEMORY();
  const uint32_t new_table = memory->SystemHeapAlloc((count + 1) * kClassEntrySize);
  const uint32_t new_record = memory->SystemHeapAlloc(16);
  const uint32_t new_map = memory->SystemHeapAlloc((map_count + 1) * kNameMapEntrySize);
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

uint32_t RegisterScriptedPartClass(PPCContext &ctx, uint8_t *base, const std::string &name,
                                   const std::string &parent, const CustomPart *part) {
  std::lock_guard lock(g_mutex);
  for (const auto &cls : g_classes)
    if (cls->name == name)
      return cls->id;
  if (ClassIdByName(base, name)) {
    RNUT_ERROR("Part classes: the game already has a class named {}", name);
    return 0;
  }
  const uint32_t parent_id = ClassIdByName(base, parent);
  const uint32_t parent_entry = parent_id ? ClassEntry(base, parent_id) : 0;
  if (!parent_entry) {
    RNUT_ERROR("Part classes: {}: no parent class {}", name, parent);
    return 0;
  }
  if (!AllocateSlotThunks()) {
    RNUT_ERROR("Part classes: no guest thunks for the callbacks");
    return 0;
  }

  // The parent's descriptor, through its getter.
  const uint64_t r3 = ctx.r3.u64;
  ctx.r3.u64 = 0;
  CallGuest(ctx, base, Load32(base, parent_entry + 8));
  const uint32_t parent_descriptor = ctx.r3.u32;
  ctx.r3.u64 = r3;
  if (!parent_descriptor) {
    RNUT_ERROR("Part classes: {}: the parent {} has no descriptor", name, parent);
    return 0;
  }

  ScriptedClass cls;
  cls.name = name;
  cls.parent_id = parent_id;
  cls.part = part;
  const size_t index = g_classes.size();
  if (index >= kMaxClasses) {
    RNUT_ERROR("Part classes: {}: at most {} scripted part classes", name, kMaxClasses);
    return 0;
  }
  auto *memory = REX_KERNEL_MEMORY();
  cls.descriptor = memory->SystemHeapAlloc(kDescriptorSlots * 4);
  const uint32_t name_va = memory->SystemHeapAlloc(uint32_t(name.size() + 1));
  cls.getter = Dispatcher()->AllocateThunk(kGetters[index], 0);
  if (!cls.descriptor || !name_va || !cls.getter) {
    RNUT_ERROR("Part classes: {}: no guest memory or thunk", name);
    return 0;
  }
  std::memcpy(base + name_va, name.c_str(), name.size() + 1);
  for (uint32_t slot = 0; slot < kDescriptorSlots; ++slot) {
    cls.parent_slots[slot] = Load32(base, parent_descriptor + slot * 4);
    Store32(base, cls.descriptor + slot * 4, g_thunks[slot] ? g_thunks[slot] : cls.parent_slots[slot]);
  }

  const uint32_t record = Load32(base, kGameClassesPtr);
  cls.id = Load32(base, record + 12);
  if (!AddToRegistry(base, cls.id, name_va, parent_id, cls.getter)) {
    RNUT_ERROR("Part classes: {}: the class registry couldn't be extended", name);
    return 0;
  }
  g_getter_descriptors[index].store(cls.descriptor, std::memory_order_release);
  g_by_id[cls.id] = index;
  RNUT_INFO("Part classes: {} is class {} (parent {} = {})", name, cls.id, parent, parent_id);
  g_classes.push_back(std::make_unique<ScriptedClass>(std::move(cls)));
  return g_classes.back()->id;
}

} // namespace renut::game
