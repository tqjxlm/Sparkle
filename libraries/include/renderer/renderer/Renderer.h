#pragma once

#include "core/math/Types.h"
#include "renderer/RenderConfig.h"
#include "rhi/RHIImage.h"

#include <functional>
#include <string>

namespace sparkle
{
class SceneRenderProxy;
class RHIContext;
class NativeView;
class RHIRenderTarget;
struct AppConfig;
class CameraRenderProxy;
class MaterialRenderProxy;
class MeshRenderProxy;

// A renderer performs the following functionalities:
// 1. process a scene of geometries
// 2. manage render resources
// 3. organize a series of render passes
// 4. produce images
// 5. output images to a specific io device (memory, monitor, disk, etc.)
class Renderer
{
public:
    Renderer(const RenderConfig &render_config, RHIContext *rhi_context, SceneRenderProxy *scene_render_proxy);

    virtual ~Renderer() = default;

    virtual void InitRenderResources() = 0;

    virtual void Render() = 0;

    [[nodiscard]] virtual RenderConfig::Pipeline GetRenderMode() const = 0;

    void Tick();

    void OnFrameBufferResize(int width, int height);

    using ScreenshotCallback = std::function<void()>;

    void RequestSaveScreenshot(const std::string &file_path, bool capture_ui = false,
                               ScreenshotCallback on_complete = nullptr);

    static std::unique_ptr<Renderer> CreateRenderer(const RenderConfig &render_config, RHIContext *rhi_context,
                                                    SceneRenderProxy *scene_render_proxy);

    void SetDebugPoint(float x, float y)
    {
        debug_point_.x() = x < 0 ? UINT_MAX : static_cast<unsigned>(x);
        debug_point_.y() = y < 0 ? UINT_MAX : static_cast<unsigned>(y);
    }

protected:
    virtual void Update() = 0;

    // return true if readback is performed
    [[nodiscard]] bool ReadbackFinalOutputIfRequested(RHIRenderTarget *final_output, bool capture_ui,
                                                      RHIPipelineStage after_stage);

    RHIContext *rhi_;
    SceneRenderProxy *scene_render_proxy_;

    Vector2UInt debug_point_{UINT_MAX, UINT_MAX};
    Vector2UInt image_size_;

    const RenderConfig &render_config_;

private:
    std::string screenshot_file_path_;
    bool screenshot_requested_ = false;
    bool screenshot_capture_ui_ = false;
    ScreenshotCallback screenshot_completion_;
};
} // namespace sparkle
