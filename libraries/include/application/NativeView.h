#pragma once

#include "core/math/Types.h"

namespace sparkle
{
class AppFramework;
class FileManager;

class NativeView
{
public:
    enum class WindowRotation : uint8_t
    {
        Portrait,
        Landscape,
        ReversePortrait,
        ReverseLandscape
    };

    NativeView();

    virtual ~NativeView();

    [[nodiscard]] bool CanRender() const
    {
        return can_render_;
    }

    [[nodiscard]] bool IsValid() const
    {
        return is_valid_;
    }

    // setup platform-specific environment, e.g. callbacks
    virtual void InitGUI(AppFramework *app) = 0;

    virtual void Cleanup() = 0;

    virtual bool ShouldClose() = 0;

    virtual void Tick() = 0;

    virtual void SetTitle(const char *title) = 0;

    virtual void InitUiSystem() = 0;
    virtual void ShutdownUiSystem() = 0;
    virtual void TickUiSystem() = 0;

#if ENABLE_VULKAN
    virtual bool CreateVulkanSurface(void *in_instance, void *out_surface);

    virtual void GetVulkanRequiredExtensions(std::vector<const char *> &required_extensions);
#endif

    virtual void GetFrameBufferSize(int &width, int &height) = 0;

    void SetWindowRotation(WindowRotation rotation)
    {
        window_rotation_ = rotation;
    }

    void SetGuiScale(const Vector2 &scale)
    {
        gui_scale_ = scale;
    }

    [[nodiscard]] WindowRotation GetWindowOrientation() const
    {
        return window_rotation_;
    }

    [[nodiscard]] Vector2 GetWindowScale() const
    {
        return window_scale_;
    }

    static Mat2 GetRotationMatrix(WindowRotation rotation);

protected:
    Vector2 window_scale_{1.f, 1.f};
    Vector2 gui_scale_{1.f, 1.f};
    bool can_render_ = false;
    bool is_valid_ = false;

    WindowRotation window_rotation_ = WindowRotation::Portrait;

    AppFramework *app_ = nullptr;

    // native file manager for platform-specific file operations
    std::unique_ptr<FileManager> file_manager_;
};
} // namespace sparkle
