#include "renderer/renderer/DeferredRenderer.h"

#include "core/Profiler.h"
#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/DepthPass.h"
#include "renderer/pass/DirectionalLightingPass.h"
#include "renderer/pass/GBufferPass.h"
#include "renderer/pass/LightingInputs.h"
#include "renderer/pass/PipelinePass.h"
#include "renderer/pass/SkyBoxPass.h"
#include "renderer/pass/ToneMappingPass.h"
#include "renderer/proxy/DirectionalLightRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "renderer/resource/ImageBasedLighting.h"
#include "rhi/RHI.h"

namespace sparkle
{
DeferredRenderer::DeferredRenderer(const RenderConfig &render_config, RHIContext *rhi_context,
                                   SceneRenderProxy *scene_render_proxy, RGTexturePool &graph_texture_pool)
    : Renderer(render_config, rhi_context, scene_render_proxy, graph_texture_pool)
{
    ASSERT_EQUAL(render_config_.pipeline, RenderConfig::Pipeline::Deferred);
}

DeferredRenderer::~DeferredRenderer() = default;

void DeferredRenderer::InitRenderResources()
{
    scene_render_proxy_->InitRenderResources(rhi_, render_config_);

    const auto scene_color_format = GetSceneColorDesc().format;

    gbuffer_pass_ = PipelinePass::Create<GBufferPass>(render_config_, rhi_, scene_render_proxy_, SceneDepthDesc.format);

    InitPostChain(ToneMappedScreenDesc);

    tone_mapping_pass_ = PipelinePass::Create<ToneMappingPass>(render_config_, rhi_, screen_desc_.format);

    directional_lighting_pass_ =
        PipelinePass::Create<DirectionalLightingPass>(render_config_, rhi_, scene_color_format);

    sky_box_pass_ = PipelinePass::Create<SkyBoxPass>(render_config_, rhi_, scene_color_format, SceneDepthDesc.format);
}

void DeferredRenderer::Render()
{
    PROFILE_SCOPE("DeferredRenderer::Render");

    if (ibl_cook_pending_)
    {
        if (!ibl_->NeedUpdate())
        {
            UnregisterAsyncTask();
            ibl_cook_pending_ = false;
        }
    }

    RenderGraph graph(rhi_, graph_texture_pool_, render_config_);

    if (ibl_)
    {
        ibl_->AddCookPasses(graph, render_config_);
    }

    const auto shadow_map = directional_shadow_pass_ ? directional_shadow_pass_->AddTo(graph) : RGTexture{};
    const auto lighting = LightingInputs::Import(graph, shadow_map, ibl_);
    const auto scene_depth = graph.CreateTexture("SceneDepth", SceneDepthDesc);
    const auto gbuffer = gbuffer_pass_->AddTo(graph, scene_depth);
    const auto scene_color = graph.CreateTexture("SceneColor", GetSceneColorDesc());
    directional_lighting_pass_->AddTo(graph, lighting, gbuffer, scene_depth, scene_color);

    const auto output_mode = render_config_.output_image;
    const auto sky_map = GetSkyBoxMap(output_mode, ibl_, bound_sky_proxy_ ? bound_sky_proxy_->GetSkyMap() : nullptr);
    if (sky_map)
    {
        sky_box_pass_->AddTo(graph, graph.Import("SkyMap", sky_map), scene_color, scene_depth);
    }

    AddPostChain(graph, scene_color, tone_mapping_pass_.get());

    ExecuteGraph(graph);
}

void DeferredRenderer::Update()
{
    PROFILE_SCOPE("DeferredRenderer::Update");

    HandleSceneChanges();

    auto *directional_light = scene_render_proxy_->GetDirectionalLight();

    if (directional_shadow_pass_)
    {
        directional_shadow_pass_->SetProjectionMatrix(directional_light->GetRenderData().shadow_matrix);
        directional_shadow_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
    }

    gbuffer_pass_->UpdateFrameData(render_config_, scene_render_proxy_);

    directional_lighting_pass_->UpdateFrameData(render_config_, scene_render_proxy_);

    sky_box_pass_->UpdateFrameData(render_config_, scene_render_proxy_);

    tone_mapping_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
}

void DeferredRenderer::HandleSceneChanges()
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
