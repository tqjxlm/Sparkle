#if ENABLE_VULKAN

#include "VulkanRenderPass.h"

#include "VulkanCommon.h"
#include "VulkanContext.h"
#include "VulkanDescriptorSetManager.h"
#include "VulkanImage.h"
#include "VulkanRenderTarget.h"
#include "VulkanSwapChain.h"
#include "core/Exception.h"

namespace sparkle
{
void VulkanRenderPass::CreateRenderPass()
{
    auto *rhi_rt = RHICast<VulkanRenderTarget>(GetRenderTarget());
    const auto &rt_attrib = rhi_rt->GetAttribute();

    ASSERT_F(rhi_rt->GetExtent().width > 0 && rhi_rt->GetExtent().height > 0, "render target size invalid {}",
             rhi_rt->GetName());

    auto msaa_samples = rt_attrib.msaa_samples;
    const VkImageLayout final_layout = GetVulkanImageLayout(attribute_.color_final_layout);

    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> color_attachment_refs;
    std::vector<VkAttachmentReference> resolve_attachment_refs;

    // we treat all color attachments with the same attribute
    // TODO(tqjxlm): maybe support different attributes for different attachments
    for (auto i = 0u; i < RHIRenderTarget::MaxNumColorImage; i++)
    {
        if (!rhi_rt->GetColorImage(i))
        {
            continue;
        }

        auto &color_attachment_ref = color_attachment_refs.emplace_back(VkAttachmentReference{});
        color_attachment_ref.attachment = static_cast<uint32_t>(attachments.size());
        color_attachment_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        auto &color_attachment = attachments.emplace_back(VkAttachmentDescription{});

        color_attachment.format = rhi_rt->GetFormat(i);
        color_attachment.samples = GetVkMsaaSampleBit(msaa_samples);
        color_attachment.loadOp = GetAttachmentLoadOp(attribute_.color_load_op);
        color_attachment.storeOp = GetAttachmentStoreOp(attribute_.color_store_op);
        color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color_attachment.initialLayout = GetVulkanImageLayout(attribute_.color_initial_layout);

        if (msaa_samples > 1)
        {
            color_attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        else
        {
            color_attachment.finalLayout = final_layout;
        }

        // for msaa resolve
        if (msaa_samples > 1)
        {
            auto &resolve_attachment_ref = resolve_attachment_refs.emplace_back(VkAttachmentReference{});
            resolve_attachment_ref.attachment = static_cast<uint32_t>(attachments.size());
            resolve_attachment_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            auto &resolve_attachment = attachments.emplace_back(VkAttachmentDescription{});

            resolve_attachment.format = rhi_rt->GetFormat(i);
            resolve_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            resolve_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            resolve_attachment.storeOp = GetAttachmentStoreOp(attribute_.color_store_op);
            resolve_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            resolve_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            resolve_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            resolve_attachment.finalLayout = final_layout;
        }
    }

    VkAttachmentReference depth_attachment_ref{};
    if (rhi_rt->GetDepthImage())
    {
        depth_attachment_ref.attachment = static_cast<uint32_t>(attachments.size());
        depth_attachment_ref.layout = GetVulkanImageLayout(attribute_.depth_final_layout);

        auto &depth_attachment = attachments.emplace_back(VkAttachmentDescription{});

        depth_attachment.format = FindDepthFormat(context->GetPhysicalDevice());
        depth_attachment.samples = GetVkMsaaSampleBit(msaa_samples);
        depth_attachment.loadOp = GetAttachmentLoadOp(attribute_.depth_load_op);
        depth_attachment.storeOp = GetAttachmentStoreOp(attribute_.depth_store_op);
        depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_attachment.initialLayout = GetVulkanImageLayout(attribute_.depth_initial_layout);
        depth_attachment.finalLayout = GetVulkanImageLayout(attribute_.depth_final_layout);
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;

    if (!color_attachment_refs.empty())
    {
        subpass.colorAttachmentCount = static_cast<uint32_t>(color_attachment_refs.size());
        subpass.pColorAttachments = color_attachment_refs.data();
        if (!resolve_attachment_refs.empty())
        {
            subpass.pResolveAttachments = resolve_attachment_refs.data();
        }
    }

    if (rhi_rt->GetDepthImage())
    {
        subpass.pDepthStencilAttachment = &depth_attachment_ref;
    }

    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = static_cast<uint32_t>(attachments.size());
    render_pass_info.pAttachments = attachments.data();
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    render_pass_info.dependencyCount = 1;
    render_pass_info.pDependencies = &dependency;

    CHECK_VK_ERROR(vkCreateRenderPass(context->GetDevice(), &render_pass_info, nullptr, &render_pass_));

    context->SetDebugInfo(reinterpret_cast<uint64_t>(render_pass_), VK_OBJECT_TYPE_RENDER_PASS, GetName().c_str());
}

void VulkanRenderPass::Begin()
{
    ASSERT(GetRenderTarget());

    auto *command_buffer = context->GetCurrentCommandBuffer();

    VkDebugUtilsLabelEXT debug_label{};
    debug_label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    debug_label.pLabelName = GetName().c_str();

    vkCmdBeginDebugUtilsLabelEXT(command_buffer, &debug_label);

    context->GetDescriptorSetManager().UpdateDirtyResourceArrays();

    auto *rhi_rt = RHICast<VulkanRenderTarget>(GetRenderTarget());

    auto image_index = RequireBackBuffer() ? context->GetSwapChain()->GetCurrentImageIndex() : 0;

    VkRenderPassBeginInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_info.renderPass = render_pass_;
    render_pass_info.framebuffer = frame_buffers_[image_index];
    render_pass_info.renderArea.offset = {.x = 0, .y = 0};
    render_pass_info.renderArea.extent = rhi_rt->GetExtent();

    std::vector<VkClearValue> clear_values;
    for (const auto &color_image : rhi_rt->GetColorImages())
    {
        if (color_image)
        {
            clear_values.push_back({.color = {{attribute_.clear_color.x(), attribute_.clear_color.y(),
                                               attribute_.clear_color.z(), attribute_.clear_color.w()}}});
        }
    }

    if (rhi_rt->GetDepthImage())
    {
        clear_values.push_back({.depthStencil = {1.0f, 0}});
    }

    render_pass_info.clearValueCount = static_cast<uint32_t>(clear_values.size());
    render_pass_info.pClearValues = clear_values.data();

    vkCmdBeginRenderPass(context->GetCurrentCommandBuffer(), &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);
}

void VulkanRenderPass::CreateFramebuffers()
{
    auto *rhi_rt = RHICast<VulkanRenderTarget>(GetRenderTarget());

    auto frame_buffer_num = RequireBackBuffer() ? context->GetRHI()->GetMaxFramesInFlight() : 1;

    frame_buffers_.resize(frame_buffer_num);

    for (auto i = 0u; i < frame_buffer_num; i++)
    {
        auto attachments = rhi_rt->GetAttachments(i);

        VkFramebufferCreateInfo framebuffer_info{};
        framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_info.renderPass = render_pass_;
        framebuffer_info.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebuffer_info.pAttachments = attachments.data();
        framebuffer_info.width = rhi_rt->GetExtent().width;
        framebuffer_info.height = rhi_rt->GetExtent().height;
        framebuffer_info.layers = 1;

        CHECK_VK_ERROR(vkCreateFramebuffer(context->GetDevice(), &framebuffer_info, nullptr, &frame_buffers_[i]));
    }
}

void VulkanRenderPass::End()
{
    auto *command_buffer = context->GetCurrentCommandBuffer();

    vkCmdEndRenderPass(command_buffer);

    auto *rhi_rt = RHICast<VulkanRenderTarget>(GetRenderTarget());

    // layout is managed by explicit transition most of the time, except implicit transition by render passes
    for (const auto &color_image : rhi_rt->GetColorImages())
    {
        if (color_image)
        {
            // set layout manually to keep record
            // TODO(tqjxlm): record per layer
            color_image->SetCurrentLayout(attribute_.color_final_layout, 0, color_image->GetAttributes().mip_levels);
        }
    }

    // layout is managed by explicit transition most of the time, except implicit transition by render passes
    if (auto image = rhi_rt->GetDepthImage())
    {
        // set layout manually to keep record
        image->SetCurrentLayout(attribute_.depth_final_layout, 0, image->GetAttributes().mip_levels);
    }

    vkCmdEndDebugUtilsLabelEXT(command_buffer);
}

void VulkanRenderPass::Cleanup()
{
    for (auto &swap_chain_frame_buffer : frame_buffers_)
    {
        vkDestroyFramebuffer(context->GetDevice(), swap_chain_frame_buffer, nullptr);
    }
    vkDestroyRenderPass(context->GetDevice(), render_pass_, nullptr);
}

void VulkanRenderPass::Init(const RHIResourceRef<RHIRenderTarget> &rt)
{
    render_target_ = rt;

    ASSERT(GetRenderTarget());

    CreateRenderPass();

    CreateFramebuffers();
}

VulkanRenderPass::VulkanRenderPass(const RHIRenderPass::Attribute &attribute, const RHIResourceRef<RHIRenderTarget> &rt,
                                   const std::string &name)
    : RHIRenderPass(attribute, rt, name), require_back_buffer_(RHICast<VulkanRenderTarget>(rt)->IsBackBufferTarget())
{
    Init(rt);
}
} // namespace sparkle

#endif
