#pragma once

#include "renderer/pass/PipelinePass.h"

#include "rhi/RHIPIpelineState.h"

namespace sparkle
{
class RenderGraph;
struct RGTexture;

// draws a cube map around the camera where nothing nearer was drawn
class SkyBoxPass : public PipelinePass
{
public:
    // draws into a color attachment of `color_format` at slot 0, tested against a depth attachment of `depth_format`
    SkyBoxPass(RHIContext *rhi, PixelFormat color_format, PixelFormat depth_format);

    void InitRenderResources(const RenderConfig &config) override;

    void UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene) override;

    // adds a Raster pass drawing the cube map `sky_map`, sampled with `sky_map_sampler`, into `scene_color` where
    // `scene_depth` is at the far plane
    void AddTo(RenderGraph &graph, RGTexture sky_map, const RHISampler::SamplerAttribute &sky_map_sampler,
               RGTexture scene_color, RGTexture scene_depth) const;

private:
    RHIAttachmentSignature signature_;

    RHIResourceRef<RHIBuffer> vertex_buffer_;
    RHIResourceRef<RHIBuffer> index_buffer_;

    RHIResourceRef<RHIPipelineState> pipeline_state_;

    RHIResourceRef<RHIBuffer> vs_ub_;

    DrawArgs draw_args_;
};
} // namespace sparkle
