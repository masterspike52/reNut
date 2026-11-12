#include "renut_engine/game/custom_materials.h"
#include <rexfabric/game_api.h>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>
#include "renut_engine/gpu/render/world_materials.h"
#include "renut_engine/mods/custom_parts.h"

namespace renut::game::custom_materials {

namespace {

constexpr uint8_t kTypeModel = 0x04;

uint32_t Table(const std::string &model) {
  return renut::mods::AssetId(kTypeModel, model);
}

std::vector<std::string> Lines(const std::string &text) {
  std::vector<std::string> out;
  size_t at = 0;
  while (at <= text.size() && !text.empty()) {
    const size_t end = text.find('\n', at);
    out.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
    if (end == std::string::npos)
      break;
    at = end + 1;
  }
  return out;
}

constexpr const char kLua[] = R"lua(
local materials = renut.materials

local function model_name(model, what)
  assert(type(model) == "string" and #model > 0, what .. ": the model is its asset name (renutworld_<id>, ...)")
  return (model:gsub("^aid_model_", ""))
end

local function index_of(model, name, what)
  local i = materials._index(model, tostring(name))
  if i < 0 then error(what .. ": " .. model .. " has no material '" .. tostring(name) .. "'", 3) end
  return i
end

local function number(v, what)
  assert(type(v) == "number" and v == v and v ~= math.huge and v ~= -math.huge, what .. " is a number")
  return v
end

-- renut.materials.define{model = m, names = {...}}: a model's materials by name (NutTool writes it).
function materials.define(def)
  assert(type(def) == "table", "renut.materials.define takes a table")
  local model = model_name(def.model, "renut.materials.define")
  local names = {}
  for i, name in ipairs(def.names or {}) do
    assert(type(name) == "string" and not name:find("\n"), "renut.materials.define: names are strings")
    names[i] = name
  end
  materials._define(model, table.concat(names, "\n"))
end

-- renut.materials.set(model, name, {emissive = {r, g, b}, emissive_strength = n, glow = n, opacity = n}).
function materials.set(model, name, values)
  model = model_name(model, "renut.materials.set")
  assert(type(values) == "table", "renut.materials.set: the values are a table")
  local i = index_of(model, name, "renut.materials.set")
  if values.emissive ~= nil then
    local e = values.emissive
    assert(type(e) == "table", "renut.materials.set: emissive is {r, g, b}")
    materials._set_emissive(model, i, number(e[1] or e.r or e.x, "emissive r"), number(e[2] or e.g or e.y, "emissive g"),
                            number(e[3] or e.b or e.z, "emissive b"))
  end
  local mask, strength, glow, opacity = 0, 1, -1, 1
  if values.emissive_strength ~= nil then strength = number(values.emissive_strength, "emissive_strength"); mask = mask | 2 end
  if values.glow ~= nil then glow = number(values.glow, "glow"); mask = mask | 4 end
  if values.opacity ~= nil then opacity = number(values.opacity, "opacity"); mask = mask | 8 end
  if mask ~= 0 then materials._set(model, i, mask, strength, glow, opacity) end
end

-- renut.materials.reset(model, name): back to how NutTool made it. list(model): its names.
function materials.reset(model, name)
  model = model_name(model, "renut.materials.reset")
  materials._reset(model, index_of(model, name, "renut.materials.reset"))
end

function materials.list(model)
  model = model_name(model, "renut.materials.list")
  local list = {}
  for i = 0, materials._count(model) - 1 do list[#list + 1] = materials._name(model, i) end
  return list
end

-- A world's materials, by the world's id.
function renut.worlds.set_material(world, name, values) materials.set("renutworld_" .. tostring(world), name, values) end
function renut.worlds.reset_material(world, name) materials.reset("renutworld_" .. tostring(world), name) end
function renut.worlds.materials(world) return materials.list("renutworld_" .. tostring(world)) end
)lua";

}

void Declare() {
  rexfabric::Namespace renut("renut");
  renut.Category("materials.", "Materials");  // the node editor's add menu
  for (const auto &[name, args] : std::initializer_list<std::pair<const char *, std::vector<std::string>>>{
           {"materials._define", {"model", "names"}},
           {"materials._index", {"model", "name"}},
           {"materials._set_emissive", {"model", "index", "r", "g", "b"}},
           {"materials._set", {"model", "index", "mask", "strength", "glow", "opacity"}},
           {"materials._reset", {"model", "index"}},
           {"materials._count", {"model"}},
           {"materials._name", {"model", "index"}}})
    renut.Args(name, args);
  renut.Function("materials._define", [](std::string model, std::string names) {
    renut::gpu::DefineMaterialTable(Table(model), Lines(names));
  });

  renut.Function("materials._index", [](std::string model, std::string name) {
    return int64_t(renut::gpu::MaterialIndex(Table(model), name));
  });

  renut.Function("materials._set_emissive", [](std::string model, int64_t index, double r, double g, double b) {
    renut::gpu::MaterialValues values;
    values.emissive[0] = float(r);
    values.emissive[1] = float(g);
    values.emissive[2] = float(b);
    if (index < 0 || !renut::gpu::SetMaterialOverride(Table(model), uint32_t(index), renut::gpu::kMaterialEmissive, values))
      throw std::runtime_error("renut.materials: " + model + " has no material " + std::to_string(index));
  });

  renut.Function("materials._set", [](std::string model, int64_t index, int64_t mask, double strength, double glow, double opacity) {
    renut::gpu::MaterialValues values;
    values.strength = float(strength);
    values.glow = float(glow);
    values.opacity = float(opacity);
    const uint32_t fields = uint32_t(mask) & (renut::gpu::kMaterialStrength | renut::gpu::kMaterialGlow | renut::gpu::kMaterialOpacity);
    if (index < 0 || !renut::gpu::SetMaterialOverride(Table(model), uint32_t(index), fields, values))
      throw std::runtime_error("renut.materials: " + model + " has no material " + std::to_string(index));
  });

  renut.Function("materials._reset", [](std::string model, int64_t index) {
    if (index < 0 || !renut::gpu::ResetMaterialOverride(Table(model), uint32_t(index)))
      throw std::runtime_error("renut.materials: " + model + " has no material " + std::to_string(index));
  });

  renut.Function("materials._count", [](std::string model) {
    return int64_t(renut::gpu::MaterialNames(Table(model)).size());
  });

  renut.Function("materials._name", [](std::string model, int64_t index) {
    const std::vector<std::string> names = renut::gpu::MaterialNames(Table(model));
    if (index < 0 || size_t(index) >= names.size())
      throw std::runtime_error("renut.materials: no material " + std::to_string(index));
    return names[size_t(index)];
  });

  renut.Lua(kLua);
}

}
