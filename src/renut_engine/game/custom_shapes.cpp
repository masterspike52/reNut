#include "renut_engine/game/custom_shapes.h"

#include <rexfabric/game_api.h>

#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include "renut_engine/gpu/render/shape_weights.h"
#include "renut_engine/mods/custom_parts.h"

namespace renut::game::custom_shapes {
namespace {

constexpr uint8_t kTypeModel = 0x04;

uint32_t Table(const std::string &model) {
  return renut::mods::AssetId(kTypeModel, model);
}

std::vector<std::string> Split(const std::string &text, char by) {
  std::vector<std::string> out;
  if (text.empty())
    return out;
  size_t at = 0;
  for (;;) {
    const size_t end = text.find(by, at);
    out.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
    if (end == std::string::npos)
      return out;
    at = end + 1;
  }
}

constexpr const char kLua[] = R"lua(
local shapes = renut.shapes

local function model_name(model, what)
  assert(type(model) == "string" and #model > 0, what .. ": the model is its asset name (renutworld_<id>, ...)")
  return (model:gsub("^aid_model_", ""))
end

-- renut.shapes.define{model, shapes = {names}, weights = {numbers}}: a model's shape keys (NutTool writes it).
function shapes.define(def)
  assert(type(def) == "table", "renut.shapes.define takes a table")
  local model = model_name(def.model, "renut.shapes.define")
  local names, weights = {}, {}
  for i, name in ipairs(def.shapes or {}) do
    assert(type(name) == "string" and #name > 0 and not name:find("\n"), "renut.shapes.define: shapes are names")
    local w = def.weights and def.weights[i] or 0
    assert(type(w) == "number", "renut.shapes.define: weights are numbers")
    names[i], weights[i] = name, string.format("%.9g", w)
  end
  shapes._define(model, table.concat(names, "\n"), table.concat(weights, ","))
end

-- renut.shapes.set(model, shape, weight) / get(model, shape) / list(model).
function shapes.set(model, shape, weight)
  model = model_name(model, "renut.shapes.set")
  assert(type(weight) == "number", "renut.shapes.set: the weight is a number (0 none, 1 all of it)")
  if not shapes._set(model, tostring(shape), weight) then
    error("renut.shapes.set: " .. model .. " has no shape '" .. tostring(shape) .. "'", 2)
  end
end

function shapes.get(model, shape)
  model = model_name(model, "renut.shapes.get")
  if shapes._index(model, tostring(shape)) < 0 then return nil end
  return shapes._get(model, tostring(shape))
end

function shapes.list(model)
  model = model_name(model, "renut.shapes.list")
  local list = {}
  for i = 0, shapes._count(model) - 1 do list[#list + 1] = shapes._name(model, i) end
  return list
end

-- A world's shapes, by the world's id.
function renut.worlds.set_shape(world, shape, weight) shapes.set("renutworld_" .. tostring(world), shape, weight) end
function renut.worlds.shape(world, shape) return shapes.get("renutworld_" .. tostring(world), shape) end
)lua";

} // namespace

void Declare() {
  rexfabric::Namespace renut("renut");
  renut.Category("shapes.", "Shape Keys");  // the node editor's add menu
  for (const auto &[name, args] : std::initializer_list<std::pair<const char *, std::vector<std::string>>>{
           {"shapes._define", {"model", "names", "weights"}},
           {"shapes._index", {"model", "shape"}},
           {"shapes._set", {"model", "shape", "weight"}},
           {"shapes._get", {"model", "shape"}},
           {"shapes._count", {"model"}},
           {"shapes._name", {"model", "index"}}})
    renut.Args(name, args);  // the node editor's pins
  renut.Function("shapes._define", [](std::string model, std::string names, std::string weights) {
    std::vector<float> values;
    for (const std::string &w : Split(weights, ','))
      values.push_back(float(std::strtod(w.c_str(), nullptr)));
    renut::gpu::DefineShapeTable(Table(model), Split(names, '\n'), values);
  });
  renut.Function("shapes._index", [](std::string model, std::string shape) {
    return int64_t(renut::gpu::ShapeIndex(Table(model), shape));
  });
  renut.Function("shapes._set", [](std::string model, std::string shape, double weight) {
    const int index = renut::gpu::ShapeIndex(Table(model), shape);
    if (index < 0 || !std::isfinite(weight))
      return false;
    return renut::gpu::SetShapeWeight(Table(model), uint32_t(index), float(weight));
  });
  renut.Function("shapes._get", [](std::string model, std::string shape) {
    const int index = renut::gpu::ShapeIndex(Table(model), shape);
    return index < 0 ? 0.0 : double(renut::gpu::ShapeWeight(Table(model), uint32_t(index)));
  });
  renut.Function("shapes._count", [](std::string model) {
    return int64_t(renut::gpu::ShapeNames(Table(model)).size());
  });
  renut.Function("shapes._name", [](std::string model, int64_t index) {
    const std::vector<std::string> names = renut::gpu::ShapeNames(Table(model));
    if (index < 0 || size_t(index) >= names.size())
      throw std::runtime_error("renut.shapes: no shape " + std::to_string(index));
    return names[size_t(index)];
  });
  renut.Lua(kLua);
}

} // namespace renut::game::custom_shapes
