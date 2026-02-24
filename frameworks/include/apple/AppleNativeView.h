#pragma once

#if FRAMEWORK_APPLE

#include "application/NativeView.h"

#include "apple/MetalView.h"

#import <MetalKit/MetalKit.h>

namespace sparkle
{
class AppleNativeView : public NativeView
{
public:
    [[nodiscard]] bool IsHeadless() const override
    {
        return headless_;
    }

    void InitGUI(AppFramework *app) override;
    void Cleanup() override;
    bool ShouldClose() override;
    void Tick() override;
    void SetTitle(const char *title) override;
#if ENABLE_VULKAN
    bool CreateVulkanSurface(void *in_instance, void *out_surface) override;
    void GetVulkanRequiredExtensions(std::vector<const char *> &required_extensions) override;
#endif
    void GetFrameBufferSize(int &width, int &height) override;
    void InitUiSystem() override;
    void ShutdownUiSystem() override;
    void TickUiSystem() override;

    void SetMetalView(MetalView *view);

    MetalView *GetMetalView()
    {
        return view_;
    }

private:
    bool headless_ = false;
    int headless_width_ = 0;
    int headless_height_ = 0;
    MetalView *view_ = nullptr;
};
} // namespace sparkle
#endif
