#if ENABLE_VULKAN

#include "rhi/VulkanRHI.h"

#include "VulkanBuffer.h"
#include "VulkanCommon.h"
#include "VulkanComputePass.h"
#include "VulkanContext.h"
#include "VulkanImage.h"
#include "VulkanPipelineState.h"
#include "VulkanRayTracing.h"
#include "VulkanRenderPass.h"
#include "VulkanRenderTarget.h"
#include "VulkanResourceArray.h"
#include "VulkanShader.h"
#include "VulkanSwapChain.h"
#include "VulkanTimer.h"
#include "VulkanUi.h"
#include "application/NativeView.h"
#include "core/Logger.h"

namespace sparkle
{
constexpr unsigned HeadlessFramesInFlight = 2;

static std::vector<RHIResourceWeakRef<VulkanRenderPass>> render_passes;

void VulkanRHI::WaitForDeviceIdle()
{
    CHECK_VK_ERROR(vkDeviceWaitIdle(context->GetDevice()));
}

void VulkanRHI::CleanupInternal()
{
    Log(Info, "Cleanup Vulkan RHI");

    DestroySurface();

    context->Cleanup();

    context = nullptr;

    initialization_success_ = false;
}

void VulkanRHI::BeginFrameInternal()
{
    context->BeginFrame();

    if (GetConfig().measure_gpu_time)
    {
        auto frame_index = GetFrameIndex();
        if (frame_timers_[frame_index]->GetStatus() != RHITimer::Status::Inactive)
        {
            frame_stats_[frame_index].elapsed_time_ms = frame_timers_[frame_index]->GetTime();
        }

        frame_timers_[frame_index]->Begin();
    }
}

void VulkanRHI::EndFrameInternal()
{
    if (GetConfig().measure_gpu_time)
    {
        frame_timers_[GetFrameIndex()]->End();
    }

    auto result = context->EndFrame();

    bool should_recreate_swapchain = !IsHeadless() && (result == VK_ERROR_OUT_OF_DATE_KHR || frame_buffer_resized_);

    if (!IsHeadless() && result == VK_SUBOPTIMAL_KHR)
    {
        if (GetConfig().enable_pre_transform)
        {
            should_recreate_swapchain = true;
        }
        else
        {
            // if we decide not to use pretransform, ignore VK_SUBOPTIMAL_KHR
            result = VK_SUCCESS;
        }
    }

    if (should_recreate_swapchain)
    {
        frame_buffer_resized_ = false;
        RecreateSwapChain();
    }
    else
    {
        CHECK_VK_ERROR(result);
        back_buffer_dirty_ = false;
    }
}

bool VulkanRHI::InitRHI(NativeView *inWindow, std::string &error)
{
    if (!RHIContext::InitRHI(inWindow, error))
    {
        return false;
    }

    context = std::make_unique<VulkanContext>(this);

    initialization_success_ = context->Init();
    if (!initialization_success_)
    {
        error = "Unable to initialize with given app config. Please run with API validation in Debug build to get "
                "more info.";
    }

    return initialization_success_;
}

void VulkanRHI::InitRenderResources()
{
    Log(Debug, "Init vulkan resources");

    context->InitRenderResources();

    for (unsigned i = 0; i < GetMaxFramesInFlight(); i++)
    {
        frame_timers_.emplace_back(CreateTimer("FrameTimer"));
    }
}

void VulkanRHI::DestroySurface()
{
    context->DestroySurface();
}

void VulkanRHI::ReleaseRenderResources()
{
    RHIContext::ReleaseRenderResources();

    frame_timers_.clear();

    context->ReleaseRenderResources();
}

bool VulkanRHI::SupportsHardwareRayTracing()
{
    return context->SupportsHardwareRayTracing();
}

void VulkanRHI::RecreateSwapChain()
{
    if (IsHeadless())
    {
        Log(Warn, "RecreateSwapChain is ignored in headless mode.");
        return;
    }

    Log(Info, "Recreating swap chain, frame index {}", total_frame_);

    WaitForDeviceIdle();

    ReleaseRenderResources();

    CreateBackBufferRenderTarget();

    InitRenderResources();

    for (const auto &render_pass_ptr : render_passes)
    {
        if (render_pass_ptr.expired())
        {
            continue;
        }

        auto render_pass = render_pass_ptr.lock();

        if (render_pass->RequireBackBuffer())
        {
            render_pass->Cleanup();
            render_pass->Init(back_buffer_rt_);
        }
    }
}

bool VulkanRHI::RecreateSurface()
{
    return context->RecreateSurface();
}

static auto CreateBackBufferDepth(VkExtent2D extent)
{
    auto depth_format = FindDepthFormat(context->GetPhysicalDevice());

    RHIImage::Attribute attribute;
    attribute.width = extent.width;
    attribute.height = extent.height;
    attribute.mip_levels = 1;
    attribute.msaa_samples = 1;
    attribute.usages = RHIImage::ImageUsage::DepthStencilAttachment | RHIImage::ImageUsage::TransientAttachment;
    attribute.sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                         .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                         .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                         .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest};

