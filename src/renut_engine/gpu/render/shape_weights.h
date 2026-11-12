#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace renut::gpu {

// Shape key tables (docs/nuttool/custom-models.md): a model's shape names and their weights, keyed by the
// model's asset id. Lua (game/custom_shapes.cpp) defines and sets them; draws read them. Thread-safe.
void DefineShapeTable(uint32_t table, const std::vector<std::string> &names, const std::vector<float> &weights);

// The index of `name` in `table`, or -1 (no such table or shape).
int ShapeIndex(uint32_t table, const std::string &name);

// Sets or reads a shape's weight; false / 0 for a table or index that isn't there.
bool SetShapeWeight(uint32_t table, uint32_t index, float weight);
float ShapeWeight(uint32_t table, uint32_t index);

// The table's shape names, in order (empty for none).
std::vector<std::string> ShapeNames(uint32_t table);

// One part's own weights, over its model's: keyed by the part's guest address, `check` (its model
// instance) telling a new part at the same address. Its slot (1 up) reaches the draw as pixel c28.w.
bool SetInstanceShapeWeight(uint32_t key, uint32_t check, uint32_t table, uint32_t index, float weight);
float InstanceShapeWeight(uint32_t key, uint32_t check, uint32_t table, uint32_t index);

// Game thread, each frame a part is drawn: its slot (0 for none), forgetting one left by another part.
uint32_t InstanceShapeSlot(uint32_t key, uint32_t check);

// A part's own weights go (it was destroyed). A new part at its address also drops them (`check`).
void ReleaseShapeInstance(uint32_t key);

// Draw thread: the weights of `count` of `table`'s shapes by index (0 for unknown ones), part `slot`'s
// own where it has them.
void ShapeWeights(uint32_t table, const uint32_t *indices, uint32_t count, float *out, uint32_t slot = 0);

} // namespace renut::gpu
