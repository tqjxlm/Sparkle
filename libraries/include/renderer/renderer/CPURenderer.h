#pragma once

#include "renderer/renderer/Renderer.h"

#include "io/Image.h"
#include "rhi/RHIBuffer.h"
#include "rhi/RHIImage.h"

namespace sparkle
{
class CPURenderer : public Renderer
{
public:
    CPURenderer(const RenderConfig &render_config, RHIContext *rhi_context, SceneRenderProxy *scene_render_proxy);

    [[nodiscard]] RenderConfig::Pipeline GetRenderMode() const override
    {
        return RenderConfig::Pipeline::cpu;
    }

    void Render() override;

    void InitRenderResources() override;

    void Update() override;

    ~CPURenderer() override;

private:
    RHIResourceRef<RHIBuffer> image_buffer_;
    RHIResourceRef<RHIImage> screen_texture_;
    std::unique_ptr<class ScreenQuadPass> screen_quad_pass_;

    Image2D output_image_;

    std::unique_ptr<class UiPass> ui_pass_;
};
} // namespace sparkle
