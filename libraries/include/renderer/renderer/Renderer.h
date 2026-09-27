#pragma once

#include "core/math/Types.h"
#include "renderer/RenderConfig.h"
#include "renderer/graph/RGTexturePool.h"
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
class RenderGraph;
class DepthPass;
class ImageBasedLighting;
class ScreenQuadPass;
class SkyBoxPass;
class UiPass;
struct RGTexture;

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

    // the next render graph the renderer executes is written to screenshots/<name>.json
    void RequestGraphDump(const std::string &name, std::function<void()> on_complete);

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
                                                    SceneRenderProxy *scene_render_proxy);

    void SetDebugPoint(float x, float y)
    {
        debug_point_.x() = x < 0 ? UINT_MAX : static_cast<unsigned>(x);
        debug_point_.y() = y < 0 ? UINT_MAX : static_cast<unsigned>(y);
    }

    [[nodiscard]] const RenderResolution &GetResolution() const
    {
        return resolution_;
    }

protected:
    virtual void Update() = 0;

    // when a screenshot with or without ui is pending, adds a pass that copies `texture` into a staging buffer, which
    // is saved once the frame completes
    void AddReadback(RenderGraph &graph, RGTexture texture, bool capture_ui);

    // adds the legacy `shadow_pass` as an External pass clearing and writing its shadow map, and returns the shadow map
    [[nodiscard]] static RGTexture AddDirectionalShadowPass(RenderGraph &graph, DepthPass &shadow_pass);

    // appends the imports of the IBL maps that are ready, which are the ones lighting binds
    static void ImportIblMaps(RenderGraph &graph, const ImageBasedLighting &ibl, std::vector<RGTexture> &textures);

    // adds the legacy `sky_box_pass` as an External pass drawing its sky map into `scene_color` where `scene_depth`
    // passes the depth test
    static void AddSkyBoxPass(RenderGraph &graph, SkyBoxPass &sky_box_pass, RGTexture scene_color,
                              RGTexture scene_depth);

    // adds the legacy `tone_mapping_pass` from `scene_color` to `screen` as an External pass, or `output_pass` in its
    // place when set, which shows its own input image
    static void AddToneMappingPass(RenderGraph &graph, RGTexture scene_color, ScreenQuadPass &tone_mapping_pass,
                                   ScreenQuadPass *output_pass, RGTexture screen);

    // adds the frame's tail once `screen` holds the final image: the screenshot readbacks, the legacy `ui_pass` wrapped
    // as an External pass when the ui is shown, and `present_pass` drawing `screen` into the back buffer
    void AddPresentPasses(RenderGraph &graph, RGTexture screen, UiPass *ui_pass, const ScreenQuadPass &present_pass);

    // compiles and records the frame's graph, dumping it first when a dump is pending
    void ExecuteGraph(RenderGraph &graph);

    RHIContext *rhi_;
    SceneRenderProxy *scene_render_proxy_;

    Vector2UInt debug_point_{UINT_MAX, UINT_MAX};
    RenderResolution resolution_;

    const RenderConfig &render_config_;

    bool scene_loaded_ = false;

    std::atomic<int32_t> pending_async_tasks_{0};

    // images behind the transients of the renderer's graphs, kept across frames
    RGTexturePool graph_texture_pool_;

private:
    struct PendingScreenshot
    {
        std::string file_path;
        bool capture_ui;
        ScreenshotCallback on_complete;
    };

    [[nodiscard]] std::optional<PendingScreenshot> TakeScreenshotRequest(bool capture_ui);

    // a staging buffer the size of `image`, saved as the screenshot once the frame completes. the caller records the
    // copy.
    [[nodiscard]] RHIResourceRef<RHIBuffer> CreateScreenshotBuffer(const RHIImage &image, PendingScreenshot screenshot);

    std::optional<PendingScreenshot> pending_screenshot_;

    std::string graph_dump_path_;
    std::function<void()> graph_dump_completion_;
};
} // namespace sparkle
