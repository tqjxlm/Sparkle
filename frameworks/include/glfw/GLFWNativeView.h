#pragma once

#if FRAMEWORK_GLFW

#include "application/NativeView.h"

struct GLFWwindow;

namespace sparkle
{
class GLFWNativeView final : public NativeView
{
public:
    GLFWNativeView();

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

private:
    static void FrameBufferResizeCallback(GLFWwindow *window, int width, int height);

    static void CursorPositionCallback(GLFWwindow *window, double xPos, double yPos);

    static void KeyboardCallback(GLFWwindow *window, int key, int scancode, int action, int mods);

    static void MouseButtonCallback(GLFWwindow *window, int button, int action, int mods);

    static void ScrollCallback(GLFWwindow *window, double xoffset, double yoffset);

    GLFWwindow *view_;
};
} // namespace sparkle
#endif
