#if ENABLE_VULKAN

#include "VulkanFunctionLoader.h"

#if VULKAN_USE_VOLK
#define VOLK_IMPLEMENTATION
#include <volk.h>
#else
#define GET_VK_FN_CHECKED(fn_name)                                                                                     \
    fn_name = reinterpret_cast<PFN_##fn_name>(vkGetDeviceProcAddr(device, #fn_name));                                  \
    ASSERT(fn_name);
#endif

#if PLATFORM_MACOS
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <mach-o/dyld.h>
#endif

namespace sparkle
{

#if PLATFORM_MACOS
static void SetBundledIcdPath()
{
    if (getenv("VK_ICD_FILENAMES"))
    {
        return;
    }

    char exe_path[PATH_MAX];
    uint32_t size = sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &size) != 0)
    {
        return;
    }

    auto icd_path = std::filesystem::path(exe_path).parent_path() / "vulkan/icd.d/MoltenVK_icd.json";
    if (std::filesystem::exists(icd_path))
    {
        setenv("VK_ICD_FILENAMES", icd_path.c_str(), 0);
    }
}
#endif

bool VulkanFunctionLoader::Init()
{
#if VULKAN_USE_VOLK
#if PLATFORM_MACOS
    SetBundledIcdPath();
#endif
    const VkResult volk_init_success = volkInitialize();
    return volk_init_success == VK_SUCCESS;
#else
    return true;
#endif
}

void VulkanFunctionLoader::LoadInstance(VkInstance instance)
{
#if VULKAN_USE_VOLK
    volkLoadInstanceOnly(instance);
#endif
}

void VulkanFunctionLoader::LoadDevice(VkDevice device)
{
#if VULKAN_USE_VOLK
    volkLoadDevice(device);
#else
    if (rhi_->SupportsHardwareRayTracing())
    {
        GET_VK_FN_CHECKED(vkGetBufferDeviceAddressKHR);
        GET_VK_FN_CHECKED(vkCreateAccelerationStructureKHR);
        GET_VK_FN_CHECKED(vkDestroyAccelerationStructureKHR);
        GET_VK_FN_CHECKED(vkGetAccelerationStructureBuildSizesKHR);
        GET_VK_FN_CHECKED(vkGetAccelerationStructureDeviceAddressKHR);
        GET_VK_FN_CHECKED(vkCmdBuildAccelerationStructuresKHR);
        // GET_VK_FN_CHECKED(vkBuildAccelerationStructuresKHR);
        // GET_VK_FN_CHECKED(vkCmdTraceRaysKHR);
        // GET_VK_FN_CHECKED(vkGetRayTracingShaderGroupHandlesKHR);
        // GET_VK_FN_CHECKED(vkCreateRayTracingPipelinesKHR);
    }

    if (rhi->GetRHI()->GetConfig().enable_validation)
    {
        GET_VK_FN_CHECKED(vkSetDebugUtilsObjectNameEXT)
    }
#endif
}

} // namespace sparkle

#endif
