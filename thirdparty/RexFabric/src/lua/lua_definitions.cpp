#include "lua_definitions.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <rexfabric/game_api.h>

#include "../log.h"

namespace rexfabric::lua {
namespace {

constexpr std::string_view kWritten = "-- Written by RexFabric at every start, from what RexFabric and the game offer mods. Changes here are lost.\n\n";

// RexFabric's own API (docs/lua_mods.md). The registers of rex.Context are added after it.
constexpr std::string_view kRex = R"lua(---@meta rex

---The registers of a hooked call, read and written as the guest's. Only valid inside the hook that got it.
---@class rex.Context
---@field address integer The hooked function
---@field lr integer
---@field ctr integer
local Context = {}

---Register n as 64 bits.
---@param n integer
---@return integer
function Context:u64(n) end
---@param n integer
---@param value integer
function Context:set_u64(n, value) end
---Register n as a signed 32 bit integer.
---@param n integer
---@return integer
function Context:s32(n) end
---@param n integer
---@return integer
function Context:s64(n) end
---Float register n as single precision.
---@param n integer
---@return number
function Context:f32(n) end

---Any object of the game's types. Types the game holds by handle can outlive what they stand for.
---@class rex.Object
---@field address integer? Where it is in guest memory; nil once the game destroyed it
---@field type string The full name of its type
---@field valid boolean False once the game destroyed it

---Options every shape of rex.draw takes.
---@class rex.DrawOptions
---@field key string? Replaces the shape drawn with the same key
---@field rgb integer? 0xRRGGBB, white by default
---@field alpha number? 0 to 1
---@field seconds number? How long it stays; one frame by default
---@field thickness number? Pixels, for lines and outlines
---@field filled boolean? Rectangles and circles; true by default
---@field size number? Text: of the screen's height, 0.03 by default
---@field align "left"|"center"|"right"? Text
---@field background boolean? Text

rex = {}

---The SDK's version.
---@type string
rex.version = ""

---Runs `hook` before the function. Returning exactly false skips the original, once every before hook ran.
---@param fn integer|rex.FunctionName The function's address, or the name the game gave it
---@param hook fun(ctx: rex.Context): boolean?
---@return integer id For rex.unhook
function rex.hook(fn, hook) end

---Runs `hook` after the function.
---@param fn integer|rex.FunctionName The function's address, or the name the game gave it
---@param hook fun(ctx: rex.Context)
---@return integer id For rex.unhook
function rex.hook_after(fn, hook) end

---@param id integer
---@return boolean removed
function rex.unhook(id) end

---Every Lua hook.
---@return {id: integer, address: integer, name: string?, phase: "pre"|"post", owner: string}[]
function rex.hooks() end

---Whether a function can be hooked; false for a name the game doesn't have.
---@param fn integer|rex.FunctionName
---@return boolean
function rex.hookable(fn) end

---Calls a guest function with up to eight integer arguments and returns its r3. Only on a guest thread: inside a
---hook, or in a frame event when the game fires frames from its present.
---@param fn integer|rex.FunctionName The function's address, or the name the game gave it
---@param ... integer
---@return integer
function rex.call(fn, ...) end

---Claims a midasm site the game compiled in. At a branch, returning true takes it.
---@param address integer
---@param fn fun(site: table<string, number>): boolean?
---@return integer id For rex.unmidasm
function rex.midasm(address, fn) end
---@param id integer
function rex.unmidasm(id) end
---@return {id: integer, address: integer, owner: string}[]
function rex.midasms() end

---Listens for an event: "frame", with the seconds since the last one, or one the game declares.
---@param event string
---@param fn fun(...)
---@return integer id
function rex.on(event, fn) end

---Runs `fn` once, `seconds` from now, on the frame thread.
---@param seconds number
---@param fn fun()
---@return integer id For rex.cancel
function rex.after(seconds, fn) end
---Runs `fn` every `seconds`, at most once a frame, until it is cancelled.
---@param seconds number
---@param fn fun()
---@return integer id For rex.cancel
function rex.every(seconds, fn) end
---@param id integer?
function rex.cancel(id) end

---Runs `fn(...)` now as a coroutine, so it may wait with rex.wait and rex.wait_for.
---@param fn fun(...)
---@param ... any
function rex.run(fn, ...) end
---Waits `seconds`, inside a function rex.run runs.
---@param seconds number
function rex.wait(seconds) end
---Waits for the event's next firing whose arguments `filter` says yes to, inside a function rex.run runs, and gives
---its arguments; nothing after `timeout` seconds, when given.
---@param event string
---@param filter (fun(...): boolean)?
---@param timeout number?
---@return any ...
function rex.wait_for(event, filter, timeout) end

---The calling mod's own table, kept in the save of the world the game is in: booleans, numbers, text, and tables of
---them (lists, or text keys). The same table for as long as the scripts run; a world loaded fills it in place.
---@return table
function rex.world_data() end

---Text in the screen's top right corner for a while, newest at the top, for debugging.
---@param text string
---@param seconds number? Two by default
---@param rgb integer? 0x00A8FF by default
function rex.screen_text(text, seconds, rgb) end

---@param ... any
function rex.log(...) end
---@param ... any
function rex.info(...) end
---@param ... any
function rex.warn(...) end
---@param ... any
function rex.error(...) end
---@param ... any
function rex.debug(...) end

---A clock in seconds; only differences mean anything.
---@return number
function rex.now() end

---Guest memory, by guest address. Values convert from and to the guest's big endian.
rex.mem = {}
---@param address integer
---@param max integer? 4096 by default
---@return string
function rex.mem.string(address, max) end
---@param address integer
---@param text string
function rex.mem.set_string(address, text) end
---A UTF-16 string, as UTF-8.
---@param address integer
---@param max integer? 4096 by default
---@return string
function rex.mem.wstring(address, max) end
---@param address integer
---@param count integer
---@return string
function rex.mem.bytes(address, count) end
---@param address integer
---@param bytes string
function rex.mem.set_bytes(address, bytes) end

---Switches the game's compiled-in code reads.
rex.switch = {}
---@param name string
---@param value integer|boolean
function rex.switch.set(name, value) end
---@param name string
---@return integer
function rex.switch.get(name) end
---@return table<string, integer>
function rex.switch.list() end

---Settings a mod defines, saved with the game's.
rex.cvar = {}
---@param name string
---@return any
function rex.cvar.get(name) end
---@param name string
---@param value any
function rex.cvar.set(name, value) end
---@param name string
---@return boolean
function rex.cvar.exists(name) end
---@param category string?
---@return string[]
function rex.cvar.list(category) end
---Runs `fn` on the next frame after the setting changes.
---@param name string
---@param fn fun(name: string, value: any)
function rex.cvar.on_change(name, fn) end

---Shapes and text drawn over the game. Positions and sizes are fractions of the screen.
rex.draw = {}
---@param text string
---@param x number
---@param y number
---@param options rex.DrawOptions?
function rex.draw.text(text, x, y, options) end
---@param x number
---@param y number
---@param width number
---@param height number
---@param options rex.DrawOptions?
function rex.draw.rect(x, y, width, height, options) end
---@param x1 number
---@param y1 number
---@param x2 number
---@param y2 number
---@param options rex.DrawOptions?
function rex.draw.line(x1, y1, x2, y2, options) end
---@param x number
---@param y number
---@param radius number Of the screen's height
---@param options rex.DrawOptions?
function rex.draw.circle(x, y, radius, options) end
---@param key string
function rex.draw.remove(key) end
---@param prefix string? Only the shapes whose key starts with it
function rex.draw.clear(prefix) end

---The keyboard and mouse, through the game.
rex.input = {}
---@return boolean
function rex.input.available() end
---@param key string "W", "F9", "Shift"
---@return boolean
function rex.input.down(key) end
---True once when the key goes down.
---@param key string
---@return boolean
function rex.input.pressed(key) end
---@param n integer 0 left, 1 right, 2 middle, 3 and 4 the side buttons
---@return boolean
function rex.input.mouse_button(n) end
---@return number dx
---@return number dy
function rex.input.mouse_delta() end
---@return number
function rex.input.wheel() end
---Whether the game window has the focus and no menu wants the input. Check it before acting on a hotkey.
---@return boolean
function rex.input.accepts() end
---@param held boolean
function rex.input.hold_guest(held) end
function rex.input.flush() end
)lua";

// Lua names for what crosses between the game and a mod.
std::string LuaType(ValueType type) {
  switch (type) {
    case ValueType::kNone: return "nil";
    case ValueType::kBool: return "boolean";
    case ValueType::kInt: return "integer";
    case ValueType::kFloat: return "number";
    case ValueType::kString: return "string";
    case ValueType::kObject: return "rex.Object";
  }
  return "any";
}

std::string LuaType(const FieldType& type) {
  switch (type.kind) {
    case FieldKind::kBool8: case FieldKind::kBool32: return "boolean";
    case FieldKind::kF32: case FieldKind::kF64: return "number";
    case FieldKind::kPointer: return (type.target ? type.target->name() : std::string("rex.Object")) + "?";
    case FieldKind::kStruct: return type.target ? type.target->name() : std::string("rex.Object");
    case FieldKind::kCString: return "string?";
    case FieldKind::kChars: return "string";
    default: return "integer";
  }
}

// A declaration's parameter names and types from Namespace::Args ("id", "part:Part", "->Vehicle"), where the
// namespace has them.
struct NamedParams {
  std::vector<std::pair<std::string, std::string>> params;  // name, Lua type (empty: from the declaration)
  std::string result;                                       // Lua type, or empty
};

NamedParams ParamsOf(const NamespaceDef& space, const std::string& full_name) {
  NamedParams named;
  const auto found = std::find_if(space.args.begin(), space.args.end(), [&](const auto& entry) { return entry.first == full_name; });
  if (found == space.args.end()) return named;
  const auto lua_type = [&space](std::string type) {
    const bool optional = type.ends_with('?');
    if (optional) type.pop_back();
    const std::string full = space.name + "." + type;
    const bool known = FindType(full) || std::any_of(space.enums.begin(), space.enums.end(), [&](const EnumDef& e) { return e.name == full; });
    return known ? full + (optional ? "?" : "") : std::string();
  };
  for (const std::string& entry : found->second) {
    if (entry.starts_with("->")) {
      named.result = lua_type(entry.substr(2));
      continue;
    }
    const size_t colon = entry.find(':');
    named.params.emplace_back(entry.substr(0, colon), colon == std::string::npos ? std::string() : lua_type(entry.substr(colon + 1)));
  }
  return named;
}

// A function's annotations and its empty body, as `qualified` (tip.garden.give, or tip.Pinata:feed for a method,
// whose first declared parameter is the object itself).
void WriteFunction(std::ostream& out, const NamespaceDef& space, const FunctionDef& function, const std::string& qualified, bool method) {
  const NamedParams named = ParamsOf(space, function.name);
  std::vector<std::string> names;
  const size_t first = method ? 1 : 0;
  for (size_t i = first; i < function.params.size(); ++i) {
    const size_t at = i - first;
    const bool has_name = at < named.params.size() && !named.params[at].first.empty();
    const std::string name = has_name ? named.params[at].first : "arg" + std::to_string(at + 1);
    const std::string type = at < named.params.size() && !named.params[at].second.empty() ? named.params[at].second : LuaType(function.params[i]);
    out << "---@param " << name << " " << type << "\n";
    names.push_back(name);
  }
  if (function.result != ValueType::kNone) out << "---@return " << (named.result.empty() ? LuaType(function.result) : named.result) << "\n";
  out << "function " << qualified << "(";
  for (size_t i = 0; i < names.size(); ++i) out << (i ? ", " : "") << names[i];
  out << ") end\n\n";
}

// Every table on the way to a dotted name ("tip.garden.money" needs tip.garden), declared once.
void DeclareTables(std::ostream& out, std::set<std::string>& declared, std::string_view name) {
  for (size_t dot = name.find('.'); dot != std::string_view::npos; dot = name.find('.', dot + 1)) {
    const std::string table(name.substr(0, dot));
    if (declared.insert(table).second) out << table << " = " << table << " or {}\n";
  }
}

std::string WriteNamespace(const NamespaceDef& space) {
  std::ostringstream out;
  out << "---@meta " << space.name << "\n\n" << kWritten;
  std::set<std::string> tables;
  DeclareTables(out, tables, space.name + ".");
  out << "\n";

  for (const EnumDef& enumeration : space.enums) {
    DeclareTables(out, tables, enumeration.name);
    out << "---@enum " << enumeration.name << "\n" << enumeration.name << " = {\n";
    for (const auto& [name, value] : enumeration.values) out << "  " << name << " = " << value << ",\n";
    out << "}\n\n";
  }

  for (const ConstantDef& constant : space.constants) {
    DeclareTables(out, tables, constant.name);
    out << "---@type " << LuaType(TypeOf(constant.value)) << "\n" << constant.name << " = nil\n\n";
  }

  for (const auto& type : space.types) {
    DeclareTables(out, tables, type->name());
    out << "---@class " << type->name() << " : rex.Object\n";
    for (const FieldDef& field : type->fields()) {
      out << "---@field " << field.name << " " << LuaType(field.type) << (field.count ? "[]" : "") << "\n";
    }
    out << "---@overload fun(address: integer|rex.Object): " << type->name() << "?\n" << type->name() << " = {}\n\n";
    for (const auto& [name, method] : type->methods()) WriteFunction(out, space, method, type->name() + ":" + name, true);
  }

  for (const FunctionDef& function : space.functions) {
    DeclareTables(out, tables, function.name);
    WriteFunction(out, space, function, function.name, false);
  }

  if (!space.events.empty()) {
    out << "---The events the game fires, for rex.on.\n---@alias " << space.name << ".Event\n";
    for (const std::string& event : space.events) out << "---| \"" << event << "\"\n";
  }
  return out.str();
}

// rex.FunctionName: every guest function the game named, each with what it is, so editors complete them in rex.hook.
std::string WriteFunctionNames() {
  std::ostringstream out;
  out << "---@meta functions\n\n" << kWritten << "---A guest function by the name the game gave it.\n---@alias rex.FunctionName\n";
  for (const GuestFunctionName& function : GuestFunctionNames()) {
    out << "---| \"" << function.name << "\"";
    if (!function.signature.empty() || !function.source.empty()) out << " # " << function.signature << (function.signature.empty() || function.source.empty() ? "" : ", ") << function.source;
    out << "\n";
  }
  if (GuestFunctionNames().empty()) out << "---| string\n";
  return out.str();
}

std::string WriteReference() {
  std::ostringstream out;
  out << "# What mods can use\n\n" << "Written by RexFabric at every start, from what RexFabric and the game offer mods. The Lua definitions beside "
      << "this file are the same for editors.\n\n";
  for (const auto& space : Namespaces()) {
    out << "## " << space->name << "\n\n";
    if (!space->functions.empty()) {
      out << "| Function | Takes | Gives |\n|---|---|---|\n";
      for (const FunctionDef& function : space->functions) {
        const NamedParams named = ParamsOf(*space, function.name);
        std::string takes;
        for (size_t i = 0; i < function.params.size(); ++i) {
          const std::string name = i < named.params.size() ? named.params[i].first : "arg" + std::to_string(i + 1);
          takes += (i ? ", " : "") + name + " " + LuaType(function.params[i]);
        }
        out << "| `" << function.name << "` | " << takes << " | " << (function.result == ValueType::kNone ? "" : LuaType(function.result)) << " |\n";
      }
      out << "\n";
    }
    for (const auto& type : space->types) {
      out << "### " << type->name() << (type->held() ? ", held by handle" : "") << "\n\n";
      if (!type->fields().empty()) {
        out << "| Field | Offset | Type |\n|---|---|---|\n";
        for (const FieldDef& field : type->fields()) out << "| `" << field.name << "` | +0x" << std::hex << field.offset << std::dec << " | " << LuaType(field.type) << (field.count ? "[" + std::to_string(field.count) + "]" : "") << " |\n";
        out << "\n";
      }
    }
  }
  const std::vector<GuestFunctionName>& functions = GuestFunctionNames();
  out << "## Guest functions by name\n\n" << functions.size() << " functions, for `rex.hook`, `rex.hook_after`, `rex.hookable` and `rex.call`.\n\n";
  if (!functions.empty()) {
    out << "| Name | Address | C++ type | Source |\n|---|---|---|---|\n";
    for (const GuestFunctionName& function : functions) {
      char address[16];
      std::snprintf(address, sizeof(address), "%08X", function.address);
      out << "| `" << function.name << "` | " << address << " | `" << function.signature << "` | " << function.source << " |\n";
    }
  }
  return out.str();
}

void WriteFile(const std::filesystem::path& path, std::string_view text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!file) RF_WARN("couldn't write the Lua definitions to {}", path.string());
}

