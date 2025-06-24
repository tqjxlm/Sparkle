#if FRAMEWORK_APPLE

#include "apple/AppleNativeView.h"

#if ENABLE_VULKAN
#include "rhi/VulkanRHI.h"
#endif

#include "application/AppFramework.h"

#if FRAMEWORK_MACOS
#import <imgui_impl_osx.h>
#endif
#if FRAMEWORK_IOS
#import "../ios/imgui_impl_ios.h"
#endif

namespace sparkle
{
void AppleNativeView::InitGUI(AppFramework *app)
{
    app_ = app;
}

void AppleNativeView::Cleanup()
{
}

bool AppleNativeView::ShouldClose()
{
    return false;
}

void AppleNativeView::Tick()
{
}

void AppleNativeView::InitUiSystem()
{
#if FRAMEWORK_MACOS
    ImGui_ImplOSX_Init(view_);
#elif FRAMEWORK_IOS
    ImGui_ImplIOS_Init(view_);
#else
#error
#endif
}

void AppleNativeView::ShutdownUiSystem()
{
#if FRAMEWORK_MACOS
    ImGui_ImplOSX_Shutdown();
#elif FRAMEWORK_IOS
    ImGui_ImplIOS_Shutdown();
#else
#error
#endif
}

void AppleNativeView::TickUiSystem()
{
#if FRAMEWORK_MACOS
    ImGui_ImplOSX_NewFrame(view_);
#elif FRAMEWORK_IOS
    ImGui_ImplIOS_NewFrame(view_);
#else
#error
#endif

    ImGuiIO &io = ImGui::GetIO();
    io.DisplayFramebufferScale = ImVec2(1, 1);
}

void AppleNativeView::SetTitle([[maybe_unused]] const char *title)
{
#if FRAMEWORK_MACOS
    auto new_title = [NSString stringWithUTF8String:title];

    dispatch_async(dispatch_get_main_queue(), ^{
      [(NSView *)view_ window].title = new_title;
    });
#endif
}

#if ENABLE_VULKAN
bool AppleNativeView::CreateVulkanSurface(void *in_instance, void *out_surface)
{
#if FRAMEWORK_MACOS
    VkMacOSSurfaceCreateInfoMVK surface_create_info = {};
#endif
#if FRAMEWORK_IOS
    VkIOSSurfaceCreateInfoMVK surface_create_info = {};
#endif
    surface_create_info.sType = VK_STRUCTURE_TYPE_MACOS_SURFACE_CREATE_INFO_MVK;
    surface_create_info.pNext = nullptr;
    surface_create_info.flags = 0;
    surface_create_info.pView = view_;

#if FRAMEWORK_MACOS
    auto success = vkCreateMacOSSurfaceMVK(static_cast<VkInstance>(in_instance), &surface_create_info, nullptr,
                                           static_cast<VkSurfaceKHR *>(out_surface)) == VK_SUCCESS;
#endif
#if FRAMEWORK_IOS
    auto success = vkCreateIOSSurfaceMVK(static_cast<VkInstance>(in_instance), &surface_create_info, nullptr,
                                         static_cast<VkSurfaceKHR *>(out_surface)) == VK_SUCCESS;
#endif

    return success;
}

void AppleNativeView::GetVulkanRequiredExtensions(std::vector<const char *> &required_extensions)
{
#if FRAMEWORK_MACOS
    required_extensions.push_back(VK_MVK_MACOS_SURFACE_EXTENSION_NAME);
#endif
#if FRAMEWORK_IOS
    required_extensions.push_back(VK_MVK_IOS_SURFACE_EXTENSION_NAME);
#endif
    required_extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#if VK_KHR_portability_enumeration
    required_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif
}
#endif

void AppleNativeView::GetFrameBufferSize(int &width, int &height)
{
    auto rect = [view_ drawableSize];

    width = rect.width;
    height = rect.height;
}

void AppleNativeView::SetMetalView(MetalView *view)
{
    view_ = view;

    can_render_ = true;

#if FRAMEWORK_MACOS
    auto scale = [view_.window backingScaleFactor];
#endif
#if FRAMEWORK_IOS
    auto scale = [view_.window contentScaleFactor];
#endif
    window_scale_.x() = scale;
    window_scale_.y() = scale;
}
} // namespace sparkle

#endif
