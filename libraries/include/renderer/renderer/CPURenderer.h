#pragma once

#include "renderer/renderer/Renderer.h"

#include "io/Image.h"
#include "renderer/resource/GBuffer.h"
#include "rhi/RHIBuffer.h"

namespace sparkle
{
class CPURenderer : public Renderer
{
public:
    CPURenderer(const RenderConfig &render_config, RHIContext *rhi_context, SceneRenderProxy *scene_render_proxy,
                RGTexturePool &graph_texture_pool);

    [[nodiscard]] RenderConfig::Pipeline GetRenderMode() const override
    {
        return RenderConfig::Pipeline::Cpu;
    }

    void InitRenderResources() override;

    void Update() override
    {
    }

    [[nodiscard]] bool IsReadyForAutoScreenshot() const override;

    ~CPURenderer() override;

private:
    [[nodiscard]] RGTexture BuildGraph(RenderGraph &graph) override;

    void RenderPixel(unsigned i, unsigned j, Scalar pixel_width, Scalar pixel_height, const SceneRenderProxy &scene,
                     const RenderConfig &config, const Vector2UInt &debug_point);

    void BasePass(const SceneRenderProxy &scene, const RenderConfig &config, const Vector2UInt &debug_point);

    void DenoisePass(const RenderConfig &config, const Vector2UInt &debug_point);

    void ToneMappingPass(Image2D &image);

    CameraRenderProxy *camera_;

    RHIResourceRef<RHIBuffer> image_buffer_;

    Image2D output_image_;

    // output of rendering passes. cleared every frame.
    CPUGBuffer gbuffer_;

    // cleared every frame
    std::vector<std::vector<Vector4>> ping_pong_buffer_;

    // accumulate all frame's results after temporal denoising. cleared on dirty
    std::vector<std::vector<Vector4>> frame_buffer_;

    unsigned sub_pixel_count_;
    unsigned actual_sample_per_pixel_;
    uint32_t dispatched_sample_count_ = 0;
};
} // namespace sparkle
