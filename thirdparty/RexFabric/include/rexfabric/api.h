// The C interface native mods (.asi, .dll, .so) are built against. Nothing here depends on the SDK, so a
// mod only breaks when this file does. RexFabricApi only ever grows: new members go at the end and bump
// REXFABRIC_API_VERSION, so a mod built against an older version keeps working on newer hosts.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REXFABRIC_API_VERSION 1

#if defined(_WIN32)
#define REXFABRIC_MOD_EXPORT __declspec(dllexport)
#else
#define REXFABRIC_MOD_EXPORT __attribute__((visibility("default")))
#endif

// The mod calling in. Its hooks, listeners, claims and log lines belong to it.
typedef struct RexFabricMod RexFabricMod;
// A hooked call in progress, valid until the hook returns.
typedef struct RexFabricCall RexFabricCall;
// A midasm site being run, valid until the handler returns.
typedef struct RexFabricMidasm RexFabricMidasm;
// Something the game declared for mods: a function, or a type of guest object.
typedef struct RexFabricFunction RexFabricFunction;
typedef struct RexFabricType RexFabricType;

// Zero means none.
typedef uint64_t RexFabricHookId;
typedef uint64_t RexFabricListenerId;
typedef uint64_t RexFabricClaimId;

typedef enum RexFabricLogLevel { REXFABRIC_LOG_DEBUG, REXFABRIC_LOG_INFO, REXFABRIC_LOG_WARN, REXFABRIC_LOG_ERROR } RexFabricLogLevel;

// A value crossing into or out of the game's API.
typedef enum RexFabricValueType { REXFABRIC_VALUE_NONE, REXFABRIC_VALUE_BOOL, REXFABRIC_VALUE_INT, REXFABRIC_VALUE_FLOAT, REXFABRIC_VALUE_STRING, REXFABRIC_VALUE_OBJECT } RexFabricValueType;

typedef struct RexFabricValue {
  uint32_t type;                     // RexFabricValueType
  uint32_t address;                  // OBJECT: where it is in guest memory
  const RexFabricType* object_type;  // OBJECT: what it is, or null when untyped
  int64_t i;                         // BOOL (0 or 1) and INT
  double f;                          // FLOAT
  const char* s;                     // STRING, UTF-8. A result's stays valid until this thread's next call.
} RexFabricValue;

// How a field of a game type sits in guest memory, big endian as the guest keeps it.
typedef enum RexFabricFieldKind {
  REXFABRIC_FIELD_BOOL8, REXFABRIC_FIELD_BOOL32, REXFABRIC_FIELD_S8, REXFABRIC_FIELD_U8, REXFABRIC_FIELD_S16, REXFABRIC_FIELD_U16, REXFABRIC_FIELD_S32, REXFABRIC_FIELD_U32,
  REXFABRIC_FIELD_S64, REXFABRIC_FIELD_U64, REXFABRIC_FIELD_F32, REXFABRIC_FIELD_F64,
  REXFABRIC_FIELD_POINTER,  // a 32 bit pointer to a `target`
  REXFABRIC_FIELD_STRUCT,   // a `target` held inline
  REXFABRIC_FIELD_CSTRING,  // a 32 bit pointer to a NUL terminated string
  REXFABRIC_FIELD_CHARS,    // `length` characters held inline, NUL padded
} RexFabricFieldKind;

typedef struct RexFabricField {
  uint32_t offset;
  uint32_t kind;    // RexFabricFieldKind
  uint32_t count;   // 0 for one value, otherwise an array of this many
  uint32_t stride;  // bytes between array elements
  uint32_t length;  // CHARS
  const RexFabricType* target;
} RexFabricField;

// A register a midasm stub lends. The host is little endian, so u32 and s32 are the low half of u64.
typedef union RexFabricRegister {
  uint64_t u64;
  uint32_t u32;
  int32_t s32;
  double f64;
} RexFabricRegister;

// Runs on the guest thread making the call. Call call_next to run the rest of the chain and the original
// function, or skip it to replace the function.
typedef void (*RexFabricHookFn)(RexFabricCall* call, void* user);
typedef void (*RexFabricEventFn)(const RexFabricValue* args, uint32_t count, void* user);
// Nonzero asks for the patch: the branch is taken.
typedef int (*RexFabricMidasmFn)(RexFabricMidasm* site, void* user);

