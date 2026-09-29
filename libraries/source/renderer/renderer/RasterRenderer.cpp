#include "renderer/renderer/RasterRenderer.h"

#include "core/Profiler.h"
#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/DepthPass.h"
#include "renderer/pass/LightingInputs.h"
#include "renderer/pass/SkyBoxPass.h"
#include "renderer/pass/ToneMappingPass.h"
#include "renderer/proxy/DirectionalLightRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "renderer/resource/ImageBasedLighting.h"
#include "rhi/RHI.h"

namespace sparkle
{
namespace
{
// a cube map the sky box shows and the sampler it samples the map with
struct SkyBoxMap
{
    RHIResourceRef<RHIImage> image;
    RHISampler::SamplerAttribute sampler;
};

// the cube map the sky box shows in output mode `mode`: an IBL map once it is ready in the IBL map modes, otherwise
// `sky_map`
SkyBoxMap GetSkyBoxMap(RenderConfig::OutputImage mode, const ImageBasedLighting *ibl,
                       const RHIResourceRef<RHIImage> &sky_map)
{
    RHIResourceRef<RHIImage> ibl_map;
    if (ibl && mode == RenderConfig::OutputImage::IBLDiffuseMap)
    {
        ibl_map = ibl->GetDiffuseMap();
    }
    else if (ibl && mode == RenderConfig::OutputImage::IBLSpecularMap)
    {
        ibl_map = ibl->GetSpecularMap();
    }
    return ibl_map ? SkyBoxMap{.image = ibl_map, .sampler = ImageBasedLighting::MapSampler}
                   : SkyBoxMap{.image = sky_map, .sampler = SkyRenderProxy::SkyMapSampler};
}
} // namespace

RasterRenderer::RasterRenderer(const RenderConfig &render_config, RHIContext *rhi_context,
                               SceneRenderProxy *scene_render_proxy, RGTexturePool &graph_texture_pool)
    : Renderer(render_config, rhi_context, scene_render_proxy, graph_texture_pool)
{
}

RasterRenderer::~RasterRenderer() = default;

void RasterRenderer::InitRenderResources()
{
    scene_render_proxy_->InitRenderResources(rhi_, render_config_);

    InitPostChain(ToneMappingPass::ScreenFormat, PostChain::ScreenPass::ToneMapping);

    InitScenePasses();

    sky_box_pass_ =
        PipelinePass::Create<SkyBoxPass>(render_config_, rhi_, SceneColorDesc.format, SceneDepthDesc.format);
}

RGTexture RasterRenderer::BuildGraph(RenderGraph &graph)
{
    PROFILE_SCOPE("RasterRenderer::BuildGraph");

    if (ibl_cook_pending_)
    {
        if (!ibl_->NeedUpdate())
        {
            UnregisterAsyncTask();
            ibl_cook_pending_ = false;
        }
    }

    if (ibl_)
    {
        ibl_->AddCookPasses(graph, render_config_);
    }

    const auto shadow_map = directional_shadow_pass_ ? directional_shadow_pass_->AddTo(graph) : RGTexture{};
    const auto lighting = LightingInputs::Import(graph, shadow_map, ibl_);
    const auto scene = AddScenePasses(graph, lighting);

    const auto output_mode = render_config_.output_image;
    const auto sky_map = GetSkyBoxMap(output_mode, ibl_, bound_sky_proxy_ ? bound_sky_proxy_->GetSkyMap() : nullptr);
    if (sky_map.image)
    {
        sky_box_pass_->AddTo(graph, graph.Import("SkyMap", sky_map.image), sky_map.sampler, scene.color, scene.depth);
    }

    return scene.color;
}

void RasterRenderer::Update()
{
    PROFILE_SCOPE("RasterRenderer::Update");

    HandleSceneChanges();

    auto *directional_light = scene_render_proxy_->GetDirectionalLight();

    if (directional_shadow_pass_)
    {
        directional_shadow_pass_->SetProjectionMatrix(directional_light->GetRenderData().shadow_matrix);
        directional_shadow_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
    }

    UpdateScenePasses();

    sky_box_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
}

void RasterRenderer::HandleSceneChanges()
{
    auto *directional_light = scene_render_proxy_->GetDirectionalLight();

    if (directional_light)
    {
        if (!directional_shadow_pass_)
        {
            directional_shadow_pass_ = PipelinePass::Create<DepthPass>(render_config_, rhi_, scene_render_proxy_,
                                                                       render_config_.shadow_map_resolution,
                                                                       render_config_.shadow_map_resolution);
        }
    }
    else
    {
        directional_shadow_pass_.reset();
    }

    auto *sky_proxy = scene_render_proxy_->GetSkyLight();
    if (sky_proxy != nullptr && !sky_proxy->GetSkyMap())
    {
        sky_proxy = nullptr;
    }

    if (bound_sky_proxy_ != sky_proxy)
    {
        if (ibl_cook_pending_)
        {
            UnregisterAsyncTask();
            ibl_cook_pending_ = false;
        }

        bound_sky_proxy_ = sky_proxy;
        ibl_ = sky_proxy ? sky_proxy->GetImageBasedLighting() : nullptr;

        if (ibl_ && ibl_->NeedUpdate())
        {
            RegisterAsyncTask();
            ibl_cook_pending_ = true;
        }
    }
}
} // namespace sparkle
