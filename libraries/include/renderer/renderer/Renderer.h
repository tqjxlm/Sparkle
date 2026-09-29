#pragma once

#include "core/math/Types.h"
#include "renderer/RenderConfig.h"
#include "renderer/graph/RGPassTimers.h"
#include "renderer/graph/RGTexturePool.h"
#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/PostChain.h"
#include "rhi/RHIImage.h"

#include <atomic>
#include <functional>
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

    // builds the frame's graph, its scene passes (BuildGraph) followed by the post chain, then compiles, records and
    // dumps it when a dump is pending
    void Render();

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

protected:
    virtual void Update() = 0;

    // adds the frame's passes before the post chain, returning the texture they leave the scene in
    [[nodiscard]] virtual RGTexture BuildGraph(RenderGraph &graph) = 0;

    // creates the post chain, whose Screen transient is of `screen_format`
    void InitPostChain(PixelFormat screen_format, PostChain::ScreenPass screen_pass);

    RHIContext *rhi_;
    SceneRenderProxy *scene_render_proxy_;

    Vector2UInt debug_point_{UINT_MAX, UINT_MAX};
    RenderResolution resolution_;

    const RenderConfig &render_config_;

    bool scene_loaded_ = false;

    std::atomic<int32_t> pending_async_tasks_{0};

private:
    // compiles and records the frame's graph, then dumps it when a dump is pending
    void ExecuteGraph(RenderGraph &graph);

    // images behind the transients of the renderer's graphs, kept across frames and renderers
    RGTexturePool &graph_texture_pool_;

    std::unique_ptr<PostChain> post_chain_;

    // times the raster passes of the renderer's graphs across frames
    RGPassTimers graph_pass_timers_;

    std::string graph_dump_path_;
    std::function<void()> graph_dump_completion_;
    std::function<void(const nlohmann::json &)> graph_dump_consumer_;
};
} // namespace sparkle