    return context->GetRHI()->CreateResource<VulkanImage>(attribute, depth_format, "BackBufferDepth");
}

void VulkanRHI::CreateBackBufferRenderTarget()
{
    ASSERT(!back_buffer_rt_);

    if (IsHeadless())
    {
        int width = 0;
        int height = 0;
        GetHardwareInterface()->GetFrameBufferSize(width, height);
        ASSERT_F(width > 0 && height > 0, "Invalid headless render size [{}, {}]", width, height);

        VkExtent2D extent{.width = static_cast<uint32_t>(width), .height = static_cast<uint32_t>(height)};
        SetMaxFramesInFlight(HeadlessFramesInFlight);

        RHIImage::Attribute color_attribute;
        color_attribute.format = PixelFormat::B8G8R8A8_SRGB;
        color_attribute.width = extent.width;
        color_attribute.height = extent.height;
        color_attribute.mip_levels = 1;
        color_attribute.msaa_samples = 1;
        color_attribute.usages = RHIImage::ImageUsage::ColorAttachment;
        color_attribute.sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
                                   .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                                   .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                                   .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest};

        auto color_image = CreateImage(color_attribute, "HeadlessBackBufferColor");
        auto depth_image = CreateBackBufferDepth(extent);

        RHIRenderTarget::ColorImageArray color_images{};
        color_images[0] = color_image;
        back_buffer_rt_ = CreateRenderTarget({}, color_images, depth_image, "HeadlessBackBufferRT");
    }
    else
    {
        context->RecreateSwapChain();
        back_buffer_rt_ =
            CreateBackBufferRenderTarget({}, CreateBackBufferDepth(context->GetSwapChain()->GetExtent()), "BackBufferRT");
    }

    back_buffer_dirty_ = true;
}

RHIResourceRef<RHIBuffer> VulkanRHI::CreateBuffer(const RHIBuffer::Attribute &attribute, const std::string &name)
{
    return CreateResource<VulkanBuffer>(attribute, name);
}

void VulkanRHI::BeginCommandBuffer()
{
    context->BeginCommandBuffer();
};

void VulkanRHI::SubmitCommandBuffer()
{
    context->SubmitCommandBuffer();
}

RHIResourceRef<RHIImage> VulkanRHI::CreateImage(const RHIImage::Attribute &attributes, const std::string &name)
{
    return CreateResource<VulkanImage>(attributes, VK_FORMAT_UNDEFINED, name);
}

RHIResourceRef<RHIImageView> VulkanRHI::CreateImageView(RHIImage *image, const RHIImageView::Attribute &attribute)
{
    return CreateResource<VulkanImageView>(attribute, image);
}

RHIResourceRef<RHIBLAS> VulkanRHI::CreateBLAS(const TransformMatrix &transform,
                                              const RHIResourceRef<RHIBuffer> &vertex_buffer,
                                              const RHIResourceRef<RHIBuffer> &index_buffer, uint32_t num_primitive,
                                              uint32_t num_vertex, const std::string &name)
{
    return CreateResource<VulkanBLAS>(transform, vertex_buffer, index_buffer, num_primitive, num_vertex, name);
}

RHIResourceRef<RHITLAS> VulkanRHI::CreateTLAS(const std::string &name)
{
    return CreateResource<VulkanTLAS>(name);
}

RHIResourceRef<RHIUiHandler> VulkanRHI::CreateUiHandler()
{
    return CreateResource<VulkanUiHandler>();
}

RHIResourceRef<RHISampler> VulkanRHI::CreateSampler(RHISampler::SamplerAttribute attribute, const std::string &name)
{
    return CreateResource<VulkanSampler>(attribute, name);
}

RHIResourceRef<RHIRenderTarget> VulkanRHI::CreateBackBufferRenderTarget(const RHIRenderTarget::Attribute &attribute,
                                                                        const RHIResourceRef<RHIImage> &depth_image,
                                                                        const std::string &name)
{
    return CreateResource<VulkanRenderTarget>(attribute, depth_image, name);
}

