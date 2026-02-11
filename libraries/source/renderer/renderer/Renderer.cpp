#include "renderer/renderer/Renderer.h"

#include "core/ThreadManager.h"
#include "io/Image.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/renderer/CPURenderer.h"
#include "renderer/renderer/DeferredRenderer.h"
#include "renderer/renderer/ForwardRenderer.h"
#include "renderer/renderer/GPURenderer.h"
#include "rhi/RHI.h"

#include <filesystem>

namespace sparkle
{
std::unique_ptr<Renderer> Renderer::CreateRenderer(const RenderConfig &render_config, RHIContext *rhi_context,
                                                   SceneRenderProxy *scene_render_proxy)
{
    ASSERT(ThreadManager::IsInRenderThread());

    std::unique_ptr<Renderer> renderer;

    switch (render_config.pipeline)
    {
    case RenderConfig::Pipeline::cpu:
        renderer = std::make_unique<CPURenderer>(render_config, rhi_context, scene_render_proxy);
        break;
    case RenderConfig::Pipeline::gpu:
        renderer = std::make_unique<GPURenderer>(render_config, rhi_context, scene_render_proxy);
        break;
    case RenderConfig::Pipeline::forward:
        renderer = std::make_unique<ForwardRenderer>(render_config, rhi_context, scene_render_proxy);
        break;
    case RenderConfig::Pipeline::deferred:
        renderer = std::make_unique<DeferredRenderer>(render_config, rhi_context, scene_render_proxy);
        break;
    default:
        UnImplemented(render_config.pipeline);
        break;
    }

    rhi_context->BeginCommandBuffer();

    renderer->InitRenderResources();

    rhi_context->SubmitCommandBuffer();

    // the first initialization is very special. it's better to wait for completion.
    rhi_context->WaitForDeviceIdle();

    return renderer;
}

Renderer::Renderer(const RenderConfig &render_config, RHIContext *rhi_context, SceneRenderProxy *scene_render_proxy)
    : rhi_(rhi_context), scene_render_proxy_(scene_render_proxy),
      image_size_{render_config.image_width, render_config.image_height}, render_config_(render_config)
{
    Log(Info, "View size [{}, {}]", image_size_.x(), image_size_.y());
}

void Renderer::Tick()
{
    scene_render_proxy_->Update(rhi_, *scene_render_proxy_->GetCamera(), render_config_);

    Update();

    scene_render_proxy_->EndUpdate(rhi_);
}

void Renderer::OnFrameBufferResize(int width, int height)
{
    rhi_->RecreateFrameBuffer(width, height);
}

void Renderer::RequestSaveScreenshot(const std::string &file_path, bool capture_ui,
                                     Renderer::ScreenshotCallback on_complete)
{
    ASSERT(ThreadManager::IsInRenderThread());
    ASSERT(!file_path.empty());

    std::filesystem::path screenshot_path = std::filesystem::path("screenshots") / file_path;
    screenshot_path.replace_extension(".png");

    screenshot_file_path_ = screenshot_path.string();
    screenshot_requested_ = true;
    screenshot_capture_ui_ = capture_ui;
    screenshot_completion_ = std::move(on_complete);
}

bool Renderer::ReadbackFinalOutputIfRequested(RHIRenderTarget *final_output, bool capture_ui,
                                              RHIPipelineStage after_stage)
{
    ASSERT(ThreadManager::IsInRenderThread());
    ASSERT(final_output);

    if (!screenshot_requested_)
    {
        return false;
    }

    if (screenshot_capture_ui_ != capture_ui)
    {
        return false;
    }

    screenshot_requested_ = false;
    screenshot_capture_ui_ = false;
    auto output_path = screenshot_file_path_;
    screenshot_file_path_.clear();
    auto completion = std::move(screenshot_completion_);

    // just assume color image 0 is the final output.
    auto color_image = final_output->GetColorImage(0);

    ASSERT(color_image);

    auto staging_buffer =
        rhi_->CreateBuffer({.size = color_image->GetStorageSize(),
                            .usages = RHIBuffer::BufferUsage::TransferDst,
                            .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                            .is_dynamic = false},
                           "ScreenshotReadbackStagingBuffer");

    color_image->Transition({.target_layout = RHIImageLayout::TransferSrc,
                             .after_stage = after_stage,
                             .before_stage = RHIPipelineStage::Transfer});

    color_image->CopyToBuffer(staging_buffer);

    // we do not transition the image back. only the caller knows what to do with the image.

    const auto width = color_image->GetWidth();
    const auto height = color_image->GetHeight();
    const auto format = color_image->GetAttributes().format;
    auto *rhi = rhi_;

    rhi_->EnqueueEndOfFrameTasks(
        [rhi, staging_buffer, width, height, format, output_path, on_complete = std::move(completion)]() {
            rhi->WaitForDeviceIdle();

            const auto *raw_data = reinterpret_cast<const uint8_t *>(staging_buffer->Lock());
            auto screenshot = Image2D::CreateFromRawPixels(raw_data, width, height, format);
            staging_buffer->UnLock();
            bool success = screenshot.WriteToFile(output_path);

            if (success)
            {
                Log(Info, "Screenshot saved to {}", output_path);
            }
            else
            {
                Log(Error, "Failed to save screenshot to {}", output_path);
            }

            if (on_complete)
            {
                on_complete();
            }
        });

    return true;
}
} // namespace sparkle
