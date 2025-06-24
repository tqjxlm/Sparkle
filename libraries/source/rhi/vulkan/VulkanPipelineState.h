#pragma once

#if ENABLE_VULKAN

#include "rhi/VulkanRHI.h"

#include "VulkanDescriptorSet.h"

namespace sparkle
{
inline VkBlendFactor GetVulkanBlendFactor(RHIPipelineState::BlendFactor factor)
{
    switch (factor)
    {
    case RHIPipelineState::BlendFactor::Zero:
        return VK_BLEND_FACTOR_ZERO;
    case RHIPipelineState::BlendFactor::One:
        return VK_BLEND_FACTOR_ONE;
    case RHIPipelineState::BlendFactor::SrcAlpha:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case RHIPipelineState::BlendFactor::OneMinusSrcAlpha:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    }
}

inline VkBlendOp GetVulkanBlendOp(RHIPipelineState::BlendOp op)
{
    switch (op)
    {
    case RHIPipelineState::BlendOp::Add:
        return VK_BLEND_OP_ADD;
    case RHIPipelineState::BlendOp::Min:
        return VK_BLEND_OP_MIN;
    case RHIPipelineState::BlendOp::Max:
        return VK_BLEND_OP_MAX;
    }
}

inline VkFormat GetVertexAttributeFormat(RHIVertexFormat format)
{
    switch (format)
    {
    case RHIVertexFormat::R32G32B32A32Float:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case RHIVertexFormat::R32G32B32Float:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case RHIVertexFormat::R32G32Float:
        return VK_FORMAT_R32G32_SFLOAT;
    default:
        UnImplemented(format);
        return VK_FORMAT_UNDEFINED;
    }
}

inline VkCullModeFlags GetFaceCullMode(RHIPipelineState::FaceCullMode mode)
{
    switch (mode)
    {
    case RHIPipelineState::FaceCullMode::Front:
        return VK_CULL_MODE_FRONT_BIT;
    case RHIPipelineState::FaceCullMode::Back:
        return VK_CULL_MODE_BACK_BIT;
    case RHIPipelineState::FaceCullMode::None:
        return VK_CULL_MODE_NONE;
    }
}

inline VkPolygonMode GetPolygonMode(RHIPipelineState::PolygonMode mode)
{
    switch (mode)
    {

    case RHIPipelineState::PolygonMode::Fill:
        return VK_POLYGON_MODE_FILL;
    case RHIPipelineState::PolygonMode::Line:
        return VK_POLYGON_MODE_LINE;
    case RHIPipelineState::PolygonMode::Point:
        return VK_POLYGON_MODE_POINT;
    }
}

inline VkCompareOp GetDepthCompareOp(const RHIPipelineState::DepthState &depth_state)
{
    switch (depth_state.test_state)
    {
    case RHIPipelineState::DepthTestState::Always:
        return VK_COMPARE_OP_ALWAYS;
    case RHIPipelineState::DepthTestState::Equal:
        return VK_COMPARE_OP_EQUAL;
    case RHIPipelineState::DepthTestState::NotEqual:
        return VK_COMPARE_OP_NOT_EQUAL;
    case RHIPipelineState::DepthTestState::Less:
        return VK_COMPARE_OP_LESS;
    case RHIPipelineState::DepthTestState::LessEqual:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    case RHIPipelineState::DepthTestState::Greater:
        return VK_COMPARE_OP_GREATER;
    case RHIPipelineState::DepthTestState::GreaterEqual:
        return VK_COMPARE_OP_GREATER_OR_EQUAL;
    }
}

class VulkanPipelineState : public RHIPipelineState
{
public:
    VulkanPipelineState(RHIPipelineState::PipelineType type, const std::string &name) : RHIPipelineState(type, name)
    {
    }

    [[nodiscard]] VkPipeline GetPipeline() const
    {
        return pipeline_;
    }

    ~VulkanPipelineState() override;

protected:
    // check if this descriptor set should be replicated by max frames in flight
    [[nodiscard]] bool IsDescriptorSetDynamic(uint32_t set_id) const;

    void SetupShaderStageInfo();

    void SetupPipelineLayoutInfo();

    void SetupDescriptorSetLayouts();

    std::vector<VkPipelineShaderStageCreateInfo> shader_stages_;

    std::vector<VkDescriptorSetLayout> descriptor_layouts_;

    std::vector<VulkanDescriptorSet> descriptor_sets_;

    std::vector<RHIShaderResourceSet> combined_resource_sets_;

    VkPipelineLayout pipeline_layout_;

    VkPipeline pipeline_;
};

class VulkanForwardPipelineState : public VulkanPipelineState
{
public:
    VulkanForwardPipelineState(RHIPipelineState::PipelineType type, const std::string &name)
        : VulkanPipelineState(type, name)
    {
    }

    void CompileInternal() override;

    void SetViewportAndScissor();

    void BindBuffers();

    void BindDescriptorSets();

private:
    struct VulkanVertexInputDescription
    {
        void Clear()
        {
            bindings.clear();
            attributes.clear();
            flags = 0;
        }

        std::vector<VkVertexInputBindingDescription> bindings;
        std::vector<VkVertexInputAttributeDescription> attributes;

        VkPipelineVertexInputStateCreateFlags flags = 0;
    };

    void InitPipelineInfo();

    void CreatePipeline();

    void SetupViewport();

    void SetupVertexInputInfo();

    void SetupInputAssemblyInfo(VkPrimitiveTopology topology);

    void SetupRasterizationStateInfo();

    void SetupMultiSamplingStateInfo();

    void SetupColorAndDepthAttachments();

    void SetupPipelineLayoutInfo();

    VulkanVertexInputDescription vertex_input_description_;

    VkPipelineVertexInputStateCreateInfo vertex_input_info_;
    VkPipelineInputAssemblyStateCreateInfo input_assembly_;
    VkViewport viewport_;
    VkRect2D scissor_;
    VkPipelineRasterizationStateCreateInfo rasterizer_;
    VkPipelineMultisampleStateCreateInfo multisampling_;

    std::vector<VkPipelineColorBlendAttachmentState> color_blend_attachments_;
    VkPipelineColorBlendStateCreateInfo color_blending_;
    VkPipelineDepthStencilStateCreateInfo depth_stencil_;
};

class VulkanComputePipelineState : public VulkanPipelineState
{
public:
    VulkanComputePipelineState(RHIPipelineState::PipelineType type, const std::string &name)
        : VulkanPipelineState(type, name)
    {
    }

    void CompileInternal() override;

    void BindDescriptorSets();
};
} // namespace sparkle

#endif
