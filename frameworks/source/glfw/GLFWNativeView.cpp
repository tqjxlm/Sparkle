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
static uint32_t GetKeyboardModifiers(int mods)
{
    uint32_t modifiers = 0;
    if (mods & GLFW_MOD_CONTROL)
    {
        modifiers |= static_cast<uint32_t>(KeyboardModifier::Control);
    }
    if (mods & GLFW_MOD_SHIFT)
    {
        modifiers |= static_cast<uint32_t>(KeyboardModifier::Shift);
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

    // pointer input reaches imgui through InputManager. the backend is initialized without
    // callbacks: key/char/focus/enter are chained to it below, and it keeps handling cursor
    // shapes and clipboard.
    ImGui_ImplGlfw_InitForVulkan(view_, false);
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
    glfwSetCharCallback(view_, CharCallback);
    glfwSetMouseButtonCallback(view_, MouseButtonCallback);
    glfwSetScrollCallback(view_, ScrollCallback);
    glfwSetCursorPosCallback(view_, CursorPositionCallback);
    glfwSetWindowFocusCallback(view_, WindowFocusCallback);
    glfwSetCursorEnterCallback(view_, CursorEnterCallback);
    if (!app->GetRHIConfig().use_vsync)
    {
        glfwSwapInterval(0);
    }
}

void GLFWNativeView::MouseButtonCallback(GLFWwindow *window, int button, int action, int mods)
{
    ClickButton click_button;
    switch (button)
    {
    case GLFW_MOUSE_BUTTON_LEFT:
        click_button = ClickButton::PrimaryLeft;
        break;
    case GLFW_MOUSE_BUTTON_RIGHT:
        click_button = ClickButton::SecondaryRight;
        break;
    default:
        return;
    }

    double x_pos;
    double y_pos;
    glfwGetCursorPos(window, &x_pos, &y_pos);

    auto *app = reinterpret_cast<AppFramework *>(glfwGetWindowUserPointer(window));
    app->PushInputEvent(PointerEvent{.action = action == GLFW_PRESS ? PointerAction::Down : PointerAction::Up,
                                     .button = click_button,
                                     .modifiers = GetKeyboardModifiers(mods),
                                     .position = Vector2(static_cast<float>(x_pos), static_cast<float>(y_pos))});
}

void GLFWNativeView::ScrollCallback(GLFWwindow *window, double xoffset, double yoffset)
{
    auto *app = reinterpret_cast<AppFramework *>(glfwGetWindowUserPointer(window));
    app->PushInputEvent(ScrollEvent{.delta = Vector2(static_cast<float>(xoffset), static_cast<float>(yoffset))});
}

void GLFWNativeView::FrameBufferResizeCallback(GLFWwindow *window, int width, int height)
{
    auto *app = reinterpret_cast<AppFramework *>(glfwGetWindowUserPointer(window));
    app->FrameBufferResizeCallback(width, height);
}

void GLFWNativeView::CursorPositionCallback(GLFWwindow *window, double xPos, double yPos)
{
    auto *app = reinterpret_cast<AppFramework *>(glfwGetWindowUserPointer(window));
    app->PushInputEvent(PointerEvent{.action = PointerAction::Move,
                                     .position = Vector2(static_cast<float>(xPos), static_cast<float>(yPos))});
}

void GLFWNativeView::KeyboardCallback(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    // keys go to imgui through its own backend to get the full key table, including text navigation
    ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);

    if (action == GLFW_REPEAT)
    {
        return;
    }

    auto *app = reinterpret_cast<AppFramework *>(glfwGetWindowUserPointer(window));
    app->PushInputEvent(KeyEvent{.key = key,
                                 .action = action == GLFW_PRESS ? KeyAction::Press : KeyAction::Release,
                                 .modifiers = GetKeyboardModifiers(mods)});
}

void GLFWNativeView::CharCallback(GLFWwindow *window, unsigned int codepoint)
{
    ImGui_ImplGlfw_CharCallback(window, codepoint);
}

void GLFWNativeView::WindowFocusCallback(GLFWwindow *window, int focused)
{
    ImGui_ImplGlfw_WindowFocusCallback(window, focused);
}

void GLFWNativeView::CursorEnterCallback(GLFWwindow *window, int entered)
{
    ImGui_ImplGlfw_CursorEnterCallback(window, entered);
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
