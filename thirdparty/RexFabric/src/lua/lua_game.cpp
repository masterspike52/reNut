// The game's namespaces in Lua. Each is a global table; dotted names nest inside it.
//
//   tip.version()                      -- a function
//   tip.max_pinatas, tip.Species.Whirlm -- constants and enums
//   local p = tip.Pinata(ctx.r3)        -- an object of a game type, or nil for a null address
//   p.happiness = p.happiness + 1       -- fields read and write guest memory
//   p:feed(3)                           -- methods
//   p.needs[1]                          -- arrays index from 1, as Lua tables do

#include <exception>
#include <string>

#include "../guest.h"
#include "../log.h"
#include "lua_vm.h"

namespace rexfabric::lua {
namespace {

constexpr const char* kObjectMeta = "rexfabric.object";
constexpr const char* kArrayMeta = "rexfabric.array";

struct ObjectUD {
  uint32_t address;
  const TypeDef* type;
};

struct ArrayUD {
  uint32_t address;  // the first element
  const FieldDef* field;
};

void PushObject(lua_State* L, Object object) {
  if (!object.address) {
    lua_pushnil(L);
    return;
  }
  auto* ud = static_cast<ObjectUD*>(lua_newuserdatauv(L, sizeof(ObjectUD), 0));
  *ud = {object.address, object.type};
  luaL_setmetatable(L, kObjectMeta);
}

// Accepts an object, an address, or nil for none.
Object CheckObject(lua_State* L, int arg) {
  if (lua_isnoneornil(L, arg)) return {};
  if (auto* ud = static_cast<ObjectUD*>(luaL_testudata(L, arg, kObjectMeta))) return {ud->address, ud->type};
  const lua_Integer address = luaL_checkinteger(L, arg);
  if (address < 0 || address > 0xFFFFFFFFll) luaL_argerror(L, arg, "guest address out of range");
  return {static_cast<uint32_t>(address), nullptr};
}

Value CheckValue(lua_State* L, int arg, ValueType type) {
  switch (type) {
    case ValueType::kNone: return {};
    case ValueType::kBool: return Value(lua_toboolean(L, arg) != 0);
    case ValueType::kInt:
      // An object where an integer goes is its address, for functions that take addresses (Namespace::Args types).
      if (auto* ud = static_cast<ObjectUD*>(luaL_testudata(L, arg, kObjectMeta))) return Value(static_cast<int64_t>(ud->address));
      return Value(static_cast<int64_t>(luaL_checkinteger(L, arg)));
    case ValueType::kFloat: return Value(static_cast<double>(luaL_checknumber(L, arg)));
    case ValueType::kString: {
      // With its length: a string can hold zero bytes (binary data), which a C string would end at.
      size_t length = 0;
      const char* text = luaL_checklstring(L, arg, &length);
      return Value(std::string(text, length));
    }
    case ValueType::kObject: return Value(CheckObject(L, arg));
  }
  return {};
}

// A game function: upvalue 1 is its FunctionDef.
int CallFunction(lua_State* L) {
  const auto* function = static_cast<const FunctionDef*>(lua_touserdata(L, lua_upvalueindex(1)));
  std::vector<Value> args;
  args.reserve(function->params.size());
  for (size_t i = 0; i < function->params.size(); ++i) args.push_back(CheckValue(L, static_cast<int>(i + 1), function->params[i]));
  Value result;
  try {
    result = function->invoke(args);
  } catch (const std::exception& error) {
    return luaL_error(L, "%s: %s", function->name.c_str(), error.what());
  }
  if (TypeOf(result) == ValueType::kNone) return 0;
  PushValue(L, result);
  return 1;
}

void PushFunction(lua_State* L, const FunctionDef& function) {
  lua_pushlightuserdata(L, const_cast<FunctionDef*>(&function));
  lua_pushcclosure(L, CallFunction, 1);
}

// Fields -------------------------------------------------------------------------------------------------

void ReadField(lua_State* L, uint32_t address, const FieldType& type) {
  if (!guest::Translate(address)) luaL_error(L, "guest memory is not mapped yet");
  switch (type.kind) {
    case FieldKind::kBool8: lua_pushboolean(L, guest::Read<uint8_t>(address) != 0); return;
    case FieldKind::kBool32: lua_pushboolean(L, guest::Read<uint32_t>(address) != 0); return;
    case FieldKind::kS8: lua_pushinteger(L, guest::Read<int8_t>(address)); return;
    case FieldKind::kU8: lua_pushinteger(L, guest::Read<uint8_t>(address)); return;
    case FieldKind::kS16: lua_pushinteger(L, guest::Read<int16_t>(address)); return;
    case FieldKind::kU16: lua_pushinteger(L, guest::Read<uint16_t>(address)); return;
    case FieldKind::kS32: lua_pushinteger(L, guest::Read<int32_t>(address)); return;
    case FieldKind::kU32: lua_pushinteger(L, guest::Read<uint32_t>(address)); return;
    case FieldKind::kS64: lua_pushinteger(L, guest::Read<int64_t>(address)); return;
    case FieldKind::kU64: lua_pushinteger(L, static_cast<lua_Integer>(guest::Read<uint64_t>(address))); return;
    case FieldKind::kF32: lua_pushnumber(L, guest::Read<float>(address)); return;
    case FieldKind::kF64: lua_pushnumber(L, guest::Read<double>(address)); return;
    case FieldKind::kPointer: PushObject(L, {guest::Read<uint32_t>(address), type.target}); return;
    case FieldKind::kStruct: PushObject(L, {address, type.target}); return;
    case FieldKind::kCString: {
      const uint32_t text = guest::Read<uint32_t>(address);
      if (text) lua_pushstring(L, reinterpret_cast<const char*>(guest::Translate(text)));
      else lua_pushnil(L);
      return;
    }
    case FieldKind::kChars: {
      const char* text = reinterpret_cast<const char*>(guest::Translate(address));
      lua_pushlstring(L, text, strnlen(text, type.length));
      return;
    }
  }
}

void WriteField(lua_State* L, uint32_t address, const FieldType& type, const std::string& name) {
  if (!guest::Translate(address)) luaL_error(L, "guest memory is not mapped yet");
  switch (type.kind) {
    case FieldKind::kBool8: guest::Write<uint8_t>(address, lua_toboolean(L, 3) ? 1 : 0); return;
    case FieldKind::kBool32: guest::Write<uint32_t>(address, lua_toboolean(L, 3) ? 1 : 0); return;
    case FieldKind::kS8: case FieldKind::kU8: guest::Write<uint8_t>(address, static_cast<uint8_t>(luaL_checkinteger(L, 3))); return;
    case FieldKind::kS16: case FieldKind::kU16: guest::Write<uint16_t>(address, static_cast<uint16_t>(luaL_checkinteger(L, 3))); return;
    case FieldKind::kS32: case FieldKind::kU32: guest::Write<uint32_t>(address, static_cast<uint32_t>(luaL_checkinteger(L, 3))); return;
    case FieldKind::kS64: case FieldKind::kU64: guest::Write<uint64_t>(address, static_cast<uint64_t>(luaL_checkinteger(L, 3))); return;
    case FieldKind::kF32: guest::Write<float>(address, static_cast<float>(luaL_checknumber(L, 3))); return;
    case FieldKind::kF64: guest::Write<double>(address, luaL_checknumber(L, 3)); return;
    case FieldKind::kPointer: guest::Write<uint32_t>(address, CheckObject(L, 3).address); return;
    case FieldKind::kChars: {
      size_t length = 0;
      const char* text = luaL_checklstring(L, 3, &length);
      char* out = reinterpret_cast<char*>(guest::Translate(address));
      const size_t copied = std::min<size_t>(length, type.length ? type.length - 1 : 0);
      std::memcpy(out, text, copied);
      std::memset(out + copied, 0, type.length - copied);
      return;
    }
    case FieldKind::kStruct: case FieldKind::kCString: break;
  }
  luaL_error(L, "field '%s' can't be assigned", name.c_str());
}

// Objects ------------------------------------------------------------------------------------------------

int ObjectIndex(lua_State* L) {
  const auto* self = static_cast<ObjectUD*>(luaL_checkudata(L, 1, kObjectMeta));
  const std::string_view key = luaL_checkstring(L, 2);
  if (key == "address") {
    lua_pushinteger(L, self->address);
    return 1;
  }
  if (key == "type") {
    if (self->type) lua_pushstring(L, self->type->name().c_str());
    else lua_pushnil(L);
    return 1;
  }
  if (!self->type) return luaL_error(L, "an untyped object at 0x%08X has no field '%s'", self->address, lua_tostring(L, 2));
  if (const FieldDef* field = self->type->FindField(key)) {
    if (field->count) {
      auto* array = static_cast<ArrayUD*>(lua_newuserdatauv(L, sizeof(ArrayUD), 0));
      *array = {self->address + field->offset, field};
      luaL_setmetatable(L, kArrayMeta);
    } else {
      ReadField(L, self->address + field->offset, field->type);
    }
    return 1;
  }
  if (const FunctionDef* method = self->type->FindMethod(key)) {
    PushFunction(L, *method);
    return 1;
  }
  return luaL_error(L, "%s has no field '%s'", self->type->name().c_str(), lua_tostring(L, 2));
}

int ObjectNewIndex(lua_State* L) {
  const auto* self = static_cast<ObjectUD*>(luaL_checkudata(L, 1, kObjectMeta));
  const std::string_view key = luaL_checkstring(L, 2);
  const FieldDef* field = self->type ? self->type->FindField(key) : nullptr;
  if (!field) return luaL_error(L, "%s has no field '%s'", self->type ? self->type->name().c_str() : "an untyped object", lua_tostring(L, 2));
  if (field->count) return luaL_error(L, "array field '%s' can't be assigned; assign its elements", field->name.c_str());
  WriteField(L, self->address + field->offset, field->type, field->name);
  return 0;
}

int ObjectEq(lua_State* L) {
  const auto* a = static_cast<ObjectUD*>(luaL_checkudata(L, 1, kObjectMeta));
  const auto* b = static_cast<ObjectUD*>(luaL_checkudata(L, 2, kObjectMeta));
  lua_pushboolean(L, a->address == b->address && a->type == b->type);
  return 1;
}

int ObjectToString(lua_State* L) {
  const auto* self = static_cast<ObjectUD*>(luaL_checkudata(L, 1, kObjectMeta));
  lua_pushfstring(L, "%s(0x%08X)", self->type ? self->type->name().c_str() : "object", self->address);
  return 1;
}

// Arrays -------------------------------------------------------------------------------------------------

uint32_t CheckElement(lua_State* L, const ArrayUD& array) {
  const lua_Integer index = luaL_checkinteger(L, 2);
  if (index < 1 || index > array.field->count) luaL_error(L, "index %I out of range [1, %d] for '%s'", index, static_cast<int>(array.field->count), array.field->name.c_str());
  return array.address + static_cast<uint32_t>(index - 1) * array.field->stride;
}

int ArrayIndex(lua_State* L) {
  const auto* array = static_cast<ArrayUD*>(luaL_checkudata(L, 1, kArrayMeta));
  ReadField(L, CheckElement(L, *array), array->field->type);
  return 1;
}

int ArrayNewIndex(lua_State* L) {
  const auto* array = static_cast<ArrayUD*>(luaL_checkudata(L, 1, kArrayMeta));
  WriteField(L, CheckElement(L, *array), array->field->type, array->field->name);
  return 0;
}

int ArrayLen(lua_State* L) {
  lua_pushinteger(L, static_cast<ArrayUD*>(luaL_checkudata(L, 1, kArrayMeta))->field->count);
  return 1;
}

// Types --------------------------------------------------------------------------------------------------

// tip.Pinata(address): upvalue 1 is the TypeDef. Argument 1 is the type table itself, from __call.
int ConstructObject(lua_State* L) {
  const auto* type = static_cast<const TypeDef*>(lua_touserdata(L, lua_upvalueindex(1)));
  PushObject(L, {CheckObject(L, 2).address, type});
  return 1;
}

void PushType(lua_State* L, const TypeDef& type) {
  lua_newtable(L);
  lua_pushstring(L, type.name().c_str());
  lua_setfield(L, -2, "name");
  lua_pushinteger(L, type.size());
  lua_setfield(L, -2, "size");
  lua_newtable(L);
  lua_pushlightuserdata(L, const_cast<TypeDef*>(&type));
  lua_pushcclosure(L, ConstructObject, 1);
  lua_setfield(L, -2, "__call");
  lua_setmetatable(L, -2);
}

// Sets the value on top of the stack at `name` ("tip.garden.money") under the globals, making the tables
// on the way, and pops it.
void SetPath(lua_State* L, std::string_view name) {
  const int value = lua_gettop(L);
  lua_pushglobaltable(L);
  size_t start = 0;
  for (size_t dot = name.find('.'); dot != std::string_view::npos; start = dot + 1, dot = name.find('.', start)) {
    const std::string part(name.substr(start, dot - start));
    lua_getfield(L, -1, part.c_str());
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      lua_newtable(L);
      lua_pushvalue(L, -1);
      lua_setfield(L, -3, part.c_str());
    }
    lua_remove(L, -2);
  }
  lua_pushvalue(L, value);
  lua_setfield(L, -2, std::string(name.substr(start)).c_str());
  lua_settop(L, value - 1);
}

}  // namespace

