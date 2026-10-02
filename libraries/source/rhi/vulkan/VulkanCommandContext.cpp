#if ENABLE_VULKAN

#include "VulkanCommandContext.h"

#include "VulkanBuffer.h"
#include "VulkanContext.h"
#include "VulkanImage.h"
#include "VulkanPipelineState.h"
#include "core/math/Utilities.h"

namespace sparkle
{
// a barrier's source access mask only makes writes available; a read leaves nothing to flush, and the stage masks
// already order it before the destination
constexpr VkAccessFlags2 WriteAccessFlags =
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
    VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_MEMORY_WRITE_BIT;

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
    add(RHIAccess::PixelLocalRead, shader_stages, VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT);
    add(RHIAccess::HostRead, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    add(RHIAccess::Any, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);

    return scope;
}

template <typename VkBarrier>
static void SetVulkanAccessScopes(VkBarrier &vk_barrier, const RHIResourceAccess &from, const RHIResourceAccess &to)
{
    const auto src = GetVulkanAccessScope(from);
    const auto dst = GetVulkanAccessScope(to);
    // presentation has no stage of its own: a barrier from it starts at the acquire wait, so it chains to that wait
    vk_barrier.srcStageMask =
        from.access & RHIAccess::Present ? src.stages | VulkanContext::AcquireWaitStage : src.stages;
    vk_barrier.srcAccessMask = src.access & WriteAccessFlags;
    vk_barrier.dstStageMask = dst.stages;
    vk_barrier.dstAccessMask = dst.access;
}

// a sync1 stage or access bit has the value of its sync2 counterpart; only the sync2 bits above 32 need translating
static VkPipelineStageFlags GetSync1Stages(VkPipelineStageFlags2 stages, VkPipelineStageFlags none_stage)
{
    constexpr VkPipelineStageFlags2 VertexInputStages =
        VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
    ASSERT_F((stages & ~VertexInputStages) >> 32 == 0, "sync2 stages {:#x} have no sync1 equivalent", stages);

    auto sync1_stages = static_cast<VkPipelineStageFlags>(stages);
    if (stages & VertexInputStages)
    {
        sync1_stages |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    }
    return sync1_stages ? sync1_stages : none_stage;
}

static VkAccessFlags GetSync1Access(VkAccessFlags2 access)
{
    constexpr VkAccessFlags2 ShaderAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    ASSERT_F((access & ~ShaderAccess) >> 32 == 0, "sync2 accesses {:#x} have no sync1 equivalent", access);

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

static VkAttachmentLoadOp GetAttachmentLoadOp(RHILoadOp op)
{
    switch (op)
    {
    case RHILoadOp::DontCare:
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    case RHILoadOp::Load:
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    case RHILoadOp::Clear:
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    default:
        UnImplemented(op);
    }
}

static VkAttachmentStoreOp GetAttachmentStoreOp(RHIStoreOp op)
{
    switch (op)
    {
    case RHIStoreOp::DontCare:
        return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    case RHIStoreOp::Store:
        return VK_ATTACHMENT_STORE_OP_STORE;
    default:
        UnImplemented(op);
    }
}

static VkImageView GetAttachmentView(RHIImage *image, unsigned mip_level, unsigned array_layer)
{
    const auto view = image->GetView(context->GetRHI(), {.base_mip_level = mip_level, .base_array_layer = array_layer});
    return RHICast<VulkanImageView>(view)->GetView();
}

// lowers an RHIRenderingInfo to vkCmdBeginRendering and sets the viewport and scissor to its extent
static void BeginVulkanRendering(VulkanCommandContext &command_context, const RHIRenderingInfo &info)
{
    std::array<VkRenderingAttachmentInfo, MaxNumColorAttachments> color_infos{};
    uint32_t color_attachment_count = MinColorAttachmentCount;
    for (auto slot = 0u; slot < MaxNumColorAttachments; slot++)
    {
        const auto &attachment = info.color_attachments[slot];
        auto &color_info = color_infos[slot];
        color_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (!attachment.image)
        {
            continue;
        }

        color_attachment_count = slot + 1;

        color_info.imageView = GetAttachmentView(attachment.image, attachment.mip_level, attachment.array_layer);
        color_info.imageLayout = GetVulkanImageLayout(attachment.layout);
        color_info.loadOp = GetAttachmentLoadOp(attachment.load_op);
        color_info.storeOp = GetAttachmentStoreOp(attachment.store_op);
        color_info.clearValue.color = {{attachment.clear_color.x(), attachment.clear_color.y(),
                                        attachment.clear_color.z(), attachment.clear_color.w()}};
    }

    const auto &depth_attachment = info.depth_attachment;
    VkRenderingAttachmentInfo depth_info{};
    depth_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (depth_attachment.image)
    {
        depth_info.imageView =
            GetAttachmentView(depth_attachment.image, depth_attachment.mip_level, depth_attachment.array_layer);
        depth_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_info.loadOp = GetAttachmentLoadOp(depth_attachment.load_op);
        depth_info.storeOp = GetAttachmentStoreOp(depth_attachment.store_op);
        depth_info.clearValue.depthStencil = {.depth = depth_attachment.clear_depth, .stencil = 0};
    }

    const VkRect2D render_area{.offset = {.x = 0, .y = 0}, .extent = {.width = info.width, .height = info.height}};

    VkRenderingInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea = render_area;
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = color_attachment_count;
    rendering_info.pColorAttachments = color_infos.data();
    rendering_info.pDepthAttachment = depth_attachment.image ? &depth_info : nullptr;

    vkCmdBeginRendering(command_context.GetCommandBuffer(), &rendering_info);

    const VkViewport viewport{.x = 0.0f,
                              .y = 0.0f,
                              .width = static_cast<float>(info.width),
                              .height = static_cast<float>(info.height),
                              .minDepth = 0.0f,
                              .maxDepth = 1.0f};
    command_context.SetViewportAndScissor(viewport, render_area);
}

void VulkanCommandContext::Begin(VkCommandBuffer command_buffer)
{
    ASSERT(!command_buffer_);

    command_buffer_ = command_buffer;
    ResetCommandState();
}

void VulkanCommandContext::End()
{
    AssertOutsidePass("End of command buffer");

    command_buffer_ = VK_NULL_HANDLE;
}

VkImageMemoryBarrier2 VulkanCommandContext::GetVkImageBarrier(VkImage image, const VkImageSubresourceRange &range,
                                                              const RHIResourceAccess &from,
                                                              const RHIResourceAccess &to, RHIImageLayout from_layout,
                                                              RHIImageLayout to_layout)
{
    VkImageMemoryBarrier2 vk_barrier{};
    vk_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    SetVulkanAccessScopes(vk_barrier, from, to);
    vk_barrier.oldLayout = GetVulkanImageLayout(from_layout);
    vk_barrier.newLayout = GetVulkanImageLayout(to_layout);
    vk_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vk_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vk_barrier.image = image;
    vk_barrier.subresourceRange = range;
    return vk_barrier;
}

void VulkanCommandContext::RecordBarriers(std::span<const VkImageMemoryBarrier2> image_barriers,
                                          std::span<const VkMemoryBarrier2> memory_barriers,
                                          VkDependencyFlags dependency_flags) const
{
    VkDependencyInfo dependency_info{};
    dependency_info.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency_info.dependencyFlags = dependency_flags;
    dependency_info.memoryBarrierCount = static_cast<uint32_t>(memory_barriers.size());
    dependency_info.pMemoryBarriers = memory_barriers.data();
    dependency_info.imageMemoryBarrierCount = static_cast<uint32_t>(image_barriers.size());
    dependency_info.pImageMemoryBarriers = image_barriers.data();

    vkCmdPipelineBarrier2(command_buffer_, &dependency_info);
}

// VK_KHR_dynamic_rendering_local_read: inside the rendering, framebuffer-space stages only, by region, and each image
// in the local read layout on both sides
void VulkanCommandContext::PixelLocalBarrierInternal(std::span<const RHIImageBarrier> image_barriers)
{
    std::vector<VkImageMemoryBarrier2> vk_image_barriers;
    vk_image_barriers.reserve(image_barriers.size());
    for (const auto &barrier : image_barriers)
    {
        ASSERT(barrier.from_layout == RHIImageLayout::LocalRead && barrier.to_layout == RHIImageLayout::LocalRead);
        const auto *image = RHICast<VulkanImage>(barrier.image);
        vk_image_barriers.push_back(GetVkImageBarrier(image->GetImage(),
                                                      {.aspectMask = image->GetAspect(),
                                                       .baseMipLevel = barrier.base_mip,
                                                       .levelCount = barrier.mip_count,
                                                       .baseArrayLayer = barrier.base_array_layer,
                                                       .layerCount = barrier.array_layer_count},
                                                      barrier.from, barrier.to, barrier.from_layout,
                                                      barrier.to_layout));
    }

    RecordBarriers(vk_image_barriers, {}, VK_DEPENDENCY_BY_REGION_BIT);
}

void VulkanCommandContext::BarrierInternal(std::span<const RHIImageBarrier> image_barriers,
                                           std::span<const RHIMemoryBarrier> memory_barriers)
{
    ASSERT(command_buffer_);

    std::vector<VkImageMemoryBarrier2> vk_image_barriers;
    std::vector<VkImageMemoryBarrier2> compressed_image_barriers;
    vk_image_barriers.reserve(image_barriers.size());
    for (const auto &barrier : image_barriers)
    {
        const auto *image = RHICast<VulkanImage>(barrier.image);

        (context->CompressedImageBarriersNeedSync1() && IsCompressedFormat(image->GetAttributes().format)
             ? compressed_image_barriers
             : vk_image_barriers)
            .push_back(GetVkImageBarrier(image->GetImage(),
                                         {.aspectMask = image->GetAspect(),
                                          .baseMipLevel = barrier.base_mip,
                                          .levelCount = barrier.mip_count,
                                          .baseArrayLayer = barrier.base_array_layer,
                                          .layerCount = barrier.array_layer_count},
                                         barrier.from, barrier.to, barrier.from_layout, barrier.to_layout));
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
        RecordSync1ImageBarriers(command_buffer_, compressed_image_barriers);
        if (vk_image_barriers.empty() && vk_memory_barriers.empty())
        {
            return;
        }
    }

    RecordBarriers(vk_image_barriers, vk_memory_barriers);
}

void VulkanCommandContext::DrawMeshInternal(const RHIResourceRef<RHIPipelineState> &pipeline_state,
                                            const DrawArgs &draw_args)
{
    const auto &rhi_pipeline = RHICast<VulkanForwardPipelineState>(pipeline_state);

    BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, rhi_pipeline->GetPipeline(GetAttachmentSignature()));

    rhi_pipeline->BindBuffers(*this);
    rhi_pipeline->BindDescriptorSets(*this);

    vkCmdDrawIndexed(command_buffer_, draw_args.index_count, draw_args.instance_count, draw_args.first_index,
                     static_cast<int>(draw_args.first_vertex), draw_args.first_instance);
}

void VulkanCommandContext::DispatchComputeInternal(const RHIResourceRef<RHIPipelineState> &pipeline,
                                                   Vector3UInt total_threads, Vector3UInt thread_per_group)
{
    auto *compute_pipeline = RHICast<VulkanComputePipelineState>(pipeline);

    BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline->GetPipeline());

