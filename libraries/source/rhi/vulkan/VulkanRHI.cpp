#if ENABLE_VULKAN

#include "rhi/VulkanRHI.h"

#include "VulkanBuffer.h"
#include "VulkanCommon.h"
#include "VulkanContext.h"
#include "VulkanImage.h"
#include "VulkanNrdBackend.h"
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

#include <string_view>

namespace sparkle
{
constexpr unsigned HeadlessFramesInFlight = 2;

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

bool VulkanRHI::BeginFrameInternal()
{
    if (!context->BeginFrame())
    {
        return false;
    }

    if (GetConfig().measure_gpu_time && !frame_timers_.empty())
    {
        auto frame_index = GetFrameIndex();
        if (frame_timers_[frame_index]->GetStatus() != RHITimer::Status::Inactive)
        {
            frame_stats_[frame_index].elapsed_time_ms = frame_timers_[frame_index]->GetTime();
        }

        frame_timers_[frame_index]->Begin(*context->GetCommandContext());
    }

    return true;
}

void VulkanRHI::EndFrameInternal()
{
    if (GetConfig().measure_gpu_time && !frame_timers_.empty())
    {
        frame_timers_[GetFrameIndex()]->End(*context->GetCommandContext());
    }

    auto result = context->EndFrame();

    if (result == VK_ERROR_SURFACE_LOST_KHR)
    {
        Log(Info, "Vulkan surface lost while presenting. Waiting for a new native window...");
        back_buffer_dirty_ = true;
        frame_buffer_resized_ = false;
        return;
    }

    const bool surface_result =
        result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR;
    if (!surface_result)
    {
        CHECK_VK_ERROR(result);
        return;
    }

    if (!IsHeadless() && !GetHardwareInterface()->CanRender())
    {
        back_buffer_dirty_ = true;
        frame_buffer_resized_ = false;
        return;
    }

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

    if (SupportsPassTimestamps())
    {
        for (unsigned i = 0; i < GetMaxFramesInFlight(); i++)
        {
            frame_timers_.emplace_back(CreateTimer("FrameTimer"));
        }
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

bool VulkanRHI::SupportsPixelLocalRead()
{
    return context->SupportsDynamicRenderingLocalRead();
}

bool VulkanRHI::SupportsUnifiedImageLayouts()
{
    return context->SupportsUnifiedImageLayouts();
}

bool VulkanRHI::SupportsPassTimestamps()
{
    return context->GetTimestampValidBits() > 0;
}

bool VulkanRHI::HasPhysicalGpu()
{
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(context->GetPhysicalDevice(), &properties);
    return properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU;
}

bool VulkanRHI::SupportsSampledFormat(PixelFormat format)
{
    if (format == PixelFormat::ASTC4x4HDR && !context->SupportsAstcHdr())
    {
        return false;
    }

    if (IsCompressedFormat(format))
    {
        // the Apple Paravirtual device (macOS CI runners) advertises compressed formats
        // through MoltenVK but its buffer-to-image copies produce blank texels; the same
        // device handles them correctly through native Metal shared-storage uploads
        VkPhysicalDeviceProperties device_properties;
        vkGetPhysicalDeviceProperties(context->GetPhysicalDevice(), &device_properties);
        if (std::string_view(device_properties.deviceName).find("Paravirtual") != std::string_view::npos)
        {
            return false;
        }
    }

    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(context->GetPhysicalDevice(), GetVkPixelFormat(format), &properties);

    constexpr VkFormatFeatureFlags Required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                              VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (properties.optimalTilingFeatures & Required) == Required;
}

uint32_t VulkanRHI::GetMinBufferOffsetAlignment() const
{
    return context->GetMinBufferOffsetAlignment();
}

std::unique_ptr<RHINrdBackend> VulkanRHI::CreateNrdBackend()
{
    // ReBLUR's HistoryFix uses subgroup quad swaps in compute
    if (!context->SupportsSubgroupQuadOps())
    {
        Log(Error, "VulkanNrdBackend: device lacks subgroup quad operations in compute");
        return nullptr;
    }

    return std::make_unique<VulkanNrdBackend>();
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
        color_attribute.format = PixelFormat::B8G8R8A8Srgb;
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
        back_buffer_rt_ = CreateBackBufferRenderTarget({}, CreateBackBufferDepth(context->GetSwapChain()->GetExtent()),
                                                       "BackBufferRT");
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

RHICommandContext *VulkanRHI::GetCommandContext()
{
    return context->GetCommandContext();
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
    return CreateResource<VulkanRenderPass>(this, attribute, rt, name);
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
    return CreateResource<RHIComputePass>(this, need_timestamp, name);
}
} // namespace sparkle

#endif
