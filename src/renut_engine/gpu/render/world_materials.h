#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace renut::gpu {

// What Lua can change of a material of NutTool's world models while the game runs (NutTool's
// docs/renut-materials.md): its emissive colour and strength, glow and opacity.
struct MaterialValues {
  float emissive[3] = {0, 0, 0};
  float strength = 1;
  float glow = -1;  // below 0: from the emissive
  float opacity = 1;
};

// Which of MaterialValues an override sets.
enum MaterialField : uint32_t {
  kMaterialEmissive = 1,
  kMaterialStrength = 2,
  kMaterialGlow = 4,
  kMaterialOpacity = 8,
};

// A model's material table (keyed by its asset id, as its draws' records name it): their names, in
// the records' index order. Defining one again drops its overrides. Thread-safe.
void DefineMaterialTable(uint32_t table, const std::vector<std::string> &names);

// The index of `name` in `table`, or -1. A table's names, in order.
int MaterialIndex(uint32_t table, const std::string &name);
std::vector<std::string> MaterialNames(uint32_t table);

// Sets the `fields` of `values` over the material's own (false: no such table or index); Reset drops them.
bool SetMaterialOverride(uint32_t table, uint32_t index, uint32_t fields, const MaterialValues &values);
bool ResetMaterialOverride(uint32_t table, uint32_t index);

// The fields set over the material (0 for none), and their values in `values`.
uint32_t MaterialOverride(uint32_t table, uint32_t index, MaterialValues &values);

} // namespace renut::gpu
