#pragma once

#include "renderer/graph/RenderGraph.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace sparkle
{
struct RenderConfig;
class RHIContext;
class SceneRenderProxy;
class ScreenQuadPass;
class UiPass;

// the passes every frame ends with, which AddTo adds
class PostChain
{
public:
    // the pass drawing the scene into the Screen transient
    enum class ScreenPass : uint8_t
    {
        // none: the scene is the screen
        None,
        ToneMapping,
        Upsample,
    };

    // the pass drawing the texture render_graph_view names into the Screen transient; its first access samples it
    static constexpr const char *GraphViewPassName = "GraphView";

    // Screen is a transient of `screen_format` at output resolution
    PostChain(const RenderConfig &config, RHIContext *rhi, PixelFormat screen_format, ScreenPass screen_pass);

    ~PostChain();

    // the next frame saves its screen, with the ui when `capture_ui` and the ui draws, to screenshots/<file_path>.png
    void RequestScreenshot(const std::string &file_path, bool capture_ui, std::function<void()> on_complete);

    void UpdateFrameData(SceneRenderProxy *scene);

    // adds the passes after the scene passes: the screen pass drawing `scene` into the Screen transient, or `scene` as
    // the screen without one, unless the texture render_graph_view names is drawn there instead; the screenshot
    // readbacks; the ui when shown; and the present drawing the screen into the back buffer
    void AddTo(RenderGraph &graph, RGTexture scene);

private:
    struct PendingScreenshot
    {
        std::string file_path;
        bool capture_ui;
        std::function<void()> on_complete;
    };

    // the texture render_graph_view names when the post chain can show it; otherwise invalid, with a warning once per
    // change of the value
    [[nodiscard]] RGTexture FindGraphView(const RenderGraph &graph);

    // when a screenshot with or without ui is pending, adds a pass that copies `texture` into a staging buffer, which
    // is saved once the frame completes
    void AddReadback(RenderGraph &graph, RGTexture texture, bool capture_ui);

    const RenderConfig &config_;
    RHIContext *rhi_;

    RGTextureDesc screen_desc_;
    // null for ScreenPass::None
    std::unique_ptr<ScreenQuadPass> screen_pass_;
    // null when headless
    std::unique_ptr<UiPass> ui_pass_;
    std::unique_ptr<ScreenQuadPass> present_pass_;

    std::optional<PendingScreenshot> pending_screenshot_;

    // draws the texture render_graph_view names into the Screen transient
    std::unique_ptr<ScreenQuadPass> graph_view_pass_;
    // the render_graph_view the post chain last looked up, and whether its fallback was logged
    std::string graph_view_;
    bool graph_view_warned_ = false;
};
} // namespace sparkle