RHIResourceRef<RHIRenderTarget> VulkanRHI::CreateRenderTarget(const RHIRenderTarget::Attribute &attribute,
                                                              const RHIRenderTarget::ColorImageArray &color_images,
                                                              const RHIResourceRef<RHIImage> &depth_image,
                                                              const std::string &name)
{
    return CreateResource<VulkanRenderTarget>(attribute, color_images, depth_image, name);
}

RHIResourceRef<RHIRenderPass> VulkanRHI::CreateRenderPass(const RHIRenderPass::Attribute &attribute,
                                                          const RHIResourceRef<RHIRenderTarget> &rt,
                                                          const std::string &name)
{
    auto render_pass = CreateResource<VulkanRenderPass>(attribute, rt, name);
    render_passes.emplace_back(render_pass);
    return render_pass;
}

RHIResourceRef<RHIShader> VulkanRHI::CreateShader(const RHIShaderInfo *shader_info)
{
    return CreateResource<VulkanShader>(shader_info);
}

RHIResourceRef<RHIPipelineState> VulkanRHI::CreatePipelineState(RHIPipelineState::PipelineType type,
                                                                const std::string &name)
{
    switch (type)
    {
    case RHIPipelineState::PipelineType::Graphics:
        return CreateResource<VulkanForwardPipelineState>(type, name);
    case RHIPipelineState::PipelineType::Compute:
        return CreateResource<VulkanComputePipelineState>(type, name);
    default:
        UnImplemented(type);
    }
}

void VulkanRHI::DrawMesh(const RHIResourceRef<RHIPipelineState> &pipeline_state, const DrawArgs &draw_args)
{
    if (!pipeline_state)
    {
        return;
    }

    VkCommandBuffer command_buffer = context->GetCurrentCommandBuffer();

    const auto &rhi_pipeline = RHICast<VulkanForwardPipelineState>(pipeline_state);
    VkPipeline pipeline = rhi_pipeline->GetPipeline();

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    rhi_pipeline->SetViewportAndScissor();
    rhi_pipeline->BindBuffers();
    rhi_pipeline->BindDescriptorSets();

    vkCmdDrawIndexed(command_buffer, draw_args.index_count, draw_args.instance_count, draw_args.first_index,
                     static_cast<int>(draw_args.first_vertex), draw_args.first_instance);
}

void VulkanRHI::DispatchCompute(const RHIResourceRef<RHIPipelineState> &pipeline, Vector3UInt total_threads,
                                Vector3UInt thread_per_group)
{
    VkCommandBuffer command_buffer = context->GetCurrentCommandBuffer();

    auto *compute_pipeline = RHICast<VulkanComputePipelineState>(pipeline);

    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline->GetPipeline());

    compute_pipeline->BindDescriptorSets();

    unsigned group_count_x = utilities::DivideAndRoundUp(total_threads.x(), thread_per_group.x());
    unsigned group_count_y = utilities::DivideAndRoundUp(total_threads.y(), thread_per_group.y());
    unsigned group_count_z = utilities::DivideAndRoundUp(total_threads.z(), thread_per_group.z());

    vkCmdDispatch(command_buffer, group_count_x, group_count_y, group_count_z);
}

void VulkanRHI::BeginRenderPassInternal(const RHIResourceRef<RHIRenderPass> &pass)
{
    auto *rhi_render_pass = RHICast<VulkanRenderPass>(pass);
    rhi_render_pass->Begin();
}

void VulkanRHI::EndRenderPassInternal()
{
    auto *rhi_render_pass = RHICast<VulkanRenderPass>(current_render_pass_);
    rhi_render_pass->End();
}

RHIResourceRef<RHIResourceArray> VulkanRHI::CreateResourceArray(RHIShaderResourceReflection::ResourceType type,
                                                                unsigned int capacity, const std::string &name)
{
    return CreateResource<VulkanResourceArray>(type, capacity, name);
}

RHIResourceRef<RHITimer> VulkanRHI::CreateTimer(const std::string &name)
{
    return CreateResource<VulkanTimer>(name);
}

RHIResourceRef<RHIComputePass> VulkanRHI::CreateComputePass(const std::string &name, bool need_timestamp)
{
    return CreateResource<VulkanComputePass>(this, need_timestamp, name);
}

void VulkanRHI::BeginComputePassInternal(const RHIResourceRef<RHIComputePass> &pass)
{
    auto *rhi_compute_pass = RHICast<VulkanComputePass>(pass);
    rhi_compute_pass->Begin();
}

void VulkanRHI::EndComputePassInternal(const RHIResourceRef<RHIComputePass> &pass)
{
    auto *rhi_compute_pass = RHICast<VulkanComputePass>(pass);
    rhi_compute_pass->End();
}
} // namespace sparkle

#endif
