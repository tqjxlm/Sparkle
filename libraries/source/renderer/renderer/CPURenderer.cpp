#include "renderer/renderer/CPURenderer.h"

#include "core/Profiler.h"
#include "renderer/pass/ScreenQuadPass.h"
#include "renderer/pass/UiPass.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "rhi/RHI.h"

namespace sparkle
{
CPURenderer::CPURenderer(const RenderConfig &render_config, RHIContext *rhi_context,
                         SceneRenderProxy *scene_render_proxy)
    : Renderer(render_config, rhi_context, scene_render_proxy),
      output_image_(image_size_.x(), image_size_.y(), PixelFormat::RGBAFloat16)
{
    ASSERT_EQUAL(render_config.pipeline, RenderConfig::Pipeline::cpu);
}

void CPURenderer::InitRenderResources()
{
    scene_render_proxy_->InitRenderResources(rhi_, render_config_);

    image_buffer_ = rhi_->CreateBuffer({.size = output_image_.GetStorageSize(),
                                        .usages = RHIBuffer::BufferUsage::TransferSrc,
                                        .mem_properties = RHIMemoryProperty::None,
                                        .is_dynamic = true},
                                       "RayTracingOutputBuffer");

    screen_texture_ = rhi_->CreateImage(
        RHIImage::Attribute{
            .format = output_image_.GetFormat(),
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
                        .filtering_method_min = RHISampler::FilteringMethod::Linear,
                        .filtering_method_mag = RHISampler::FilteringMethod::Linear,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Linear},
            .width = image_size_.x(),
            .height = image_size_.y(),
            .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::TransferDst |
                      RHIImage::ImageUsage::ColorAttachment,
            .msaa_samples = static_cast<uint8_t>(rhi_->GetConfig().msaa_samples),
        },
        "CpuPipelineColorBuffer");

    auto base_rt = rhi_->CreateRenderTarget({}, screen_texture_, nullptr, "CpuPipelineRenderTarget");

    screen_quad_pass_ =
        PipelinePass::Create<ScreenQuadPass>(render_config_, rhi_, screen_texture_, rhi_->GetBackBufferRenderTarget());

    ui_pass_ = PipelinePass::Create<UiPass>(render_config_, rhi_, base_rt);
}

void CPURenderer::Render()
{
    PROFILE_SCOPE("CPURenderer::Render");

    // CPU workload: software ray tracing
    {
        scene_render_proxy_->GetCamera()->RenderCPU(*scene_render_proxy_, render_config_, debug_point_);
        scene_render_proxy_->GetCamera()->Print(output_image_);
    }

    // GPU workload: copy the image to a texture
    {
        image_buffer_->Upload(rhi_, output_image_.GetRawData());

        screen_texture_->Transition({.target_layout = RHIImageLayout::TransferDst,
                                     .after_stage = RHIPipelineStage::Top,
                                     .before_stage = RHIPipelineStage::Transfer});

        image_buffer_->CopyToImage(screen_texture_.get());
    }

    // post process: ui
    {
        if (render_config_.render_ui)
        {
            screen_texture_->Transition({.target_layout = RHIImageLayout::ColorOutput,
                                         .after_stage = RHIPipelineStage::Transfer,
                                         .before_stage = RHIPipelineStage::ColorOutput});
            ui_pass_->Render();
            screen_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                         .after_stage = RHIPipelineStage::ColorOutput,
                                         .before_stage = RHIPipelineStage::PixelShader});
        }
        else
        {
            screen_texture_->Transition({.target_layout = RHIImageLayout::Read,
                                         .after_stage = RHIPipelineStage::Transfer,
                                         .before_stage = RHIPipelineStage::PixelShader});
        }
    }

    // screen pass: render it on a screen quad
    {
        screen_quad_pass_->Render();
    }
}

void CPURenderer::Update()
{
    PROFILE_SCOPE("CPURenderer::Update");

    screen_quad_pass_->UpdateFrameData(render_config_, scene_render_proxy_);

    if (ui_pass_)
    {
        ui_pass_->UpdateFrameData(render_config_, scene_render_proxy_);
    }
}

CPURenderer::~CPURenderer() = default;
} // namespace sparkle