typedef struct RexFabricApi {
  uint32_t version;  // the host's REXFABRIC_API_VERSION
  uint32_t size;     // the host's sizeof(RexFabricApi)

  void (*log)(RexFabricMod* mod, RexFabricLogLevel level, const char* message);

  // Hooks attach to guest function addresses. Hooks on one function run in the order they were added.
  int (*is_hookable)(uint32_t address);
  RexFabricHookId (*hook)(RexFabricMod* mod, uint32_t address, RexFabricHookFn fn, void* user);
  int (*unhook)(RexFabricHookId id);

  // Only valid inside a hook. Registers follow the PowerPC ABI: r3 to r10 carry integer arguments and r3 the
  // result, f1 to f13 carry float arguments and f1 the result.
  void (*call_next)(RexFabricCall* call);
  uint32_t (*call_address)(const RexFabricCall* call);
  uint64_t (*get_gpr)(const RexFabricCall* call, uint32_t index);
  void (*set_gpr)(RexFabricCall* call, uint32_t index, uint64_t value);
  double (*get_fpr)(const RexFabricCall* call, uint32_t index);
  void (*set_fpr)(RexFabricCall* call, uint32_t index, double value);
  // Calls the guest function at `address` with up to eight integer arguments and returns its r3. The hooked
  // call's registers are left as they were.
  uint64_t (*call_guest)(RexFabricCall* call, uint32_t address, const uint64_t* args, uint32_t count);

  // Guest memory, by guest virtual address. Reads and writes convert from and to the guest's big endian.
  void* (*guest_pointer)(uint32_t address);
  uint8_t (*read_u8)(uint32_t address);
  uint16_t (*read_u16)(uint32_t address);
  uint32_t (*read_u32)(uint32_t address);
  uint64_t (*read_u64)(uint32_t address);
  float (*read_f32)(uint32_t address);
  double (*read_f64)(uint32_t address);
  void (*write_u8)(uint32_t address, uint8_t value);
  void (*write_u16)(uint32_t address, uint16_t value);
  void (*write_u32)(uint32_t address, uint32_t value);
  void (*write_u64)(uint32_t address, uint64_t value);
  void (*write_f32)(uint32_t address, float value);
  void (*write_f64)(uint32_t address, double value);

  // The game's own API: what it declared for its mods, by full name ("tip.garden.money", "tip.Pinata").
  const RexFabricFunction* (*find_function)(const char* name);
  // Converts the arguments to what the function declares. Zero when they don't fit; `result` may be null.
  int (*call_function)(const RexFabricFunction* function, const RexFabricValue* args, uint32_t count, RexFabricValue* result);
  const RexFabricType* (*find_type)(const char* name);
  uint32_t (*type_size)(const RexFabricType* type);
  // Zero when the type has no such field.
  int (*type_field)(const RexFabricType* type, const char* name, RexFabricField* field);
  // A method takes the object as its first argument.
  const RexFabricFunction* (*type_method)(const RexFabricType* type, const char* name);
  // A plain C table the game published for native mods, at `min_version` or later, or null.
  const void* (*find_interface)(const char* name, uint32_t min_version);

  // Events: "frame", with the seconds since the last one, and whatever the game declares. Arguments are
  // valid during the call.
  RexFabricListenerId (*listen)(RexFabricMod* mod, const char* event, RexFabricEventFn fn, void* user);
  int (*unlisten)(RexFabricListenerId id);

  // Midasm sites: instructions partway through a function the game compiled a stub in for. Every claim on a
  // site runs, in claim order.
  RexFabricClaimId (*claim_midasm)(RexFabricMod* mod, uint32_t address, RexFabricMidasmFn fn, void* user);
  int (*release_midasm)(RexFabricClaimId id);
  uint32_t (*midasm_address)(const RexFabricMidasm* site);
  // A register the stub lent, by name ("r3", "f1"), or null when it lent no such register.
  RexFabricRegister* (*midasm_register)(RexFabricMidasm* site, const char* name);

  // Switches the game's compiled-in patches read. They start at 0.
  int32_t (*get_switch)(const char* name);
  void (*set_switch)(const char* name, int32_t value);

  // Cvars, as text. get_cvar writes up to `size` bytes with a NUL and returns the full length, or -1 when
  // there's no such cvar.
  int32_t (*get_cvar)(const char* name, char* buffer, uint32_t size);
  int (*set_cvar)(const char* name, const char* value);
} RexFabricApi;

// A native mod exports RexFabricModInit and may export RexFabricModShutdown. Init runs once guest memory exists
// and before the game starts; returning zero unloads the mod. A library without RexFabricModInit still loads, so
// plain .asi files can do their own setup in DllMain.
typedef int (*RexFabricModInitFn)(const RexFabricApi* api, RexFabricMod* mod);
typedef void (*RexFabricModShutdownFn)(void);

#define REXFABRIC_MOD_INIT_NAME "RexFabricModInit"
#define REXFABRIC_MOD_SHUTDOWN_NAME "RexFabricModShutdown"

#ifdef __cplusplus
}
#endif
