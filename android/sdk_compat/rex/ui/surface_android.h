#pragma once
// Xenia's Android surface, which the SDK's Vulkan presenter
// (src/ui/vulkan/vulkan_presenter.cpp) includes but the SDK does not ship.
// Nothing makes one (the SDL window has no Android surface; reNut presents
// through plume), so GetSizeImpl has no definition yet.

#include <android/native_window.h>

#include <rex/ui/surface.h>

namespace rex::ui {

class AndroidNativeWindowSurface final : public Surface {
public:
    explicit AndroidNativeWindowSurface(ANativeWindow *window) : window_(window) {}
    TypeIndex GetType() const override { return kTypeIndex_AndroidNativeWindow; }
    ANativeWindow *window() const { return window_; }

protected:
    bool GetSizeImpl(uint32_t &width_out, uint32_t &height_out) const override;

private:
    ANativeWindow *window_;
};

} // namespace rex::ui