    compute_pipeline->BindDescriptorSets(*this);

    unsigned group_count_x = utilities::DivideAndRoundUp(total_threads.x(), thread_per_group.x());
    unsigned group_count_y = utilities::DivideAndRoundUp(total_threads.y(), thread_per_group.y());
    unsigned group_count_z = utilities::DivideAndRoundUp(total_threads.z(), thread_per_group.z());

    vkCmdDispatch(command_buffer_, group_count_x, group_count_y, group_count_z);
}

void VulkanCommandContext::CopyBufferInternal(const RHIBuffer *src, const RHIBuffer *dst)
{
    RHICast<VulkanBuffer>(src)->CopyToBuffer(*this, dst);
}

void VulkanCommandContext::CopyBufferToImageInternal(const RHIBuffer *src, const RHIImage *dst)
{
    RHICast<VulkanBuffer>(src)->CopyToImage(*this, dst);
}

void VulkanCommandContext::CopyImageToBufferInternal(const RHIImage *src, const RHIBuffer *dst)
{
    RHICast<VulkanImage>(src)->CopyToBuffer(*this, dst);
}

void VulkanCommandContext::BlitImageInternal(const RHIImage *src, const RHIImage *dst,
                                             RHISampler::FilteringMethod filter)
{
    RHICast<VulkanImage>(src)->BlitToImage(*this, dst, filter);
}

void VulkanCommandContext::BeginRenderingInternal(const RHIRenderingInfo &info, const std::string & /*name*/,
                                                  RHITimer * /*timer*/)
{
    BeginVulkanRendering(*this, info);
}

void VulkanCommandContext::EndRenderingInternal()
{
    vkCmdEndRendering(command_buffer_);
}

void VulkanCommandContext::BeginDebugLabel(const std::string &name) const
{
    if (context->SupportsDebugUtils())
    {
        VkDebugUtilsLabelEXT label{};
        label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        label.pLabelName = name.c_str();

        vkCmdBeginDebugUtilsLabelEXT(command_buffer_, &label);
    }
}

void VulkanCommandContext::EndDebugLabel() const
{
    if (context->SupportsDebugUtils())
    {
        vkCmdEndDebugUtilsLabelEXT(command_buffer_);
    }
}
} // namespace sparkle

#endif
