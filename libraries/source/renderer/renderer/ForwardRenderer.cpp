#include "renderer/renderer/ForwardRenderer.h"

#include "core/Profiler.h"
#include "renderer/pass/DepthPass.h"
#include "renderer/pass/ForwardMeshPass.h"
#include "renderer/pass/ScreenQuadPass.h"
#include "renderer/pass/SkyBoxPass.h"
#include "renderer/pass/ToneMappingPass.h"
#include "renderer/pass/UiPass.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/DirectionalLightRenderProxy.h"
#include "renderer/proxy/MeshRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "renderer/resource/ImageBasedLighting.h"
#include "rhi/RHI.h"
#include "rhi/RHIRayTracing.h"

namespace sparkle
{
ForwardRenderer::ForwardRenderer(const RenderConfig &render_config, RHIContext *rhi_context,
                                 SceneRenderProxy *scene_render_proxy)
    : Renderer(render_config, rhi_context, scene_render_proxy)
{
    ASSERT_EQUAL(render_config_.pipeline, RenderConfig::Pipeline::forward);

    use_ray_tracing_ = render_config.IsRayTracingMode();
    use_prepass_ = render_config_.use_prepass;
    use_ssao_ = render_config_.use_ssao;
    resolve_prepass_depth_ = use_ssao_;
}

void ForwardRenderer::InitRenderResources()
{
    scene_render_proxy_->InitRenderResources(rhi_, render_config_);

    if (use_prepass_)
    {
        pre_pass_ = PipelinePass::Create<DepthPass>(render_config_, rhi_, scene_render_proxy_, image_size_.x(),
                                                    image_size_.y());
    }

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
        "BasePassSceneColor");

    if (use_prepass_)
    {
        scene_depth_ = pre_pass_->GetOutput()->GetDepthImage();
    }
    else
    {
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
            "BasePassSceneDepth");
    }

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
    tone_mapping_pass_ = PipelinePass::Create<ToneMappingPass>(render_config_, rhi_, scene_color_, screen_color_rt_);

    present_pass_ =
        PipelinePass::Create<ScreenQuadPass>(render_config_, rhi_, screen_color_, rhi_->GetBackBufferRenderTarget());

    ForwardMeshPass::PassResources pbr_resources;
    pbr_resources.scene_color = scene_color_;
    pbr_resources.scene_depth = scene_depth_;

    if (use_ray_tracing_)
    {
        tlas_ = rhi_->CreateTLAS("TLAS");

        pbr_resources.tlas = tlas_;
        scene_color_pass_ =
            PipelinePass::Create<ForwardMeshPass>(render_config_, rhi_, scene_render_proxy_, pbr_resources);
    }
    else
    {
        if (resolve_prepass_depth_)
        {
            pbr_resources.prepass_depth_map = pre_pass_->GetOutput()->GetDepthImage();
        }

        scene_color_pass_ =
            PipelinePass::Create<ForwardMeshPass>(render_config_, rhi_, scene_render_proxy_, pbr_resources);
    }

    ui_pass_ = PipelinePass::Create<UiPass>(render_config_, rhi_, screen_color_rt_);
}

