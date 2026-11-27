// The game's viewport renderer (rexfabric/content.h, SetViewportRenderer), as the Avatar editor's viewports reach it.

#pragma once

#include <cstdint>
#include <optional>

#include <rexfabric/content.h>

namespace rexfabric::content {

// A number for a new viewport, never 0 and never given twice.
uint64_t NewViewport();

// What the renderer drew last for `viewport`, asked as `view` says; none when no renderer is registered. UI thread.
std::optional<ViewportImage> ShowViewport(uint64_t viewport, const ViewportView& view);

// The viewport is gone: the renderer may let go of what it made for it.
void CloseViewport(uint64_t viewport);

}  // namespace rexfabric::content