std::string RexDefinitions() {
  std::string text(kRex);
  std::string registers = "---@class rex.Context\n";
  for (int i = 0; i < 32; ++i) registers += "---@field r" + std::to_string(i) + " integer\n";
  for (int i = 0; i < 32; ++i) registers += "---@field f" + std::to_string(i) + " number\n";
  text.insert(text.find("local Context = {}"), registers.substr(registers.find('\n') + 1));
  text += "\n";
  for (const char* kind : {"u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64", "f32", "f64"}) {
    const std::string type = kind[0] == 'f' ? "number" : "integer";
    text += "---@param address integer\n---@return " + type + "\nfunction rex.mem." + kind + "(address) end\n";
    text += "---@param address integer\n---@param value " + type + "\nfunction rex.mem.set_" + kind + "(address, value) end\n";
  }
  for (const char* kind : {"boolean", "int32", "int64", "uint32", "uint64", "number", "string"}) {
    text += std::string("---Defines a setting a mod keeps, shown in the settings.\n---@param name string\n---@param default any\n---@param category string\n---@param help string\nfunction rex.cvar.") + kind + "(name, default, category, help) end\n";
  }
  return text;
}

}  // namespace

void WriteDefinitions(const std::filesystem::path& mods_dir) {
  const std::filesystem::path dir = mods_dir / ".api";
  std::error_code error;
  std::filesystem::remove_all(dir, error);
  std::filesystem::create_directories(dir, error);
  if (error) {
    RF_WARN("couldn't make {} for the Lua definitions: {}", dir.string(), error.message());
    return;
  }
  WriteFile(dir / "rex.lua", RexDefinitions());
  for (const auto& space : Namespaces()) WriteFile(dir / (space->name + ".lua"), WriteNamespace(*space));
  WriteFile(dir / "functions.lua", WriteFunctionNames());
  WriteFile(dir / "reference.md", WriteReference());
  const std::filesystem::path luarc = mods_dir / ".luarc.json";
  if (!std::filesystem::exists(luarc, error)) {
    WriteFile(luarc, R"({
  "runtime.version": "Lua 5.4",
  "workspace.library": [".api"],
  "workspace.ignoreDir": [".api"]
}
)");
  }
  RF_INFO("Lua definitions for editors in {} ({} named guest functions)", dir.string(), GuestFunctionNames().size());
}

}  // namespace rexfabric::lua