void ForwardRenderer::Render()
{
    PROFILE_SCOPE("ForwardRenderer::Render");

    if (directional_shadow_pass_)
    {
        directional_shadow_pass_->Render();

        directional_shadow_pass_->GetOutput()->GetDepthImage()->Transition({.target_layout = RHIImageLayout::Read,
                                                                            .after_stage = RHIPipelineStage::LateZ,
                                                                            .before_stage = RHIPipelineStage::Bottom});
    }

    if (use_prepass_)
    {
        pre_pass_->Render();
    }

    if (ibl_ && ibl_->NeedUpdate())
    {
        ibl_->CookOnTheFly(render_config_);

        if (!ibl_->NeedUpdate())
        {
            UnregisterAsyncTask();
        }
    }

    // base pass: render to scene_color
    scene_color_pass_->Render();

    if (sky_box_pass_)
    {
        sky_box_pass_->Render();
    }

    scene_color_->Transition({.target_layout = RHIImageLayout::Read,
                              .after_stage = RHIPipelineStage::ColorOutput,
                              .before_stage = RHIPipelineStage::PixelShader});

    // screen space passes (post processing): render to screen_color
    {
        if (texture_output_pass_)
        {
            texture_output_pass_->Render();
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

bool ForwardRenderer::UpdateOutputMode(RenderConfig::OutputImage mode)
{
    if (output_mode_ == mode)
    {
        return false;
    }

    switch (mode)
    {
    case RenderConfig::OutputImage::SceneColor:
        texture_output_pass_ = nullptr;
        if (sky_box_pass_)
        {
            sky_box_pass_->OverrideSkyMap(nullptr);
        }
        return true;
    case RenderConfig::OutputImage::IBL_BrdfTexture:
        if (!ibl_)
        {
            return false;
        }
        texture_output_pass_ =
            PipelinePass::Create<ScreenQuadPass>(render_config_, rhi_, ibl_->GetBRDFMap(), screen_color_rt_);
        return true;
    case RenderConfig::OutputImage::IBL_DiffuseMap:
        if (!ibl_ || !sky_box_pass_)
        {
            return false;
        }
        sky_box_pass_->OverrideSkyMap(ibl_->GetDiffuseMap());
        return true;
    case RenderConfig::OutputImage::IBL_SpecularMap:
        if (!ibl_ || !sky_box_pass_)
        {
            return false;
        }
        sky_box_pass_->OverrideSkyMap(ibl_->GetSpecularMap());
        return true;
    default:
        UnImplemented(mode);
        return false;
    }
}

void ForwardRenderer::Update()
{
    PROFILE_SCOPE("ForwardRenderer::Update");

    HandleSceneChanges();

    auto *camera = scene_render_proxy_->GetCamera();
    auto *directional_light = scene_render_proxy_->GetDirectionalLight();

    auto output_mode_changed = UpdateOutputMode(render_config_.output_image);
    if (output_mode_changed)
    {
        output_mode_ = render_config_.output_image;
    }

    scene_color_pass_->UpdateFrameData(render_config_, scene_render_proxy_);

    if (directional_shadow_pass_)
    {
        directional_shadow_pass_->SetProjectionMatrix(directional_light->GetRenderData().shadow_matrix);
        directional_shadow_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
    }

    if (use_prepass_)
    {
        pre_pass_->SetProjectionMatrix(camera->GetViewProjectionMatrix());
    }

    if (sky_box_pass_)
    {
        sky_box_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
    }

    present_pass_->UpdateFrameData(render_config_, scene_render_proxy_);

    if (ui_pass_)
    {
        ui_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
    }

    tone_mapping_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
}

void ForwardRenderer::HandleSceneChanges()
{
    if (use_ray_tracing_)
    {
        bool need_rebuild_tlas = false;
        std::unordered_set<uint32_t> primitives_to_update;
        for (const auto &[type, primitive, from, to] : scene_render_proxy_->GetPrimitiveChangeList())
        {
            switch (type)
            {
            case SceneRenderProxy::PrimitiveChangeType::New:
                RegisterBLAS(primitive);
                need_rebuild_tlas = true;
                break;
            case SceneRenderProxy::PrimitiveChangeType::Remove:
                // for removed primitives, we don't need to re-register. their slots will be reused
                need_rebuild_tlas = true;
                break;
            case SceneRenderProxy::PrimitiveChangeType::Move:
                RegisterBLAS(primitive);
                need_rebuild_tlas = true;
                break;
            case SceneRenderProxy::PrimitiveChangeType::Update:
                primitives_to_update.insert(to);
                break;
            default:
                UnImplemented(type);
                break;
            }
        }

        if (need_rebuild_tlas)
        {
            // structural change, rebuild TLAS
            tlas_->Build();
        }
        else if (!primitives_to_update.empty())
        {
            // non-structural change, update TLAS
            tlas_->Update(primitives_to_update);
        }
    }

    auto *directional_light = scene_render_proxy_->GetDirectionalLight();

    if (directional_light)
    {
        if (!directional_shadow_pass_)
        {
            directional_shadow_pass_ = PipelinePass::Create<DepthPass>(render_config_, rhi_, scene_render_proxy_,
                                                                       render_config_.shadow_map_resolution,
                                                                       render_config_.shadow_map_resolution);

            scene_color_pass_->SetDirectionalShadow(directional_shadow_pass_->GetOutput()->GetDepthImage());
        }
    }
    else
    {
        if (directional_shadow_pass_)
        {
            directional_shadow_pass_.reset();
            scene_color_pass_->SetDirectionalShadow(nullptr);
        }
    }

    auto *sky_proxy = scene_render_proxy_->GetSkyLight();

    if ((sky_proxy != nullptr) && sky_proxy->GetSkyMap())
    {
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

                scene_color_pass_->SetIBL(ibl_.get());
            }
        }
    }
    else
    {
        if (sky_box_pass_)
        {
            sky_box_pass_.reset();
        }

        if (ibl_)
        {
            ibl_.reset();
            scene_color_pass_->SetIBL(nullptr);
        }
    }
}

void ForwardRenderer::RegisterBLAS(PrimitiveRenderProxy *primitive)
{
    if (!primitive->IsMesh())
    {
        return;
    }

    ASSERT(primitive->GetPrimitiveIndex() != UINT_MAX);

    const auto *mesh = primitive->As<MeshRenderProxy>();

    const auto &blas = mesh->GetAccelerationStructure();
    tlas_->SetBLAS(blas.get(), primitive->GetPrimitiveIndex());
}

ForwardRenderer::~ForwardRenderer() = default;

} // namespace sparkle
