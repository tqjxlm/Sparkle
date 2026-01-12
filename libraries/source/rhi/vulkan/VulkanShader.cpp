#if ENABLE_VULKAN

#include "VulkanShader.h"

#include "VulkanCommon.h"
#include "VulkanContext.h"
#include "core/FileManager.h"
#include "core/math/Utilities.h"

#define VULKAN_SHADER_DEBUG 1

namespace sparkle
{
void VulkanShader::SetupShaderReflection(RHIShaderResourceTable *shader_resource_table) const
{
    const auto &binding_map = shader_resource_table->GetBindingMap();

    for (const auto &set : reflection_sets_)
    {
        for (auto slot = 0u; slot < set->binding_count; slot++)
        {
            const auto *binding = set->bindings[slot];
            auto shader_resource = binding_map.find(binding->name);

            // things can be very wrong if this happens. don't continue in any case
            if (shader_resource == binding_map.end())
            {
                ASSERT_F(false, "failed to bind parameter for shader {}: {} [{}, {}]", GetName(), binding->name,
                         set->set, binding->binding);
                abort();
            }

            shader_resource->second->UpdateReflectionIndex(set->set, binding->binding);
        }
    }
}

void VulkanShader::Load()
{
    if (IsValid())
    {
        return;
    }

    auto spv_path = shader_info_->GetPath() + ".spv";
    LoadShaderModule(spv_path.c_str());

    loaded_ = true;

    context->SetDebugInfo(reinterpret_cast<uint64_t>(shader_module_), VK_OBJECT_TYPE_SHADER_MODULE, GetName().c_str());
}

void VulkanShader::LoadShaderModule(const char *file_path)
{
    auto shader_code = FileManager::GetNativeFileManager()->Read(FileEntry::Resource(file_path));

    SpvReflectResult result = spvReflectCreateShaderModule(shader_code.size(), shader_code.data(), &reflection_module_);
    ASSERT_EQUAL(result, SPV_REFLECT_RESULT_SUCCESS);

    uint32_t num_sets = 0;
    result = spvReflectEnumerateDescriptorSets(&reflection_module_, &num_sets, nullptr);
    ASSERT_EQUAL(result, SPV_REFLECT_RESULT_SUCCESS);

    reflection_sets_.resize(num_sets);
    result = spvReflectEnumerateDescriptorSets(&reflection_module_, &num_sets, reflection_sets_.data());
    ASSERT_EQUAL(result, SPV_REFLECT_RESULT_SUCCESS);

    VkShaderModuleCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.pNext = nullptr;

    create_info.codeSize = ARRAY_SIZE(shader_code);
    create_info.pCode = reinterpret_cast<uint32_t *>(shader_code.data());

    CHECK_VK_ERROR(vkCreateShaderModule(context->GetDevice(), &create_info, nullptr, &shader_module_));
}

void VulkanShader::ReleaseShaderModule()
{
    vkDestroyShaderModule(context->GetDevice(), shader_module_, nullptr);
}
} // namespace sparkle

#endif
