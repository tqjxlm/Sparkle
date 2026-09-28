#pragma once

#include "core/math/Types.h"
#include "renderer/RenderConfig.h"
#include "renderer/graph/RGPassTimers.h"
#include "renderer/graph/RGTexturePool.h"
#include "renderer/graph/RenderGraph.h"
#include "rhi/RHIImage.h"

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sparkle
{
class SceneRenderProxy;
class RHIContext;
class NativeView;
struct AppConfig;
class CameraRenderProxy;
class MaterialRenderProxy;
class MeshRenderProxy;
class ImageBasedLighting;
class ScreenQuadPass;
class UiPass;

// A renderer performs the following functionalities:
// 1. process a scene of geometries
// 2. manage render resources
// 3. organize a series of render passes
// 4. produce images
// 5. output images to a specific io device (memory, monitor, disk, etc.)
class Renderer
{
public:
    Renderer(const RenderConfig &render_config, RHIContext *rhi_context, SceneRenderProxy *scene_render_proxy,
             RGTexturePool &graph_texture_pool);

    virtual ~Renderer();

    virtual void InitRenderResources() = 0;

    virtual void Render() = 0;

    [[nodiscard]] virtual RenderConfig::Pipeline GetRenderMode() const = 0;

    void Tick();

    void OnFrameBufferResize(int width, int height);

    using ScreenshotCallback = std::function<void()>;

    void RequestSaveScreenshot(const std::string &file_path, bool capture_ui = false,
                               ScreenshotCallback on_complete = nullptr);

    // the next render graph the renderer executes is written to screenshots/<name>.json
    void RequestGraphDump(const std::string &name, std::function<void()> on_complete);

    // `on_dump` receives the dump of the next render graph the renderer executes, replacing an earlier such request
    void RequestGraphDump(std::function<void(const nlohmann::json &)> on_dump);

    void NotifySceneLoaded();

    void RegisterAsyncTask()
    {
        pending_async_tasks_.fetch_add(1);
    }

    void UnregisterAsyncTask()
    {
        auto prev = pending_async_tasks_.fetch_sub(1);
        ASSERT(prev > 0);
    }

    [[nodiscard]] bool HasPendingAsyncTasks() const
    {
        return pending_async_tasks_.load() > 0;
    }

    [[nodiscard]] virtual bool IsReadyForAutoScreenshot() const;

    static std::unique_ptr<Renderer> CreateRenderer(const RenderConfig &render_config, RHIContext *rhi_context,
                                                    SceneRenderProxy *scene_render_proxy,
                                                    RGTexturePool &graph_texture_pool);

    void SetDebugPoint(float x, float y)
    {
        debug_point_.x() = x < 0 ? UINT_MAX : static_cast<unsigned>(x);
        debug_point_.y() = y < 0 ? UINT_MAX : static_cast<unsigned>(y);
    }

    [[nodiscard]] const RenderResolution &GetResolution() const
    {
        return resolution_;
    }

    // the pass drawing the texture render_graph_view names into the Screen transient; its first access samples it
    static constexpr const char *GraphViewPassName = "GraphView";

protected:
    virtual void Update() = 0;

    // the screen of renderers that tone map on the GPU
    static constexpr RGTextureDesc ToneMappedScreenDesc{
        .format = PixelFormat::B8G8R8A8Srgb,
        .size_class = RGSizeClass::Output,
        .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                    .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                    .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest}};

    // the scene depth of the renderers that rasterize the scene
    static constexpr RGTextureDesc SceneDepthDesc{.format = PixelFormat::D32, .size_class = RGSizeClass::Scene};

    // the scene color of the renderers that rasterize the scene. tone mapping samples it bilinearly with edge clamping
    // when it upsamples.
    [[nodiscard]] RGTextureDesc GetSceneColorDesc() const;

    // the cube map the sky box shows in output mode `mode`: an IBL map once it is ready in the IBL map modes, otherwise
    // `sky_map`
    [[nodiscard]] static RHIResourceRef<RHIImage> GetSkyBoxMap(RenderConfig::OutputImage mode,
                                                               const ImageBasedLighting *ibl,
                                                               const RHIResourceRef<RHIImage> &sky_map);

    // creates the passes of the post chain, which ends in a Screen texture of `screen_desc`
    void InitPostChain(const RGTextureDesc &screen_desc);

    // adds the passes every frame ends with: `screen_pass` drawing `scene` into the Screen transient (tone mapping or
    // upsampling), or `scene` as the screen without one, unless the texture render_graph_view names is drawn there
    // instead; the screenshot readbacks; the ui when shown; and the present drawing the screen into the back buffer
    void AddPostChain(RenderGraph &graph, RGTexture scene, const ScreenQuadPass *screen_pass);

    // compiles and records the frame's graph, then dumps it when a dump is pending
    void ExecuteGraph(RenderGraph &graph);

    RHIContext *rhi_;
    SceneRenderProxy *scene_render_proxy_;

    Vector2UInt debug_point_{UINT_MAX, UINT_MAX};
    RenderResolution resolution_;

    const RenderConfig &render_config_;

    bool scene_loaded_ = false;

    std::atomic<int32_t> pending_async_tasks_{0};

    // images behind the transients of the renderer's graphs, kept across frames and renderers
    RGTexturePool &graph_texture_pool_;

    RGTextureDesc screen_desc_;
    // null when headless
    std::unique_ptr<UiPass> ui_pass_;
    std::unique_ptr<ScreenQuadPass> present_pass_;

private:
    struct PendingScreenshot
    {
        std::string file_path;
        bool capture_ui;
        ScreenshotCallback on_complete;
    };

    [[nodiscard]] std::optional<PendingScreenshot> TakeScreenshotRequest(bool capture_ui);

    // the texture render_graph_view names when the post chain can show it; otherwise invalid, with a warning once per
    // change of the value
    [[nodiscard]] RGTexture FindGraphView(const RenderGraph &graph);

    // when a screenshot with or without ui is pending, adds a pass that copies `texture` into a staging buffer, which
    // is saved once the frame completes
    void AddReadback(RenderGraph &graph, RGTexture texture, bool capture_ui);

    // a staging buffer for an image of `format` and `size`, saved as the screenshot once the frame completes. the
    // caller records the copy.
    [[nodiscard]] RHIResourceRef<RHIBuffer> CreateScreenshotBuffer(PixelFormat format, Vector2UInt size,
                                                                   PendingScreenshot screenshot);

    std::optional<PendingScreenshot> pending_screenshot_;

    // draws the texture render_graph_view names into the Screen transient
    std::unique_ptr<ScreenQuadPass> graph_view_pass_;
    // the render_graph_view the post chain last looked up, and whether its fallback was logged
    std::string graph_view_;
    bool graph_view_warned_ = false;

    // times the raster passes of the renderer's graphs across frames
    RGPassTimers graph_pass_timers_;

    std::string graph_dump_path_;
    std::function<void()> graph_dump_completion_;
    std::function<void(const nlohmann::json &)> graph_dump_consumer_;
};
} // namespace sparkle
