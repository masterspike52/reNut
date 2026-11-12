#include "renut_engine/gpu/render/shape_weights.h"

#include <cmath>
#include <mutex>
#include <unordered_map>

namespace renut::gpu {
namespace {

struct Table {
  std::vector<std::string> names;
  std::vector<float> weights;
};

// A part's own weights by table, NaN where it keeps its model's.
struct Instance {
  uint32_t check = 0, slot = 0;
  std::unordered_map<uint32_t, std::vector<float>> weights;
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Table> g_tables;
std::unordered_map<uint32_t, Instance> g_instances;   // by the part's address
std::unordered_map<uint32_t, uint32_t> g_slots;       // slot -> part address
uint32_t g_next_slot = 1;

// The part's entry, gone when another part (another check) now has its address. Under g_mutex.
Instance *FindInstance(uint32_t key, uint32_t check) {
  const auto it = g_instances.find(key);
  if (it == g_instances.end())
    return nullptr;
  if (it->second.check != check) {
    g_slots.erase(it->second.slot);
    g_instances.erase(it);
    return nullptr;
  }
  return &it->second;
}

} // namespace

bool SetInstanceShapeWeight(uint32_t key, uint32_t check, uint32_t table, uint32_t index, float weight) {
  std::lock_guard lock(g_mutex);
  const auto t = g_tables.find(table);
  if (!key || t == g_tables.end() || index >= t->second.weights.size())
    return false;
  Instance *in = FindInstance(key, check);
  if (!in) {
    in = &g_instances[key];
    in->check = check;
    in->slot = g_next_slot++;
    if (g_next_slot >= (1u << 24))  // c28.w is a float: whole numbers stay exact below 2^24
      g_next_slot = 1;
    g_slots[in->slot] = key;
  }
  std::vector<float> &w = in->weights[table];
  w.resize(t->second.weights.size(), std::nanf(""));
  w[index] = weight;
  return true;
}

float InstanceShapeWeight(uint32_t key, uint32_t check, uint32_t table, uint32_t index) {
  std::lock_guard lock(g_mutex);
  const auto t = g_tables.find(table);
  if (t == g_tables.end() || index >= t->second.weights.size())
    return 0.0f;
  if (const Instance *in = FindInstance(key, check)) {
    const auto w = in->weights.find(table);
    if (w != in->weights.end() && index < w->second.size() && !std::isnan(w->second[index]))
      return w->second[index];
  }
  return t->second.weights[index];
}

uint32_t InstanceShapeSlot(uint32_t key, uint32_t check) {
  std::lock_guard lock(g_mutex);
  const Instance *in = FindInstance(key, check);
  return in ? in->slot : 0;
}

void ReleaseShapeInstance(uint32_t key) {
  std::lock_guard lock(g_mutex);
  const auto it = g_instances.find(key);
  if (it == g_instances.end())
    return;
  g_slots.erase(it->second.slot);
  g_instances.erase(it);
}

void DefineShapeTable(uint32_t table, const std::vector<std::string> &names, const std::vector<float> &weights) {
  Table t{names, weights};
  t.weights.resize(t.names.size(), 0.0f);
  std::lock_guard lock(g_mutex);
  g_tables[table] = std::move(t);
}

int ShapeIndex(uint32_t table, const std::string &name) {
  std::lock_guard lock(g_mutex);
  const auto it = g_tables.find(table);
  if (it == g_tables.end())
    return -1;
  for (size_t i = 0; i < it->second.names.size(); ++i)
    if (it->second.names[i] == name)
      return int(i);
  return -1;
}

bool SetShapeWeight(uint32_t table, uint32_t index, float weight) {
  std::lock_guard lock(g_mutex);
  const auto it = g_tables.find(table);
  if (it == g_tables.end() || index >= it->second.weights.size())
    return false;
  it->second.weights[index] = weight;
  return true;
}

float ShapeWeight(uint32_t table, uint32_t index) {
  std::lock_guard lock(g_mutex);
  const auto it = g_tables.find(table);
  return it != g_tables.end() && index < it->second.weights.size() ? it->second.weights[index] : 0.0f;
}

std::vector<std::string> ShapeNames(uint32_t table) {
  std::lock_guard lock(g_mutex);
  const auto it = g_tables.find(table);
  return it != g_tables.end() ? it->second.names : std::vector<std::string>{};
}

void ShapeWeights(uint32_t table, const uint32_t *indices, uint32_t count, float *out, uint32_t slot) {
  std::lock_guard lock(g_mutex);
  const auto it = g_tables.find(table);
  for (uint32_t i = 0; i < count; ++i)
    out[i] = it != g_tables.end() && indices[i] < it->second.weights.size() ? it->second.weights[indices[i]] : 0.0f;
  // The part's own weights, where it has set them.
  const auto key = slot ? g_slots.find(slot) : g_slots.end();
  if (key == g_slots.end())
    return;
  const auto in = g_instances.find(key->second);
  if (in == g_instances.end())
    return;
  const auto own = in->second.weights.find(table);
  if (own == in->second.weights.end())
    return;
  for (uint32_t i = 0; i < count; ++i)
    if (indices[i] < own->second.size() && !std::isnan(own->second[indices[i]]))
      out[i] = own->second[indices[i]];
}

} // namespace renut::gpu
