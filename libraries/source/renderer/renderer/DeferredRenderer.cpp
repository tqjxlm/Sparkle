#include "renderer/renderer/DeferredRenderer.h"

#include "core/Profiler.h"
#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/DepthPass.h"
#include "renderer/pass/DirectionalLightingPass.h"
#include "renderer/pass/GBufferPass.h"
#include "renderer/pass/PipelinePass.h"
#include "renderer/pass/ScreenQuadPass.h"
#include "renderer/pass/SkyBoxPass.h"
#include "renderer/pass/ToneMappingPass.h"
#include "renderer/pass/UiPass.h"
#include "renderer/proxy/DirectionalLightRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "renderer/resource/ImageBasedLighting.h"
#include "rhi/RHI.h"

#include <vector>

namespace sparkle
{
DeferredRenderer::DeferredRenderer(const RenderConfig &render_config, RHIContext *rhi_context,
                                   SceneRenderProxy *scene_render_proxy)
    : Renderer(render_config, rhi_context, scene_render_proxy)
{
    ASSERT_EQUAL(render_config_.pipeline, RenderConfig::Pipeline::Deferred);
}

DeferredRenderer::~DeferredRenderer() = default;

void DeferredRenderer::InitRenderResources()
{
    scene_render_proxy_->InitRenderResources(rhi_, render_config_);

    gbuffer_.InitRenderResources(rhi_, resolution_.scene);

    RHIRenderTarget::Attribute lighting_rt_attribute;
    lighting_rt_attribute.SetColorAttribute(
        RHIImage::Attribute{
            .format = PixelFormat::RGBAFloat16,
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
            .width = resolution_.scene.x(),
            .height = resolution_.scene.y(),
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::ColorAttachment,
            .msaa_samples = 1,
        },
        0);

    lighting_rt_ = rhi_->GetRenderTargetPool().Acquire(lighting_rt_attribute, "SceneColorRT");
    scene_color_ = lighting_rt_->GetColorImage(0);

    scene_depth_ = rhi_->CreateImage(
        RHIImage::Attribute{
            .format = PixelFormat::D32,

            .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
            .width = resolution_.scene.x(),
            .height = resolution_.scene.y(),
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::DepthStencilAttachment,
            .msaa_samples = 1,
        },
        "SceneDepth");

    RHIRenderTarget::Attribute screen_color_rt_attribute;
    screen_color_rt_attribute.SetColorAttribute(
        RHIImage::Attribute{
            .format = PixelFormat::B8G8R8A8Srgb,
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
            .width = resolution_.output.x(),
            .height = resolution_.output.y(),
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::ColorAttachment |
                      RHIImage::ImageUsage::TransferSrc,

            .msaa_samples = 1,
        },
        0);

    screen_color_rt_ = rhi_->GetRenderTargetPool().Acquire(screen_color_rt_attribute, "ToneMappingRT");
    screen_color_ = screen_color_rt_->GetColorImage(0);

    gbuffer_pass_ =
        PipelinePass::Create<GBufferPass>(render_config_, rhi_, scene_render_proxy_, gbuffer_.images, scene_depth_);

    tone_mapping_pass_ = PipelinePass::Create<ToneMappingPass>(render_config_, rhi_, scene_color_, screen_color_rt_);

    present_pass_ =
        PipelinePass::Create<ScreenQuadPass>(render_config_, rhi_, screen_color_, rhi_->GetBackBufferRenderTarget());

    directional_lighting_pass_ =
        PipelinePass::Create<DirectionalLightingPass>(render_config_, rhi_, lighting_rt_,
                                                      DirectionalLightingPass::PassResources{
                                                          .gbuffer = gbuffer_,
                                                          .camera = scene_render_proxy_->GetCamera(),
                                                          .light = scene_render_proxy_->GetDirectionalLight(),
                                                          .depth_texture = scene_depth_,
                                                      });

    if (!rhi_->IsHeadless())
    {
        ui_pass_ = PipelinePass::Create<UiPass>(render_config_, rhi_, screen_color_rt_);
    }
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

    RenderGraph graph(graph_texture_pool_, render_config_);
    const auto gbuffer = graph.Import("GBufferPacked", gbuffer_.packed_texture);
    const auto scene_depth = graph.Import("SceneDepth", scene_depth_);
    const auto scene_color = graph.Import("SceneColor", scene_color_);

    // lighting reconstructs world positions from the depth it samples
    std::vector<RGTexture> lighting_inputs{gbuffer, scene_depth};
    if (directional_shadow_pass_)
    {
        lighting_inputs.push_back(AddDirectionalShadowPass(graph, *directional_shadow_pass_));
    }

    // an IBL map exists once it is ready, and lighting samples it from then on
    if (ibl_)
    {
        ImportIblMaps(graph, *ibl_, lighting_inputs);
    }

    graph.AddExternalPass("GBuffer", [this, gbuffer, scene_depth](RGBuilder &builder) {
        // the values the pass clears to
        builder.ColorWrite(gbuffer, 0, Vector4(0.f, 0.f, 0.f, 1.f));
        builder.DepthWrite(scene_depth, 1.f);
        return [this](RGExternalContext &) { gbuffer_pass_->Render(); };
    });

    graph.AddExternalPass("Lighting", [this, &lighting_inputs, scene_color](RGBuilder &builder) {
        for (const auto input : lighting_inputs)
        {
            builder.Sampled(input, RHIShaderStageMask::Pixel);
        }
        builder.ColorWrite(scene_color, 0);
        builder.FullyOverwrites();
        return [this](RGExternalContext &) { directional_lighting_pass_->Render(); };
    });

    if (sky_box_pass_)
    {
        AddSkyBoxPass(graph, *sky_box_pass_, scene_color, scene_depth);
    }

    const auto screen = graph.Import("Screen", screen_color_);

    // the debug output mode shows its image in place of the tone-mapped scene
    AddToneMappingPass(graph, scene_color, *tone_mapping_pass_, debug_output_pass_.get(), screen);

    AddPresentPasses(graph, screen, ui_pass_.get(), *present_pass_);

    ExecuteGraph(graph);
}

bool DeferredRenderer::UpdateOutputMode(RenderConfig::OutputImage mode)
{
    if (output_mode_ == mode)
    {
        return false;
    }

    switch (mode)
    {
    case RenderConfig::OutputImage::SceneColor:
        debug_output_pass_ = nullptr;
        return true;
    case RenderConfig::OutputImage::IBLBrdfTexture:
        if (ibl_ == nullptr || !ibl_->GetBRDFMap())
        {
            return false;
        }
        debug_output_pass_ =
            PipelinePass::Create<ScreenQuadPass>(render_config_, rhi_, ibl_->GetBRDFMap(), screen_color_rt_);
        return true;
    case RenderConfig::OutputImage::IBLDiffuseMap:
        if (ibl_ == nullptr || !ibl_->GetDiffuseMap())
        {
            return false;
        }
        debug_output_pass_ = nullptr;
        sky_box_pass_->OverrideSkyMap(ibl_->GetDiffuseMap());
        return true;
    case RenderConfig::OutputImage::IBLSpecularMap:
        if (ibl_ == nullptr || !ibl_->GetSpecularMap())
        {
            return false;
        }
        debug_output_pass_ = nullptr;
        sky_box_pass_->OverrideSkyMap(ibl_->GetSpecularMap());
        return true;
    default:
        return false;
    }
}

void DeferredRenderer::Update()
{
    PROFILE_SCOPE("DeferredRenderer::Update");

    HandleSceneChanges();

    auto *directional_light = scene_render_proxy_->GetDirectionalLight();

    auto output_mode_changed = UpdateOutputMode(render_config_.output_image);
    if (output_mode_changed)
    {
        output_mode_ = render_config_.output_image;
    }

    if (directional_shadow_pass_)
    {
        directional_shadow_pass_->SetProjectionMatrix(directional_light->GetRenderData().shadow_matrix);
        directional_shadow_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
    }

    gbuffer_pass_->UpdateFrameData(render_config_, scene_render_proxy_);

    directional_lighting_pass_->UpdateFrameData(render_config_, scene_render_proxy_);

    present_pass_->UpdateFrameData(render_config_, scene_render_proxy_);

    if (ui_pass_)
    {
        ui_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
    }

    if (sky_box_pass_)
    {
        sky_box_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
    }

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

            directional_lighting_pass_->SetDirectionalShadow(directional_shadow_pass_->GetOutput()->GetDepthImage());
        }
    }
    else
    {
        if (directional_shadow_pass_)
        {
            directional_shadow_pass_.reset();

            directional_lighting_pass_->SetDirectionalShadow(nullptr);
        }
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
        sky_box_pass_.reset();
        ibl_ = sky_proxy ? sky_proxy->GetImageBasedLighting() : nullptr;
        directional_lighting_pass_->SetIBL(ibl_);

        if (ibl_ && ibl_->NeedUpdate())
        {
            RegisterAsyncTask();
            ibl_cook_pending_ = true;
        }
    }

    if (sky_proxy)
    {
        directional_lighting_pass_->SetSkyLight(sky_proxy);

        if (!sky_box_pass_)
        {
            sky_box_pass_ =
                PipelinePass::Create<SkyBoxPass>(render_config_, rhi_, sky_proxy, scene_color_, scene_depth_);
        }
    }
    else
    {
        directional_lighting_pass_->SetSkyLight(nullptr);
    }
}
} // namespace sparkle