void PushValue(lua_State* L, const Value& value) {
  switch (TypeOf(value)) {
    case ValueType::kNone: lua_pushnil(L); return;
    case ValueType::kBool: lua_pushboolean(L, std::get<bool>(value)); return;
    case ValueType::kInt: lua_pushinteger(L, std::get<int64_t>(value)); return;
    case ValueType::kFloat: lua_pushnumber(L, std::get<double>(value)); return;
    case ValueType::kString: lua_pushlstring(L, std::get<std::string>(value).data(), std::get<std::string>(value).size()); return;
    case ValueType::kObject: PushObject(L, std::get<Object>(value)); return;
  }
}

void OpenGame(lua_State* L) {
  luaL_newmetatable(L, kObjectMeta);
  lua_pushcfunction(L, ObjectIndex);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, ObjectNewIndex);
  lua_setfield(L, -2, "__newindex");
  lua_pushcfunction(L, ObjectEq);
  lua_setfield(L, -2, "__eq");
  lua_pushcfunction(L, ObjectToString);
  lua_setfield(L, -2, "__tostring");
  lua_pop(L, 1);

  luaL_newmetatable(L, kArrayMeta);
  lua_pushcfunction(L, ArrayIndex);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, ArrayNewIndex);
  lua_setfield(L, -2, "__newindex");
  lua_pushcfunction(L, ArrayLen);
  lua_setfield(L, -2, "__len");
  lua_pop(L, 1);

  for (const auto& ns : Namespaces()) {
    if (ns->name == "rex") {
      RF_ERROR("the game namespace \"rex\" is RexFabric's own; rename it");
      continue;
    }
    for (const FunctionDef& function : ns->functions) {
      PushFunction(L, function);
      SetPath(L, function.name);
    }
    for (const ConstantDef& constant : ns->constants) {
      PushValue(L, constant.value);
      SetPath(L, constant.name);
    }
    for (const EnumDef& enumeration : ns->enums) {
      lua_createtable(L, 0, static_cast<int>(enumeration.values.size()));
      for (const auto& [name, value] : enumeration.values) {
        lua_pushinteger(L, value);
        lua_setfield(L, -2, name.c_str());
      }
      SetPath(L, enumeration.name);
    }
    for (const auto& type : ns->types) {
      PushType(L, *type);
      SetPath(L, type->name());
    }
  }
  // Each namespace's Lua sees every namespace's globals.
  for (const auto& ns : Namespaces()) {
    for (const std::string& source : ns->lua) {
      const std::string chunk = "=" + ns->name;
      if (luaL_loadbuffer(L, source.data(), source.size(), chunk.c_str()) != LUA_OK) {
        RF_ERROR("the {} namespace's Lua: {}", ns->name, lua_tostring(L, -1));
        lua_pop(L, 1);
        continue;
      }
      CallProtected(L, 0, 0, chunk.c_str() + 1);
    }
  }
}

}  // namespace rexfabric::lua
