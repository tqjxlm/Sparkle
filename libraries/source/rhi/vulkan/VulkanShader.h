#pragma once

#if ENABLE_VULKAN

#include "rhi/VulkanRHI.h"

#include <spirv_reflect.h>

namespace sparkle
{
inline VkShaderStageFlagBits GetShaderStage(RHIShaderStage type)
{
    switch (type)
    {
    case RHIShaderStage::Vertex:
        return VkShaderStageFlagBits::VK_SHADER_STAGE_VERTEX_BIT;
    case RHIShaderStage::Pixel:
        return VkShaderStageFlagBits::VK_SHADER_STAGE_FRAGMENT_BIT;
    case RHIShaderStage::Compute:
        return VkShaderStageFlagBits::VK_SHADER_STAGE_COMPUTE_BIT;
    default:
        ASSERT(false);
        return VK_SHADER_STAGE_ALL;
    }
}

class VulkanShader : public RHIShader
{
public:
    VulkanShader(const RHIShaderInfo *shader_info, std::string variant) : RHIShader(shader_info, std::move(variant))
    {
    }

    void Load() override;

    ~VulkanShader() override
    {
        ReleaseShaderModule();

        spvReflectDestroyShaderModule(&reflection_module_);
    }

    [[nodiscard]] VkShaderModule GetShaderModule() const
    {
        ASSERT(IsValid());
        return shader_module_;
    }

    [[nodiscard]] const char *GetEntryPointName() const
    {
        ASSERT(IsValid());
        return reflection_module_.entry_point_name;
    }

    void SetupShaderReflection(RHIShaderResourceTable *shader_resource_table) const;

private:
    void LoadShaderModule(const char *file_path);

    void ReleaseShaderModule();

    VkShaderModule shader_module_;

    SpvReflectShaderModule reflection_module_;
    std::vector<SpvReflectDescriptorSet *> reflection_sets_;
};

} // namespace sparkle

#endif
