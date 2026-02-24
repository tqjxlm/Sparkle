#if FRAMEWORK_GLFW

#include "glfw/GLFWNativeView.h"

#include "application/AppFramework.h"
#include "core/Exception.h"
#include "core/Logger.h"

#if ENABLE_VULKAN
#include <vulkan/vulkan_core.h>
#endif

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <imgui_impl_glfw.h>

namespace sparkle
{
static AppFramework::ClickButton GetClickButton(int button)
{
    switch (button)
    {
    case GLFW_MOUSE_BUTTON_LEFT:
        return AppFramework::ClickButton::Primary_Left;
    case GLFW_MOUSE_BUTTON_RIGHT:
        return AppFramework::ClickButton::Secondary_Right;
    default:
        UnImplemented(button);
    }
    return AppFramework::ClickButton::Count;
}

static AppFramework::KeyAction GetKeyAction(int action)
{
    switch (action)
    {
    case GLFW_PRESS:
        return AppFramework::KeyAction::Press;
    case GLFW_RELEASE:
        return AppFramework::KeyAction::Release;
    default:
        UnImplemented(action);
    }
    return AppFramework::KeyAction::Count;
}

static AppFramework::KeyboardModifier GetKeyboardModifier(uint32_t mod)
{
    switch (mod)
    {
    case GLFW_MOD_CONTROL:
        return AppFramework::KeyboardModifier::Control;
    case GLFW_RELEASE:
        return AppFramework::KeyboardModifier::Shift;
    default:
        UnImplemented(mod);
    }
    return AppFramework::KeyboardModifier::Count;
}

static uint32_t GetKeyboardModifiers(int mods)
{
    uint32_t modifiers = 0;
    for (auto i = 0; i < 32; i++)
    {
        uint32_t mod = 1u << i;
        if (static_cast<uint32_t>(mods) & mod)
        {
            modifiers |= static_cast<uint32_t>(GetKeyboardModifier(mod));
        }
    }

    return modifiers;
}

void GLFWNativeView::GetFrameBufferSize(int &width, int &height)
{
    if (headless_)
    {
        const auto &render_config = app_->GetRenderConfig();
        width = static_cast<int>(render_config.image_width);
        height = static_cast<int>(render_config.image_height);
        return;
    }

    glfwGetFramebufferSize(view_, &width, &height);
    while (width == 0 || height == 0)
    {
        glfwGetFramebufferSize(view_, &width, &height);
        glfwWaitEvents();
    }
}

void GLFWNativeView::InitUiSystem()
{
    if (headless_)
    {
        return;
    }

    ImGui_ImplGlfw_InitForVulkan(view_, true);
}

void GLFWNativeView::ShutdownUiSystem()
{
    if (headless_)
    {
        return;
    }

    ImGui_ImplGlfw_Shutdown();
}

void GLFWNativeView::TickUiSystem()
{
    if (headless_)
    {
        return;
    }

    ImGui_ImplGlfw_NewFrame();

    ImGuiIO &io = ImGui::GetIO();
    io.DisplayFramebufferScale = ImVec2(1, 1);
}

#if ENABLE_VULKAN
bool GLFWNativeView::CreateVulkanSurface(void *in_instance, void *out_surface)
{
    if (headless_)
    {
        Log(Error, "CreateVulkanSurface should not be called in headless mode");
        return false;
    }

    auto result = glfwCreateWindowSurface(static_cast<VkInstance>(in_instance), view_, nullptr,
                                          static_cast<VkSurfaceKHR *>(out_surface));

    if (result != VK_SUCCESS)
    {
        ASSERT_EQUAL(result, VK_SUCCESS);
        return false;
    }

    return true;
}

void GLFWNativeView::GetVulkanRequiredExtensions(std::vector<const char *> &required_extensions)
{
    if (headless_)
    {
        return;
    }

    uint32_t extension_count;
    auto *extensions = glfwGetRequiredInstanceExtensions(&extension_count);
    for (auto i = 0u; i < extension_count; i++)
    {
        required_extensions.push_back(extensions[i]);
    }

#ifdef __APPLE__
#if VK_KHR_portability_enumeration
    required_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    required_extensions.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
#endif
#endif
}
#endif

void GLFWNativeView::InitGUI(AppFramework *app)
{
    app_ = app;

    const auto &config = app->GetAppConfig();
    const auto &render_config = app->GetRenderConfig();

    headless_ = config.headless;
    if (headless_)
    {
        Log(Info, "Headless mode enabled: skip GLFW window and input initialization.");
        window_scale_ = Vector2::Ones();
        is_valid_ = true;
        return;
    }

    glfwInit();

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    auto *monitor = glfwGetPrimaryMonitor();

    if (config.platform == AppConfig::NativePlatform::MacOS)
    {
        glfwGetMonitorContentScale(monitor, &window_scale_.x(), &window_scale_.y());
    }

    Log(Info, "content scale: {}, {}", window_scale_.x(), window_scale_.y());

    view_ = glfwCreateWindow(static_cast<int>(render_config.image_width), static_cast<int>(render_config.image_height),
                             config.app_name, nullptr, nullptr);

    glfwSetWindowUserPointer(view_, app);
    glfwSetFramebufferSizeCallback(view_, FrameBufferResizeCallback);
    glfwSetKeyCallback(view_, KeyboardCallback);
    glfwSetMouseButtonCallback(view_, MouseButtonCallback);
    glfwSetScrollCallback(view_, ScrollCallback);
    glfwSetCursorPosCallback(view_, CursorPositionCallback);
    if (!app->GetRHIConfig().use_vsync)
    {
        glfwSwapInterval(0);
    }
}

void GLFWNativeView::MouseButtonCallback(GLFWwindow *window, int button, int action, int mods)
{
    auto *app = reinterpret_cast<AppFramework *>(glfwGetWindowUserPointer(window));
    app->MouseButtonCallback(GetClickButton(button), GetKeyAction(action), GetKeyboardModifiers(mods));
}

void GLFWNativeView::ScrollCallback(GLFWwindow *window, double xoffset, double yoffset)
{
    auto *app = reinterpret_cast<AppFramework *>(glfwGetWindowUserPointer(window));
    app->ScrollCallback(xoffset, yoffset);
}

void GLFWNativeView::FrameBufferResizeCallback(GLFWwindow *window, int width, int height)
{
    auto *app = reinterpret_cast<AppFramework *>(glfwGetWindowUserPointer(window));
    app->FrameBufferResizeCallback(width, height);
}

void GLFWNativeView::CursorPositionCallback(GLFWwindow *window, double xPos, double yPos)
{
    auto *app = reinterpret_cast<AppFramework *>(glfwGetWindowUserPointer(window));
    app->CursorPositionCallback(xPos, yPos);
}

void GLFWNativeView::KeyboardCallback(GLFWwindow *window, int key, int /*scancode*/, int action, int mods)
{
    auto *app = reinterpret_cast<AppFramework *>(glfwGetWindowUserPointer(window));
    bool is_shift = mods == GLFW_MOD_SHIFT;

    app->KeyboardCallback(key, GetKeyAction(action), is_shift);
}

void GLFWNativeView::Cleanup()
{
    if (headless_)
    {
        return;
    }

    glfwDestroyWindow(view_);
    glfwTerminate();
}

bool GLFWNativeView::ShouldClose()
{
    if (headless_)
    {
        return false;
    }

    return glfwWindowShouldClose(view_) != 0;
}

void GLFWNativeView::Tick()
{
    if (headless_)
    {
        return;
    }

    glfwPollEvents();
}

void GLFWNativeView::SetTitle(const char *title)
{
    if (headless_)
    {
        return;
    }

    glfwSetWindowTitle(view_, title);
}

GLFWNativeView::GLFWNativeView()
{
    can_render_ = true;
}
} // namespace sparkle

#endif
