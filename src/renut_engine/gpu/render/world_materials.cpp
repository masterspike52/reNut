#include "renut_engine/gpu/render/world_materials.h"

#include <mutex>
#include <unordered_map>

namespace renut::gpu {
namespace {

struct Override {
  uint32_t fields = 0;
  MaterialValues values;
};

struct Table {
  std::vector<std::string> names;
  std::vector<Override> overrides;
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Table> g_tables;

} // namespace

void DefineMaterialTable(uint32_t table, const std::vector<std::string> &names) {
  Table t;
  t.names = names;
  t.overrides.resize(names.size());
  std::lock_guard lock(g_mutex);
  g_tables[table] = std::move(t);
}

int MaterialIndex(uint32_t table, const std::string &name) {
  std::lock_guard lock(g_mutex);
  const auto it = g_tables.find(table);
  if (it == g_tables.end())
    return -1;
  for (size_t i = 0; i < it->second.names.size(); ++i)
    if (it->second.names[i] == name)
      return int(i);
  return -1;
}

std::vector<std::string> MaterialNames(uint32_t table) {
  std::lock_guard lock(g_mutex);
  const auto it = g_tables.find(table);
  return it != g_tables.end() ? it->second.names : std::vector<std::string>{};
}

bool SetMaterialOverride(uint32_t table, uint32_t index, uint32_t fields, const MaterialValues &values) {
  std::lock_guard lock(g_mutex);
  const auto it = g_tables.find(table);
  if (it == g_tables.end() || index >= it->second.overrides.size())
    return false;
  Override &o = it->second.overrides[index];
  if (fields & kMaterialEmissive)
    for (int k = 0; k < 3; ++k)
      o.values.emissive[k] = values.emissive[k];
  if (fields & kMaterialStrength)
    o.values.strength = values.strength;
  if (fields & kMaterialGlow)
    o.values.glow = values.glow;
  if (fields & kMaterialOpacity)
    o.values.opacity = values.opacity;
  o.fields |= fields;
  return true;
}

bool ResetMaterialOverride(uint32_t table, uint32_t index) {
  std::lock_guard lock(g_mutex);
  const auto it = g_tables.find(table);
  if (it == g_tables.end() || index >= it->second.overrides.size())
    return false;
  it->second.overrides[index] = Override{};
  return true;
}

uint32_t MaterialOverride(uint32_t table, uint32_t index, MaterialValues &values) {
  std::lock_guard lock(g_mutex);
  const auto it = g_tables.find(table);
  if (it == g_tables.end() || index >= it->second.overrides.size())
    return 0;
  const Override &o = it->second.overrides[index];
  if (o.fields & kMaterialEmissive)
    for (int k = 0; k < 3; ++k)
      values.emissive[k] = o.values.emissive[k];
  if (o.fields & kMaterialStrength)
    values.strength = o.values.strength;
  if (o.fields & kMaterialGlow)
    values.glow = o.values.glow;
  if (o.fields & kMaterialOpacity)
    values.opacity = o.values.opacity;
  return o.fields;
}

} // namespace renut::gpu
