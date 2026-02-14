#include "renderer/renderer/DeferredRenderer.h"

#include "core/Profiler.h"
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

namespace sparkle
{
DeferredRenderer::DeferredRenderer(const RenderConfig &render_config, RHIContext *rhi_context,
                                   SceneRenderProxy *scene_render_proxy)
    : Renderer(render_config, rhi_context, scene_render_proxy)
{
    ASSERT_EQUAL(render_config_.pipeline, RenderConfig::Pipeline::deferred);
}

DeferredRenderer::~DeferredRenderer() = default;

void DeferredRenderer::InitRenderResources()
{
    scene_render_proxy_->InitRenderResources(rhi_, render_config_);

    gbuffer_.InitRenderResources(rhi_, image_size_);

    scene_color_ = rhi_->CreateImage(
        RHIImage::Attribute{
            .format = PixelFormat::RGBAFloat16,
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
            .width = image_size_.x(),
            .height = image_size_.y(),
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::ColorAttachment,
            .msaa_samples = 1,
        },
        "SceneColor");

    scene_depth_ = rhi_->CreateImage(
        RHIImage::Attribute{
            .format = PixelFormat::D32,

            .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
            .width = image_size_.x(),
            .height = image_size_.y(),
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::DepthStencilAttachment,
            .msaa_samples = 1,
        },
        "SceneDepth");

    lighting_rt_ = rhi_->CreateRenderTarget({}, scene_color_, nullptr, "SceneColorRT");

    screen_color_ = rhi_->CreateImage(
        RHIImage::Attribute{
            .format = PixelFormat::B8G8R8A8_SRGB,
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
            .width = image_size_.x(),
            .height = image_size_.y(),
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::ColorAttachment |
                      RHIImage::ImageUsage::TransferSrc,

            .msaa_samples = 1,
        },
        "ScreenColor");

    screen_color_rt_ = rhi_->CreateRenderTarget({}, screen_color_, nullptr, "ToneMappingRT");

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

    ui_pass_ = PipelinePass::Create<UiPass>(render_config_, rhi_, screen_color_rt_);
}

void DeferredRenderer::Render()
{
    PROFILE_SCOPE("DeferredRenderer::Render");

    if (directional_shadow_pass_)
    {
        directional_shadow_pass_->Render();

        directional_shadow_pass_->GetOutput()->GetDepthImage()->Transition({.target_layout = RHIImageLayout::Read,
                                                                            .after_stage = RHIPipelineStage::LateZ,
                                                                            .before_stage = RHIPipelineStage::Bottom});
    }

    if (ibl_ && ibl_->NeedUpdate())
    {
        ibl_->CookOnTheFly(render_config_);

        if (!ibl_->NeedUpdate())
        {
            UnregisterAsyncTask();
        }
    }

    // geometry pass: render to gbuffer
    gbuffer_pass_->Render();

    gbuffer_.Transition({.target_layout = RHIImageLayout::Read,
                         .after_stage = RHIPipelineStage::ColorOutput,
                         .before_stage = RHIPipelineStage::PixelShader});

    // lighting pass: render to scene_color

    // lighting pass need depth buffer to reconstruct world position
    scene_depth_->Transition({.target_layout = RHIImageLayout::Read,
                              .after_stage = RHIPipelineStage::LateZ,
                              .before_stage = RHIPipelineStage::PixelShader});

    directional_lighting_pass_->Render();

    // later passes may still need depth buffer
    scene_depth_->Transition({.target_layout = RHIImageLayout::DepthStencilOutput,
                              .after_stage = RHIPipelineStage::PixelShader,
                              .before_stage = RHIPipelineStage::EarlyZ});

    if (sky_box_pass_)
    {
        sky_box_pass_->Render();
    }

    scene_color_->Transition({.target_layout = RHIImageLayout::Read,
                              .after_stage = RHIPipelineStage::ColorOutput,
                              .before_stage = RHIPipelineStage::PixelShader});

    // screen space passes (post processing): render to screen_color
    {
        if (debug_output_pass_)
        {
            debug_output_pass_->Render();
        }
        else
        {
            tone_mapping_pass_->Render();
        }

        bool has_readback = ReadbackFinalOutputIfRequested(screen_color_rt_.get(), false,
                                                           RHIPipelineStage::ColorOutput);

        if (render_config_.render_ui)
        {
            if (has_readback)
            {
                screen_color_->Transition({.target_layout = RHIImageLayout::ColorOutput,
                                           .after_stage = RHIPipelineStage::Transfer,
                                           .before_stage = RHIPipelineStage::ColorOutput});
            }

            ui_pass_->Render();
        }

        has_readback = ReadbackFinalOutputIfRequested(screen_color_rt_.get(), true,
                                                      RHIPipelineStage::ColorOutput);

        screen_color_->Transition(
            {.target_layout = RHIImageLayout::Read,
             .after_stage = has_readback ? RHIPipelineStage::Transfer : RHIPipelineStage::ColorOutput,
             .before_stage = RHIPipelineStage::PixelShader});
    }

    // screen pass: copy screen_color to final buffer
    {
        present_pass_->Render();
    }
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
    case RenderConfig::OutputImage::IBL_BrdfTexture:
        if (!ibl_)
        {
            return false;
        }
        debug_output_pass_ =
            PipelinePass::Create<ScreenQuadPass>(render_config_, rhi_, ibl_->GetBRDFMap(), screen_color_rt_);
        return true;
    case RenderConfig::OutputImage::IBL_DiffuseMap:
        if (!ibl_)
        {
            return false;
        }
        sky_box_pass_->OverrideSkyMap(ibl_->GetDiffuseMap());
        return true;
    case RenderConfig::OutputImage::IBL_SpecularMap:
        if (!ibl_)
        {
            return false;
        }
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

    if ((sky_proxy != nullptr) && sky_proxy->GetSkyMap())
    {
        directional_lighting_pass_->SetSkyLight(sky_proxy);

        if (!sky_box_pass_)
        {
            sky_box_pass_ =
                PipelinePass::Create<SkyBoxPass>(render_config_, rhi_, sky_proxy, scene_color_, scene_depth_);
            sky_box_pass_->InitRenderResources(render_config_);
        }

        if (!ibl_)
        {
            if (render_config_.use_diffuse_ibl || render_config_.use_specular_ibl)
            {
                ibl_ = std::make_unique<ImageBasedLighting>(sky_proxy->GetSkyMap());
                ibl_->InitRenderResources(rhi_, render_config_);

                if (ibl_->NeedUpdate())
                {
                    RegisterAsyncTask();
                }

                directional_lighting_pass_->SetIBL(ibl_.get());
            }
        }
    }
    else
    {
        directional_lighting_pass_->SetSkyLight(nullptr);

        if (sky_box_pass_)
        {
            sky_box_pass_.reset();
        }

        if (ibl_)
        {
            ibl_.reset();
            directional_lighting_pass_->SetIBL(nullptr);
        }
    }
}
} // namespace sparkle
