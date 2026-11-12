// The RexFabricApi table handed to native mods.

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/kernel/title_runtime.h>
#include <rex/ppc/call.h>
#include <rex/settings_registry.h>
#include <rexfabric/game_api.h>
#include <rexfabric/midasm.h>
#include <rexfabric/rexfabric.h>
#include <rexfabric/switches.h>

#include "events.h"
#include "guest.h"
#include "hooks.h"
#include "log.h"
#include "mod.h"
#include "settings.h"

// What a RexFabricMidasm points at while a native handler runs.
struct RexFabricMidasm {
  uint32_t address;
  const rexfabric::MidasmReg* regs;
  size_t count;
};

namespace rexfabric {
namespace {

static_assert(sizeof(RexFabricRegister) == sizeof(PPCRegister));
static_assert(static_cast<int>(FieldKind::kChars) == REXFABRIC_FIELD_CHARS);
static_assert(static_cast<int>(ValueType::kObject) == REXFABRIC_VALUE_OBJECT);

RexFabricMod host_mod{"host"};

const char* Name(const RexFabricMod* mod) { return mod ? mod->name.c_str() : "?"; }

void Log(RexFabricMod* mod, RexFabricLogLevel level, const char* message) {
  switch (level) {
    case REXFABRIC_LOG_DEBUG: RF_DEBUG("[{}] {}", Name(mod), message); break;
    case REXFABRIC_LOG_INFO: RF_INFO("[{}] {}", Name(mod), message); break;
    case REXFABRIC_LOG_WARN: RF_WARN("[{}] {}", Name(mod), message); break;
    case REXFABRIC_LOG_ERROR: RF_ERROR("[{}] {}", Name(mod), message); break;
  }
}

// Hooks and calls ----------------------------------------------------------------------------------------

int IsHookable(uint32_t address) { return Hooks::Get().IsHookable(address); }

RexFabricHookId Hook(RexFabricMod* mod, uint32_t address, RexFabricHookFn fn, void* user) {
  const RexFabricHookId id = Hooks::Get().Add(mod, address, fn, user);
  if (!id) RF_WARN("[{}] can't hook {:08X}: not a recompiled function", Name(mod), address);
  return id;
}

int Unhook(RexFabricHookId id) { return Hooks::Get().Remove(id); }

void ApiCallNext(RexFabricCall* call) { CallNext(*call); }
uint32_t CallAddress(const RexFabricCall* call) { return call->address; }
uint64_t GetGpr(const RexFabricCall* call, uint32_t index) { return index < 32 ? (call->ctx->*guest::kGprs[index]).u64 : 0; }
void SetGpr(RexFabricCall* call, uint32_t index, uint64_t value) { if (index < 32) (call->ctx->*guest::kGprs[index]).u64 = value; }
double GetFpr(const RexFabricCall* call, uint32_t index) { return index < 32 ? (call->ctx->*guest::kFprs[index]).f64 : 0.0; }
void SetFpr(RexFabricCall* call, uint32_t index, double value) { if (index < 32) (call->ctx->*guest::kFprs[index]).f64 = value; }

uint64_t CallGuest(RexFabricCall* call, uint32_t address, const uint64_t* args, uint32_t count) {
  rex::kernel::TitleRuntime* runtime = REX_RUNTIME();
  rex::Result<PPCFunc*> function = runtime ? runtime->FindFunction(address) : rex::Result<PPCFunc*>(nullptr);
  if (!function || !*function) {
    RF_WARN("call_guest: no function at {:08X}", address);
    return 0;
  }
  // On the hooked call's own registers, as the thread's hooks expect, put back after so the hooked call's
  // registers survive. The frame goes under the hooked call's stack pointer.
  PPCContext& ctx = *call->ctx;
  const PPCContext saved = ctx;
  rex::ppc::OpenCallFrame(ctx, call->base, 0);
  for (uint32_t i = 0; i < count && i < 8; ++i) (ctx.*guest::kGprs[3 + i]).u64 = args[i];
  (**function)(ctx, call->base);
  const uint64_t result = ctx.r3.u64;
  ctx = saved;
  return result;
}

void* GuestPointer(uint32_t address) { return guest::Translate(address); }

template <typename T>
T Read(uint32_t address) { return guest::Read<T>(address); }

template <typename T>
void Write(uint32_t address, T value) { guest::Write<T>(address, value); }

// The game's API -----------------------------------------------------------------------------------------

const TypeDef* AsType(const RexFabricType* type) { return reinterpret_cast<const TypeDef*>(type); }
const RexFabricType* AsHandle(const TypeDef* type) { return reinterpret_cast<const RexFabricType*>(type); }

// Strings a result points at, until this thread's next call.
thread_local std::string result_text;

RexFabricValue ToNative(const Value& value) {
  RexFabricValue out{static_cast<uint32_t>(TypeOf(value))};
  switch (TypeOf(value)) {
    case ValueType::kNone: break;
    case ValueType::kBool: out.i = std::get<bool>(value); break;
    case ValueType::kInt: out.i = std::get<int64_t>(value); break;
    case ValueType::kFloat: out.f = std::get<double>(value); break;
    case ValueType::kString: out.s = std::get<std::string>(value).c_str(); break;
    case ValueType::kObject:
      out.address = std::get<Object>(value).address;
      out.object_type = AsHandle(std::get<Object>(value).type);
      break;
  }
  return out;
}

// Loosely: an integer passes for a float, and an address for an object.
bool FromNative(const RexFabricValue& in, ValueType want, Value& out) {
  switch (want) {
    case ValueType::kNone: out = {}; return true;
    case ValueType::kBool: out = in.i != 0; return in.type == REXFABRIC_VALUE_BOOL || in.type == REXFABRIC_VALUE_INT;
    case ValueType::kInt: out = in.i; return in.type == REXFABRIC_VALUE_INT || in.type == REXFABRIC_VALUE_BOOL;
    case ValueType::kFloat: out = in.type == REXFABRIC_VALUE_INT ? static_cast<double>(in.i) : in.f; return in.type == REXFABRIC_VALUE_FLOAT || in.type == REXFABRIC_VALUE_INT;
    case ValueType::kString: out = std::string(in.s ? in.s : ""); return in.type == REXFABRIC_VALUE_STRING && in.s;
    case ValueType::kObject:
      out = Object{in.type == REXFABRIC_VALUE_INT ? static_cast<uint32_t>(in.i) : in.address, AsType(in.object_type)};
      return in.type == REXFABRIC_VALUE_OBJECT || in.type == REXFABRIC_VALUE_INT || in.type == REXFABRIC_VALUE_NONE;
  }
  return false;
}

const RexFabricFunction* FindFunctionC(const char* name) { return reinterpret_cast<const RexFabricFunction*>(name ? FindFunction(name) : nullptr); }

int CallFunction(const RexFabricFunction* handle, const RexFabricValue* args, uint32_t count, RexFabricValue* result) {
  const auto* function = reinterpret_cast<const FunctionDef*>(handle);
  if (!function || count != function->params.size()) return 0;
  std::vector<Value> values(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (!FromNative(args[i], function->params[i], values[i])) {
      RF_WARN("{}: argument {} should be {}", function->name, i + 1, ValueTypeName(function->params[i]));
      return 0;
    }
  }
  try {
    const Value value = function->invoke(values);
    if (result) {
      *result = ToNative(value);
      if (TypeOf(value) == ValueType::kString) result->s = (result_text = std::get<std::string>(value)).c_str();
    }
    return 1;
  } catch (const std::exception& error) {
    RF_ERROR("{}: {}", function->name, error.what());
    return 0;
  }
}

const RexFabricType* FindTypeC(const char* name) { return AsHandle(name ? FindType(name) : nullptr); }
uint32_t TypeSize(const RexFabricType* type) { return type ? AsType(type)->size() : 0; }

int TypeField(const RexFabricType* type, const char* name, RexFabricField* out) {
  const FieldDef* field = type && name ? AsType(type)->FindField(name) : nullptr;
  if (!field || !out) return 0;
  *out = {field->offset, static_cast<uint32_t>(field->type.kind), field->count, field->stride, field->type.length, AsHandle(field->type.target)};
  return 1;
}

const RexFabricFunction* TypeMethod(const RexFabricType* type, const char* name) { return reinterpret_cast<const RexFabricFunction*>(type && name ? AsType(type)->FindMethod(name) : nullptr); }
const void* FindInterfaceC(const char* name, uint32_t min_version) { return name ? FindInterface(name, min_version) : nullptr; }

// Events -------------------------------------------------------------------------------------------------

RexFabricListenerId Listen(RexFabricMod* mod, const char* event, RexFabricEventFn fn, void* user) {
  if (!event || !fn) return 0;
  return events::Listen(event, mod, [fn, user](std::span<const Value> args) {
    std::vector<RexFabricValue> values;
    values.reserve(args.size());
    for (const Value& arg : args) values.push_back(ToNative(arg));
    fn(values.data(), static_cast<uint32_t>(values.size()), user);
  });
}

int Unlisten(RexFabricListenerId id) { return events::Unlisten(id); }

// Midasm -------------------------------------------------------------------------------------------------

struct NativeClaim {
  RexFabricMidasmFn fn;
  void* user;
};

// Claims live until released, or for good when a mod never releases; they're a few bytes each.
std::mutex claims_mutex;
std::unordered_map<RexFabricClaimId, std::unique_ptr<NativeClaim>> claims;

bool RunNativeClaim(void* user, uint32_t address, const MidasmReg* regs, size_t count) {
  const auto* claim = static_cast<const NativeClaim*>(user);
  RexFabricMidasm site{address, regs, count};
  return claim->fn(&site, claim->user) != 0;
}

RexFabricClaimId ClaimMidasmC(RexFabricMod* mod, uint32_t address, RexFabricMidasmFn fn, void* user) {
  if (!fn) return 0;
  auto claim = std::make_unique<NativeClaim>(NativeClaim{fn, user});
  const RexFabricClaimId id = ClaimMidasm(mod, address, RunNativeClaim, claim.get());
  std::lock_guard lock(claims_mutex);
  claims[id] = std::move(claim);
  return id;
}

int ReleaseMidasmC(RexFabricClaimId id) { return ReleaseMidasm(id); }
uint32_t MidasmAddress(const RexFabricMidasm* site) { return site ? site->address : 0; }

RexFabricRegister* MidasmRegister(RexFabricMidasm* site, const char* name) {
  for (size_t i = 0; site && name && i < site->count; ++i) {
    if (std::strcmp(site->regs[i].name, name) == 0) return reinterpret_cast<RexFabricRegister*>(site->regs[i].reg);
  }
  return nullptr;
}

// Switches and cvars -------------------------------------------------------------------------------------

int32_t GetSwitchC(const char* name) { return name ? GetSwitch(name) : 0; }
void SetSwitchC(const char* name, int32_t value) { if (name) SetSwitch(name, value); }

int32_t GetCvar(const char* name, char* buffer, uint32_t size) {
  const rex::SettingBase* setting = name ? FindSetting(name) : nullptr;
  if (!setting) return -1;
  const std::string value = setting->GetText();
  if (buffer && size) {
    const size_t copied = std::min<size_t>(value.size(), size - 1);
    std::memcpy(buffer, value.data(), copied);
    buffer[copied] = '\0';
  }
  return static_cast<int32_t>(value.size());
}

int SetCvar(const char* name, const char* value) {
  const rex::SettingBase* setting = name && value ? FindSetting(name) : nullptr;
  return setting && REX_SETTINGS().SetText(setting->GetName(), value).has_value();
}

constexpr RexFabricApi kApi = {
    .version = REXFABRIC_API_VERSION,
    .size = sizeof(RexFabricApi),
    .log = Log,
    .is_hookable = IsHookable,
    .hook = Hook,
    .unhook = Unhook,
    .call_next = ApiCallNext,
    .call_address = CallAddress,
    .get_gpr = GetGpr,
    .set_gpr = SetGpr,
    .get_fpr = GetFpr,
    .set_fpr = SetFpr,
    .call_guest = CallGuest,
    .guest_pointer = GuestPointer,
    .read_u8 = Read<uint8_t>,
    .read_u16 = Read<uint16_t>,
    .read_u32 = Read<uint32_t>,
    .read_u64 = Read<uint64_t>,
    .read_f32 = Read<float>,
    .read_f64 = Read<double>,
    .write_u8 = Write<uint8_t>,
    .write_u16 = Write<uint16_t>,
    .write_u32 = Write<uint32_t>,
    .write_u64 = Write<uint64_t>,
    .write_f32 = Write<float>,
    .write_f64 = Write<double>,
    .find_function = FindFunctionC,
    .call_function = CallFunction,
    .find_type = FindTypeC,
    .type_size = TypeSize,
    .type_field = TypeField,
    .type_method = TypeMethod,
    .find_interface = FindInterfaceC,
    .listen = Listen,
    .unlisten = Unlisten,
    .claim_midasm = ClaimMidasmC,
    .release_midasm = ReleaseMidasmC,
    .midasm_address = MidasmAddress,
    .midasm_register = MidasmRegister,
    .get_switch = GetSwitchC,
    .set_switch = SetSwitchC,
    .get_cvar = GetCvar,
    .set_cvar = SetCvar,
};

}  // namespace

const RexFabricApi& Api() { return kApi; }
RexFabricMod* Host() { return &host_mod; }

}  // namespace rexfabric
