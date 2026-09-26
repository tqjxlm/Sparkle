#if ENABLE_VULKAN

#include "rhi/VulkanRHI.h"

#include "VulkanBuffer.h"
#include "VulkanCommon.h"
#include "VulkanComputePass.h"
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

static std::vector<RHIResourceWeakRef<VulkanRenderPass>> render_passes;

constexpr VkAccessFlags2 WriteAccessFlags = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;

struct VulkanAccessScope
{
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
};

static VulkanAccessScope GetVulkanAccessScope(const RHIResourceAccess &rhi_access)
{
    VkPipelineStageFlags2 shader_stages = VK_PIPELINE_STAGE_2_NONE;
    if (rhi_access.stages & RHIShaderStageMask::Vertex)
    {
        shader_stages |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    }
    if (rhi_access.stages & RHIShaderStageMask::Pixel)
    {
        shader_stages |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    }
    if (rhi_access.stages & RHIShaderStageMask::Compute)
    {
        shader_stages |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    }

    VulkanAccessScope scope;
    auto add = [&rhi_access, &scope](RHIAccess access, VkPipelineStageFlags2 stages, VkAccessFlags2 access_flags) {
        if (rhi_access.access & access)
        {
            ASSERT_F(stages != VK_PIPELINE_STAGE_2_NONE, "shader access {} has no shader stage",
                     static_cast<unsigned>(access));
            scope.stages |= stages;
            scope.access |= access_flags;
        }
    };

    constexpr VkPipelineStageFlags2 FragmentTests =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

    add(RHIAccess::ColorWrite, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    add(RHIAccess::DepthWrite, FragmentTests,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    add(RHIAccess::DepthTest, FragmentTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    add(RHIAccess::Sampled, shader_stages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    add(RHIAccess::StorageRead, shader_stages, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    add(RHIAccess::StorageWrite, shader_stages, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    add(RHIAccess::CopySrc, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    add(RHIAccess::CopyDst, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    add(RHIAccess::Uniform, shader_stages, VK_ACCESS_2_UNIFORM_READ_BIT);
    add(RHIAccess::VertexInput, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
    add(RHIAccess::IndexInput, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT);
    add(RHIAccess::IndirectArgs, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    // shader read covers the geometry and instance inputs of the build
    add(RHIAccess::AccelerationStructureBuild, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
            VK_ACCESS_2_SHADER_READ_BIT);
    add(RHIAccess::AccelerationStructureRead, shader_stages, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    add(RHIAccess::HostRead, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);

    return scope;
}

template <typename VkBarrier>
static void SetVulkanAccessScopes(VkBarrier &vk_barrier, const RHIResourceAccess &from, const RHIResourceAccess &to)
{
    const auto src = GetVulkanAccessScope(from);
    const auto dst = GetVulkanAccessScope(to);
    vk_barrier.srcStageMask = src.stages;
    vk_barrier.srcAccessMask = src.access & WriteAccessFlags;
    vk_barrier.dstStageMask = dst.stages;
    vk_barrier.dstAccessMask = dst.access;
}

static VkPipelineStageFlags GetSync1Stages(VkPipelineStageFlags2 stages, VkPipelineStageFlags none_stage)
{
    auto sync1_stages = static_cast<VkPipelineStageFlags>(stages);
    if (stages & (VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT))
    {
        sync1_stages |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    }
    return sync1_stages ? sync1_stages : none_stage;
}

static VkAccessFlags GetSync1Access(VkAccessFlags2 access)
{
    auto sync1_access = static_cast<VkAccessFlags>(access);
    if (access & (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT))
    {
        sync1_access |= VK_ACCESS_SHADER_READ_BIT;
    }
    if (access & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)
    {
        sync1_access |= VK_ACCESS_SHADER_WRITE_BIT;
    }
    return sync1_access;
}

static void RecordSync1ImageBarriers(VkCommandBuffer command_buffer, std::span<const VkImageMemoryBarrier2> barriers)
{
    VkPipelineStageFlags src_stages = 0;
    VkPipelineStageFlags dst_stages = 0;
    std::vector<VkImageMemoryBarrier> sync1_barriers;
    sync1_barriers.reserve(barriers.size());
    for (const auto &barrier : barriers)
    {
        src_stages |= GetSync1Stages(barrier.srcStageMask, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
        dst_stages |= GetSync1Stages(barrier.dstStageMask, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        sync1_barriers.push_back({.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  .pNext = nullptr,
                                  .srcAccessMask = GetSync1Access(barrier.srcAccessMask),
                                  .dstAccessMask = GetSync1Access(barrier.dstAccessMask),
                                  .oldLayout = barrier.oldLayout,
                                  .newLayout = barrier.newLayout,
                                  .srcQueueFamilyIndex = barrier.srcQueueFamilyIndex,
                                  .dstQueueFamilyIndex = barrier.dstQueueFamilyIndex,
                                  .image = barrier.image,
                                  .subresourceRange = barrier.subresourceRange});
    }

    vkCmdPipelineBarrier(command_buffer, src_stages, dst_stages, 0, 0, nullptr, 0, nullptr,
                         static_cast<uint32_t>(sync1_barriers.size()), sync1_barriers.data());
}

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

    if (GetConfig().measure_gpu_time)
    {
        auto frame_index = GetFrameIndex();
        if (frame_timers_[frame_index]->GetStatus() != RHITimer::Status::Inactive)
        {
            frame_stats_[frame_index].elapsed_time_ms = frame_timers_[frame_index]->GetTime();
        }

        frame_timers_[frame_index]->Begin();
    }

    return true;
}

void VulkanRHI::EndFrameInternal()
{
    if (GetConfig().measure_gpu_time)
    {
        frame_timers_[GetFrameIndex()]->End();
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

bool VulkanRHI::SupportsPixelLocalRead()
{
    return context->SupportsDynamicRenderingLocalRead();
}

bool VulkanRHI::SupportsUnifiedImageLayouts()
{
    return context->SupportsUnifiedImageLayouts();
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

void VulkanRHI::Barrier(std::span<const RHIImageBarrier> image_barriers,
                        std::span<const RHIMemoryBarrier> memory_barriers)
{
    if (image_barriers.empty() && memory_barriers.empty())
    {
        return;
    }

    ASSERT(context->GetCurrentCommandBuffer());
    ASSERT_F(!current_render_pass_, "Barrier inside render pass {}", current_render_pass_->GetName());

    std::vector<VkImageMemoryBarrier2> vk_image_barriers;
    std::vector<VkImageMemoryBarrier2> compressed_image_barriers;
    vk_image_barriers.reserve(image_barriers.size());
    for (const auto &barrier : image_barriers)
    {
        const auto *image = RHICast<VulkanImage>(barrier.image);

        auto &vk_barrier =
            (context->CompressedImageBarriersNeedSync1() && IsCompressedFormat(image->GetAttributes().format)
                 ? compressed_image_barriers
                 : vk_image_barriers)
                .emplace_back(VkImageMemoryBarrier2{});
        vk_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        SetVulkanAccessScopes(vk_barrier, barrier.from, barrier.to);
        vk_barrier.oldLayout = GetVulkanImageLayout(barrier.from_layout);
        vk_barrier.newLayout = GetVulkanImageLayout(barrier.to_layout);
        vk_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vk_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vk_barrier.image = image->GetImage();
        vk_barrier.subresourceRange = {.aspectMask = image->GetAspect(),
                                       .baseMipLevel = barrier.base_mip,
                                       .levelCount = barrier.mip_count,
                                       .baseArrayLayer = barrier.base_array_layer,
                                       .layerCount = barrier.array_layer_count};
    }

    std::vector<VkMemoryBarrier2> vk_memory_barriers;
    vk_memory_barriers.reserve(memory_barriers.size());
    for (const auto &barrier : memory_barriers)
    {
        auto &vk_barrier = vk_memory_barriers.emplace_back(VkMemoryBarrier2{});
        vk_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        SetVulkanAccessScopes(vk_barrier, barrier.from, barrier.to);
    }

    if (!compressed_image_barriers.empty())
    {
        RecordSync1ImageBarriers(context->GetCurrentCommandBuffer(), compressed_image_barriers);
        if (vk_image_barriers.empty() && vk_memory_barriers.empty())
        {
            return;
        }
    }

    VkDependencyInfo dependency_info{};
    dependency_info.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency_info.memoryBarrierCount = static_cast<uint32_t>(vk_memory_barriers.size());
    dependency_info.pMemoryBarriers = vk_memory_barriers.data();
    dependency_info.imageMemoryBarrierCount = static_cast<uint32_t>(vk_image_barriers.size());
    dependency_info.pImageMemoryBarriers = vk_image_barriers.data();

    vkCmdPipelineBarrier2(context->GetCurrentCommandBuffer(), &dependency_info);
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

    context->BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

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

    context->BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline->GetPipeline());

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
