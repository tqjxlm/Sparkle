#if ENABLE_VULKAN

#include "VulkanPipelineState.h"

#include "VulkanBuffer.h"
#include "VulkanCommon.h"
#include "VulkanContext.h"
#include "VulkanDescriptorSetManager.h"
#include "VulkanRenderPass.h"
#include "VulkanRenderTarget.h"
#include "VulkanShader.h"

namespace sparkle
{
void VulkanPipelineState::SetupDescriptorSetLayouts()
{
    // merge descriptor sets used by all stages of shaders
    for (const auto &shader : shaders_)
    {
        if (!shader)
        {
            continue;
        }
        ASSERT(shader->IsValid());

        auto stage = shader->GetInfo()->GetStage();

        auto *rhi_shader = RHICast<VulkanShader>(shader);
        auto *shader_resources = GetResourceTable(stage);

        rhi_shader->SetupShaderReflection(shader_resources);
        shader_resources->Initialize();

        const auto &stage_resource_sets = shader_resources->GetResourceSets();
        combined_resource_sets_.resize(std::max(combined_resource_sets_.size(), stage_resource_sets.size()));
    }

    for (const auto &shader : shaders_)
    {
        if (!shader)
        {
            continue;
        }

        auto stage = shader->GetInfo()->GetStage();

        auto *shader_resources = GetResourceTable(stage);

        const auto &stage_resource_sets = shader_resources->GetResourceSets();

        for (auto set_id = 0u; set_id < stage_resource_sets.size(); set_id++)
        {
            combined_resource_sets_[set_id].MergeWith(stage_resource_sets[set_id]);
        }
    }

    auto descriptor_set_count = combined_resource_sets_.size();
    descriptor_sets_.resize(descriptor_set_count);
    descriptor_layouts_.resize(descriptor_set_count);

    for (auto set_id = 0u; set_id < descriptor_set_count; set_id++)
    {
        auto &resource_set = combined_resource_sets_[set_id];
        resource_set.UpdateLayoutHash();
        descriptor_layouts_[set_id] = context->GetDescriptorSetManager().RequestLayout(resource_set);
        descriptor_sets_[set_id].SetLayoutHash(resource_set.GetLayoutHash());
    }
}

void VulkanPipelineState::SetupShaderStageInfo()
{
    shader_stages_.clear();
    for (auto i = 0u; i < static_cast<int>(RHIShaderStage::Count); i++)
    {
        auto *shader = RHICast<VulkanShader>(shaders_[i]);
        if (shader)
        {
            if (!shader->IsValid())
            {
                shader->Load();
            }

            VkPipelineShaderStageCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            info.pNext = nullptr;

            info.stage = GetShaderStage(static_cast<RHIShaderStage>(i));
            info.module = shader->GetShaderModule();
            info.pName = shader->GetInfo()->GetEntryPoint().c_str();

            shader_stages_.push_back(info);
        }
    }
}

void VulkanPipelineState::SetupPipelineLayoutInfo()
{
    VkPipelineLayoutCreateInfo pipeline_layout_create_info = {};
    pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_create_info.pNext = nullptr;

    // empty defaults
    pipeline_layout_create_info.flags = 0;
    pipeline_layout_create_info.setLayoutCount = static_cast<uint32_t>(descriptor_layouts_.size());
    pipeline_layout_create_info.pSetLayouts = descriptor_layouts_.data();
    pipeline_layout_create_info.pushConstantRangeCount = 0;
    pipeline_layout_create_info.pPushConstantRanges = nullptr;

    CHECK_VK_ERROR(
        vkCreatePipelineLayout(context->GetDevice(), &pipeline_layout_create_info, nullptr, &pipeline_layout_));
}

void VulkanForwardPipelineState::SetupVertexInputInfo()
{
    vertex_input_description_.Clear();

    const auto &bindings = vertex_input_declaration_.GetBindings();
    for (auto binding_idx = 0u; binding_idx < bindings.size(); binding_idx++)
    {
        const auto &attribute_binding = bindings[binding_idx];

        VkVertexInputBindingDescription binding_description = {};
        binding_description.binding = binding_idx;
        binding_description.stride = attribute_binding.stride;
        binding_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        vertex_input_description_.bindings.push_back(binding_description);
    }

    const auto &attributes = vertex_input_declaration_.GetAttributes();
    for (auto location = 0u; location < attributes.size(); location++)
    {
        const auto &attribute = attributes[location];
        if (attribute.format == RHIVertexFormat::Count)
        {
            continue;
        }

        VkVertexInputAttributeDescription attribute_desc = {};
        attribute_desc.binding = attribute.binding;
        attribute_desc.location = location;
        attribute_desc.format = GetVertexAttributeFormat(attribute.format);
        attribute_desc.offset = attribute.offset;

        vertex_input_description_.attributes.push_back(attribute_desc);
    }

    vertex_input_info_ = {};
    vertex_input_info_.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input_info_.pNext = nullptr;

    vertex_input_info_.vertexBindingDescriptionCount = static_cast<uint32_t>(vertex_input_description_.bindings.size());
    vertex_input_info_.pVertexBindingDescriptions = vertex_input_description_.bindings.data();
    vertex_input_info_.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(vertex_input_description_.attributes.size());
    vertex_input_info_.pVertexAttributeDescriptions = vertex_input_description_.attributes.data();

    vertex_input_info_.flags = vertex_input_description_.flags;
}

void VulkanForwardPipelineState::CreatePipeline()
{
    VkPipelineViewportStateCreateInfo viewport_state = {};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.pNext = nullptr;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    // allow different viewport/scissor rect to share the same pipeline state
    // this is almost a must for a modern portable graphics application
    VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo pipeline_info = {};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pNext = nullptr;

    pipeline_info.stageCount = static_cast<uint32_t>(shader_stages_.size());
    pipeline_info.pStages = shader_stages_.data();
    pipeline_info.pVertexInputState = &vertex_input_info_;
    pipeline_info.pInputAssemblyState = &input_assembly_;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer_;
    pipeline_info.pMultisampleState = &multisampling_;
    pipeline_info.pColorBlendState = &color_blending_;
    pipeline_info.pDepthStencilState = &depth_stencil_;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = pipeline_layout_;
    pipeline_info.renderPass = RHICast<VulkanRenderPass>(render_pass_)->GetRenderPass();
    pipeline_info.subpass = 0;
    pipeline_info.basePipelineHandle = VK_NULL_HANDLE;

    CHECK_VK_ERROR(
        vkCreateGraphicsPipelines(context->GetDevice(), VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_));

    context->SetDebugInfo(reinterpret_cast<uint64_t>(pipeline_), VK_OBJECT_TYPE_PIPELINE, GetName().c_str());
}

void VulkanForwardPipelineState::SetupColorAndDepthAttachments()
{
    color_blend_attachments_.clear();

    for (const auto &color_image : render_pass_->GetRenderTarget()->GetColorImages())
    {
        if (!color_image)
        {
            continue;
        }

        // TODO(tqjxlm): different blend state per image

        auto &blend_attachment = color_blend_attachments_.emplace_back(VkPipelineColorBlendAttachmentState{});

        blend_attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend_attachment.blendEnable = blend_state_.enabled ? VK_TRUE : VK_FALSE;
        blend_attachment.srcColorBlendFactor = GetVulkanBlendFactor(blend_state_.color_factor_src);
        blend_attachment.dstColorBlendFactor = GetVulkanBlendFactor(blend_state_.color_factor_dst);
        blend_attachment.colorBlendOp = GetVulkanBlendOp(blend_state_.color_op);
        blend_attachment.srcAlphaBlendFactor = GetVulkanBlendFactor(blend_state_.alpha_factor_src);
        blend_attachment.dstAlphaBlendFactor = GetVulkanBlendFactor(blend_state_.alpha_factor_dst);
        blend_attachment.alphaBlendOp = GetVulkanBlendOp(blend_state_.alpha_op);
    }

    color_blending_ = {};
    color_blending_.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending_.logicOpEnable = VK_FALSE;
    color_blending_.logicOp = VK_LOGIC_OP_COPY; // Optional
    color_blending_.attachmentCount = static_cast<uint32_t>(color_blend_attachments_.size());
    color_blending_.pAttachments = color_blend_attachments_.data();
    color_blending_.blendConstants[0] = 0.0f; // Optional
    color_blending_.blendConstants[1] = 0.0f; // Optional
    color_blending_.blendConstants[2] = 0.0f; // Optional
    color_blending_.blendConstants[3] = 0.0f; // Optional

    depth_stencil_ = {};
    depth_stencil_.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil_.depthTestEnable = VK_TRUE;
    depth_stencil_.depthWriteEnable = depth_state_.write_depth ? VK_TRUE : VK_FALSE;
    depth_stencil_.depthCompareOp = GetDepthCompareOp(depth_state_);
    depth_stencil_.depthBoundsTestEnable = VK_FALSE;
    // depthStencil_.minDepthBounds = 0.0f; // Optional
    // depthStencil_.maxDepthBounds = 1.0f; // Optional
    depth_stencil_.stencilTestEnable = VK_FALSE;
    depth_stencil_.front = {}; // Optional
    depth_stencil_.back = {};  // Optional
}

void VulkanForwardPipelineState::SetupRasterizationStateInfo()
{
    rasterizer_ = {};
    rasterizer_.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer_.pNext = nullptr;

    rasterizer_.depthClampEnable = VK_FALSE;
    rasterizer_.rasterizerDiscardEnable = VK_FALSE;

    rasterizer_.polygonMode = GetPolygonMode(rasterization_state_.polygon_mode);
    rasterizer_.lineWidth = rasterization_state_.line_width;
    rasterizer_.cullMode = GetFaceCullMode(rasterization_state_.cull_mode);
    rasterizer_.frontFace = VK_FRONT_FACE_CLOCKWISE;

    rasterizer_.depthBiasEnable = VK_FALSE;
    rasterizer_.depthBiasConstantFactor = 0.0f;
    rasterizer_.depthBiasClamp = 0.0f;
    rasterizer_.depthBiasSlopeFactor = 0.0f;
}

void VulkanForwardPipelineState::SetupMultiSamplingStateInfo()
{
    multisampling_ = {};
    multisampling_.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling_.pNext = nullptr;

    multisampling_.sampleShadingEnable = VK_FALSE;
    // multisampling defaulted to no multisampling (1 sample per pixel)
    multisampling_.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisampling_.minSampleShading = 1.0f;
    multisampling_.pSampleMask = nullptr;
    multisampling_.alphaToCoverageEnable = VK_FALSE;
    multisampling_.alphaToOneEnable = VK_FALSE;
}

void VulkanComputePipelineState::CompileInternal()
{
    ASSERT(shaders_[static_cast<int>(RHIShaderStage::Compute)] != nullptr);
    ASSERT(shaders_[static_cast<int>(RHIShaderStage::Vertex)] == nullptr);
    ASSERT(shaders_[static_cast<int>(RHIShaderStage::Pixel)] == nullptr);

    SetupShaderStageInfo();
    SetupDescriptorSetLayouts();
    SetupPipelineLayoutInfo();

    VkComputePipelineCreateInfo pipeline_create_info = {};
    pipeline_create_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_create_info.stage = shader_stages_[0];
    pipeline_create_info.layout = pipeline_layout_;

    vkCreateComputePipelines(context->GetDevice(), VK_NULL_HANDLE, 1, &pipeline_create_info, VK_NULL_HANDLE,
                             &pipeline_);

    compiled_ = true;

    context->SetDebugInfo(reinterpret_cast<uint64_t>(pipeline_), VK_OBJECT_TYPE_PIPELINE, GetName().c_str());
}

void VulkanForwardPipelineState::SetupViewport()
{
    auto *render_target = RHICast<VulkanRenderTarget>(render_pass_->GetRenderTarget());

    ASSERT(render_target);

    viewport_ = {};
    viewport_.x = 0.0f;
    viewport_.y = 0.0f;
    viewport_.width = static_cast<float>(render_target->GetExtent().width);
    viewport_.height = static_cast<float>(render_target->GetExtent().height);
    viewport_.minDepth = 0.0f;
    viewport_.maxDepth = 1.0f;

    scissor_ = {};
    scissor_.offset = {.x = 0, .y = 0};
    scissor_.extent = render_target->GetExtent();
}

void VulkanForwardPipelineState::SetupInputAssemblyInfo(VkPrimitiveTopology topology)
{
    input_assembly_ = {};
    input_assembly_.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly_.pNext = nullptr;

    input_assembly_.topology = topology;
    input_assembly_.primitiveRestartEnable = VK_FALSE;
}

void VulkanForwardPipelineState::InitPipelineInfo()
{
    SetupViewport();

    SetupShaderStageInfo();

    SetupVertexInputInfo();
    SetupInputAssemblyInfo(VkPrimitiveTopology::VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    SetupRasterizationStateInfo();
    SetupMultiSamplingStateInfo();
    SetupColorAndDepthAttachments();

    SetupDescriptorSetLayouts();
    SetupPipelineLayoutInfo();
}

void VulkanForwardPipelineState::SetupPipelineLayoutInfo()
{
    VkPipelineLayoutCreateInfo pipeline_layout_create_info = {};
    pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_create_info.pNext = nullptr;

    // empty defaults
    pipeline_layout_create_info.flags = 0;
    pipeline_layout_create_info.setLayoutCount = static_cast<uint32_t>(descriptor_layouts_.size());
    pipeline_layout_create_info.pSetLayouts = descriptor_layouts_.data();
    pipeline_layout_create_info.pushConstantRangeCount = 0;
    pipeline_layout_create_info.pPushConstantRanges = nullptr;

    CHECK_VK_ERROR(
        vkCreatePipelineLayout(context->GetDevice(), &pipeline_layout_create_info, nullptr, &pipeline_layout_));
}

void VulkanForwardPipelineState::SetViewportAndScissor()
{
    // TODO(tqjxlm): implement state cache to avoid running this every frame
    vkCmdSetViewport(context->GetCurrentCommandBuffer(), 0, 1, &viewport_);
    vkCmdSetScissor(context->GetCurrentCommandBuffer(), 0, 1, &scissor_);
}

void VulkanForwardPipelineState::BindBuffers()
{
    if (!vertex_buffers_.empty())
    {
        std::vector<VkBuffer> buffers;
        std::vector<VkDeviceSize> offsets;

        buffers.reserve(vertex_buffers_.size());
        offsets.reserve(vertex_buffers_.size());

        for (const auto &vertex_buffer : vertex_buffers_)
        {
            auto *rhi_buffer = RHICast<VulkanBuffer>(vertex_buffer);
            const auto &buffer = rhi_buffer->GetResourceThisFrame();
            buffers.push_back(buffer);
            offsets.push_back(0);
        }

        vkCmdBindVertexBuffers(context->GetCurrentCommandBuffer(), 0, static_cast<uint32_t>(buffers.size()),
                               buffers.data(), offsets.data());
    }

    if (index_buffer_)
    {
        const VkDeviceSize offset = 0;
        auto *rhi_buffer = RHICast<VulkanBuffer>(index_buffer_);
        const auto &buffer = rhi_buffer->GetResourceThisFrame();
        vkCmdBindIndexBuffer(context->GetCurrentCommandBuffer(), buffer, offset, VK_INDEX_TYPE_UINT32);
    }
}

void VulkanForwardPipelineState::CompileInternal()
{
    ASSERT(shaders_[static_cast<int>(RHIShaderStage::Vertex)] != nullptr);
    ASSERT(shaders_[static_cast<int>(RHIShaderStage::Compute)] == nullptr);

    InitPipelineInfo();
    CreatePipeline();
}

VulkanPipelineState::~VulkanPipelineState()
{
    if (!compiled_)
    {
        return;
    }

    vkDestroyPipeline(context->GetDevice(), pipeline_, nullptr);
    vkDestroyPipelineLayout(context->GetDevice(), pipeline_layout_, nullptr);
}

void VulkanComputePipelineState::BindDescriptorSets()
{
    for (auto set_id = 0u; set_id < descriptor_sets_.size(); set_id++)
    {
        descriptor_sets_[set_id].Bind(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, combined_resource_sets_[set_id],
                                      set_id);
    }
}

void VulkanForwardPipelineState::BindDescriptorSets()
{
    for (auto set_id = 0u; set_id < descriptor_sets_.size(); set_id++)
    {
        descriptor_sets_[set_id].Bind(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                      combined_resource_sets_[set_id], set_id);
    }
}
} // namespace sparkle

#endif
