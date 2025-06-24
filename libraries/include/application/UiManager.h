#pragma once

#include <array>
#include <functional>

class ImGuiIO;
struct ImDrawData;

namespace sparkle
{
class NativeView;

class UiManager
{
public:
    struct CustomUiWindow
    {
        std::function<void(void)> ui_generator;
    };

    explicit UiManager(NativeView *native_view);

    ~UiManager();

    void Render();

    void BeginRenderThread();

    void Shutdown();

    bool IsHandlingMouseEvent();

    bool IsHanldingKeyboradEvent();

    // request to draw a ui window that will be called in render thread.
    // you are responsible to handle thread safety.
    void RequestWindowDraw(CustomUiWindow &&window);

    static bool HasDataToDraw();

private:
    ImGuiIO *io_ = nullptr;
    NativeView *native_view_ = nullptr;

    std::vector<CustomUiWindow> pending_windows_to_draw_;

    std::array<ImDrawData *, 4> draw_data_per_frame_;

    unsigned main_thread_context_index_ = 0;
    unsigned render_thread_context_index_ = 0;
};
} // namespace sparkle
