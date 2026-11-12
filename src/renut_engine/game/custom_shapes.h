#pragma once

namespace renut::game::custom_shapes {

// Before rexfabric::Start: declares renut.shapes (shape key weights of NutTool's models) for Lua mods.
// docs/renut-api.md describes it; the tables live in gpu/render/shape_weights.h.
void Declare();

} // namespace renut::game::custom_shapes
