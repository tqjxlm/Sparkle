#pragma once

#include <functional>
#include <memory>

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
    bool IsPointerOverUi(float x, float y);

    bool IsHandlingKeyboardEvent();

    // request to draw a ui window that will be called in render thread.
    // you are responsible to handle thread safety.
    void RequestWindowDraw(CustomUiWindow &&window);

    static bool HasDataToDraw();

private:
    ImGuiIO *io_ = nullptr;
    NativeView *native_view_ = nullptr;

    std::vector<CustomUiWindow> pending_windows_to_draw_;

    // render thread only; the main thread hands it over through the frame task queue
    std::shared_ptr<ImDrawData> render_thread_draw_data_;
};
} // namespace sparkle
