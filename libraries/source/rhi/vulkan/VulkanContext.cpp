#if ENABLE_VULKAN

#include "VulkanContext.h"

#include "VulkanCommandBuffer.h"
#include "VulkanCommon.h"
#include "VulkanDescriptorSetManager.h"
#include "VulkanFunctionLoader.h"
#include "VulkanSwapChain.h"
#include "application/NativeView.h"

#ifdef __APPLE__
#include "core/math/Utilities.h"
#endif

#include <algorithm>
#include <unordered_set>

namespace sparkle
{
static std::vector<const char *> ray_tracing_extensions = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    // VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
    // Required by VK_KHR_acceleration_structure
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
    // Required by spirv
    VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME, VK_KHR_SPIRV_1_4_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME};

static VkResult CreateDebugUtilsMessengerExt(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT *pCreateInfo,
                                             const VkAllocationCallbacks *pAllocator,
                                             VkDebugUtilsMessengerEXT *pDebugMessenger)
{
    auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    if (func != nullptr)
    {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    }

    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                                    VkDebugUtilsMessageTypeFlagsEXT /*messageType*/,
                                                    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData, // NOLINT
                                                    void * /*pUserData*/)
{

    if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
        // Message is important enough to show
        Log(Warn, "validation error! {}", pCallbackData->pMessage);
    }

#ifndef NDEBUG
    if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        ASSERT(false);
    }
#endif

    return VK_FALSE;
}

static void PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT &createInfo)
{
    createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = DebugCallback;
}

static void DestroyDebugUtilsMessengerExt(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger,
                                          const VkAllocationCallbacks *pAllocator)
{
    auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (func != nullptr)
    {
        func(instance, debugMessenger, pAllocator);
    }
}

static void GetVMAFunctions(VmaVulkanFunctions &functions)
{
    // Vulkan 1.0
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    functions.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
    functions.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
    functions.vkAllocateMemory = vkAllocateMemory;
    functions.vkFreeMemory = vkFreeMemory;
    functions.vkMapMemory = vkMapMemory;
    functions.vkUnmapMemory = vkUnmapMemory;
    functions.vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges;
    functions.vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges;
    functions.vkBindBufferMemory = vkBindBufferMemory;
    functions.vkBindImageMemory = vkBindImageMemory;
    functions.vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements;
    functions.vkGetImageMemoryRequirements = vkGetImageMemoryRequirements;
    functions.vkCreateBuffer = vkCreateBuffer;
    functions.vkDestroyBuffer = vkDestroyBuffer;
    functions.vkCreateImage = vkCreateImage;
    functions.vkDestroyImage = vkDestroyImage;
    functions.vkCmdCopyBuffer = vkCmdCopyBuffer;

    // Vulkan 1.1
#if VMA_VULKAN_VERSION >= 1001000
    if constexpr (ApiVersion >= VK_API_VERSION_1_1)
    {
        functions.vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2;
        functions.vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2;
        functions.vkBindBufferMemory2KHR = vkBindBufferMemory2;
        functions.vkBindImageMemory2KHR = vkBindImageMemory2;
        functions.vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2;
    }
#endif

    // Vulkan 1.3
#if VMA_VULKAN_VERSION >= 1003000
    if constexpr (ApiVersion >= VK_API_VERSION_1_3)
    {
        functions.vkGetDeviceBufferMemoryRequirements = vkGetDeviceBufferMemoryRequirements;
        functions.vkGetDeviceImageMemoryRequirements = vkGetDeviceImageMemoryRequirements;
    }
#endif
}

static bool CheckDeviceExtensionSupport(VkPhysicalDevice device, std::vector<const char *> &device_extensions)
{
    uint32_t extension_count;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr);

    std::vector<VkExtensionProperties> available_extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, available_extensions.data());

    Log(Debug, "available device extensions:");

    for (const auto &extension : available_extensions)
    {
        Log(Debug, "\t{}", extension.extensionName);

        if (strcmp(extension.extensionName, "VK_KHR_portability_subset") == 0)
        {
            device_extensions.push_back("VK_KHR_portability_subset");
        }

        if (strcmp(extension.extensionName, VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME) == 0)
        {
            device_extensions.push_back(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME);
        }
    }

    std::unordered_set<std::string> required_extensions(device_extensions.begin(), device_extensions.end());

    for (const auto &extension : available_extensions)
    {
        required_extensions.erase(extension.extensionName);
    }

    bool const all_extension_good = required_extensions.empty();
    if (!all_extension_good)
    {
        Log(Warn, "Device extension check failed!");
        for (const auto &missing_extension : required_extensions)
        {
            Log(Warn, "missing: {}", missing_extension.data());
        }
    }

    return all_extension_good;
}

static bool DeviceSupportHardwareRayTracing(VkPhysicalDevice device)
{
    if (!CheckDeviceExtensionSupport(device, ray_tracing_extensions))
    {
        return false;
    }

    // VkPhysicalDeviceRayTracingPipelinePropertiesKHR ray_tracing_pipeline_properties{};
    // ray_tracing_pipeline_properties.sType =
    // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;

    // VkPhysicalDeviceProperties2 device_properties2{};
    // device_properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    // device_properties2.pNext = &ray_tracing_pipeline_properties;
    // vkGetPhysicalDeviceProperties2(physical_device_, &device_properties2);

    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration_structure_features{};
    acceleration_structure_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;

    VkPhysicalDeviceFeatures2 device_features2{};
    device_features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    device_features2.pNext = &acceleration_structure_features;
    vkGetPhysicalDeviceFeatures2(device, &device_features2);

    VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_properties{};
    acceleration_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;

    VkPhysicalDeviceProperties2 device_properties{};
    device_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    device_properties.pNext = &acceleration_properties;

    vkGetPhysicalDeviceProperties2(device, &device_properties);

    Log(Debug, "Acceleration structure buffer alignment {}",
        acceleration_properties.minAccelerationStructureScratchOffsetAlignment);

    return acceleration_structure_features.accelerationStructure != 0;
}

static bool IsDeviceSuitable(VkPhysicalDevice device, VkSurfaceKHR surface, std::vector<const char *> &deviceExtensions)
{
    VkPhysicalDeviceProperties device_properties;
    vkGetPhysicalDeviceProperties(device, &device_properties);

    VkPhysicalDeviceFeatures device_features;
    vkGetPhysicalDeviceFeatures(device, &device_features);

    QueueFamilyIndices const indices = FindQueueFamilies(device, surface);

    bool const extensions_supported = CheckDeviceExtensionSupport(device, deviceExtensions);

    bool swap_chain_adequate = false;
    if (extensions_supported)
    {
        SwapChainSupportDetails const swap_chain_support = QuerySwapChainSupport(device, surface);
        swap_chain_adequate = !swap_chain_support.formats.empty() && !swap_chain_support.presentModes.empty();
    }

    bool is_suitable = false;
    std::string fail_reason;
    if (!indices.IsComplete())
    {
        fail_reason = "Queue family incomplete";
    }
    else if (!extensions_supported)
    {
        fail_reason = "Extension missing";
    }
    else if (!swap_chain_adequate)
    {
        fail_reason = "Swap chain not adequate";
    }
    else if (device_features.samplerAnisotropy == 0)
    {
        fail_reason = "Anisotropy sampling not supported";
    }
    else
    {
        is_suitable = true;
    }

    if (is_suitable)
    {
        Log(Info, "Checking device [{}]: OK!", device_properties.deviceName);

        Log(Info, "enabled device extensions:");
        for (const auto &extension : deviceExtensions)
        {
            Log(Info, "\t{}", extension);
        }
    }
    else
    {
        Log(Info, "Checking device [{}]: Failed! {}", device_properties.deviceName, fail_reason.data());
    }

    return is_suitable;
}

VulkanContext::VulkanContext(VulkanRHI *in_rhi) : rhi_(in_rhi)
{
    descriptor_set_manager_ = std::make_unique<VulkanDescriptorSetManager>();
}

VulkanContext::~VulkanContext() = default;

void VulkanContext::SetupDebugMessenger()
{
    if (!enable_validation_)
    {
        return;
    }

    VkDebugUtilsMessengerCreateInfoEXT create_info;
    PopulateDebugMessengerCreateInfo(create_info);

    CHECK_VK_ERROR(CreateDebugUtilsMessengerExt(instance_, &create_info, nullptr, &debug_messenger_));
}

bool VulkanContext::Init()
{
    if (!VulkanFunctionLoader::Init())
    {
        return false;
    }

    if (!CreateInstance())
    {
        return false;
    }
    if (!RecreateSurface())
    {
        return false;
    }
    if (!PickPhysicalDevice())
    {
        return false;
    }
    if (!CreateLogicalDevice())
    {
        return false;
    }

    SetupDebugMessenger();
    SetupMemoryAllocator();
    CreateCommandPool();
    descriptor_set_manager_->Init();
    rhi_->CreateBackBufferRenderTarget();

    return true;
}

void VulkanContext::Cleanup()
{
    vmaDestroyAllocator(allocator_);

    vkDestroyCommandPool(device_, command_pool_, nullptr);
    descriptor_set_manager_->Cleanup();
    vkDestroyDevice(device_, nullptr);

    DestroySurface();

    if (enable_validation_)
    {
        DestroyDebugUtilsMessengerExt(instance_, debug_messenger_, nullptr);
    }
    vkDestroyInstance(instance_, nullptr);
}

void VulkanContext::ReleaseRenderResources()
{
    if (!command_buffers_.empty())
    {
        vkFreeCommandBuffers(device_, command_pool_, static_cast<uint32_t>(command_buffers_.size()),
                             command_buffers_.data());
        command_buffers_.resize(0);
    }

    for (auto &image_available_semaphore : image_acquire_semaphores_per_image_)
    {
        vkDestroySemaphore(device_, image_available_semaphore, nullptr);
    }

    for (auto &commands_finish_semaphore : commands_finish_semaphores_per_image_)
    {
        vkDestroySemaphore(device_, commands_finish_semaphore, nullptr);
    }

    // if we have one valid fence, then we should have all of them valid
    if (!queue_finish_fences_.empty() && queue_finish_fences_[0])
    {
        vkResetFences(device_, static_cast<unsigned>(queue_finish_fences_.size()), queue_finish_fences_.data());
    }

    for (auto &in_flight_fence : queue_finish_fences_)
    {
        vkDestroyFence(device_, in_flight_fence, nullptr);
    }

    image_acquire_semaphores_per_image_.clear();
    commands_finish_semaphores_per_image_.clear();
    queue_finish_fences_.clear();
    queue_finish_fences_for_image_.clear();
    acquire_semaphores_in_use_.clear();

    swap_chain_ = nullptr;
}

bool VulkanContext::RecreateSurface()
{
    DestroySurface();
    auto success = rhi_->GetHardwareInterface()->CreateVulkanSurface(instance_, static_cast<void *>(&surface_));
    ASSERT(surface_);
    Log(Debug, "Vulkan surface created");

    return success;
}

void VulkanContext::DestroySurface()
{
    if (surface_)
    {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        Log(Debug, "Vulkan surface destroyed");
    }
}

void VulkanContext::BeginFrame()
{
    auto frame_index = rhi_->GetFrameIndex();

    // Find an available acquire semaphore (not currently in use)
    VkSemaphore acquire_semaphore = image_acquire_semaphores_per_image_[next_acquire_semaphore_index_];
    next_acquire_semaphore_index_ = (next_acquire_semaphore_index_ + 1) % image_acquire_semaphores_per_image_.size();

    // Store which semaphore we're using for this frame
    acquire_semaphores_in_use_[frame_index] = acquire_semaphore;

    swap_chain_->SwapBuffers(acquire_semaphore);

    auto image_index = swap_chain_->GetCurrentImageIndex();

    // if we want to render to an image, the last command buffer that renders to it should be finished
    if (queue_finish_fences_for_image_[image_index] != VK_NULL_HANDLE)
    {
        CHECK_VK_ERROR(vkWaitForFences(device_, 1, &queue_finish_fences_for_image_[image_index], VK_TRUE, UINT64_MAX));
    }
    queue_finish_fences_for_image_[image_index] = queue_finish_fences_[frame_index];

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = 0;                  // Optional
    begin_info.pInheritanceInfo = nullptr; // Optional

    CHECK_VK_ERROR(vkBeginCommandBuffer(command_buffers_[frame_index], &begin_info));

    current_command_buffer_ = command_buffers_[frame_index];
}

VkResult VulkanContext::EndFrame()
{
    auto image_index = swap_chain_->GetCurrentImageIndex();
    auto frame_index = rhi_->GetFrameIndex();

    auto back_buffer_color = swap_chain_->GetImage(image_index);

    back_buffer_color->TransitionLayout(current_command_buffer_, {.target_layout = RHIImageLayout::Present,
                                                                  .after_stage = RHIPipelineStage::ColorOutput,
                                                                  .before_stage = RHIPipelineStage::Bottom});

    vkEndCommandBuffer(current_command_buffer_);

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    // Color output should wait for the swap chain image to be ready
    // Commands before that are free to fire
    VkSemaphore wait_semaphores[] = {acquire_semaphores_in_use_[frame_index]};
    VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = wait_semaphores;
    submit_info.pWaitDstStageMask = wait_stages;

    // It's possible to submit multiple command buffers
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &current_command_buffer_;

    // Use per-image semaphore for signaling to avoid conflicts with vsync
    VkSemaphore signal_semaphores[] = {commands_finish_semaphores_per_image_[image_index]};
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = signal_semaphores;

    vkResetFences(device_, 1, &queue_finish_fences_[frame_index]);
    CHECK_VK_ERROR(vkQueueSubmit(graphics_queue_, 1, &submit_info, queue_finish_fences_[frame_index]));

    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = signal_semaphores;

    // It's possible to present to multiple swap chains
    VkSwapchainKHR swap_chains[] = {swap_chain_->GetSwapchain()};
    uint32_t image_indices[] = {image_index};
    present_info.swapchainCount = 1;
    present_info.pSwapchains = swap_chains;
    present_info.pImageIndices = image_indices;
    present_info.pResults = nullptr; // Optional

    const VkResult result = vkQueuePresentKHR(present_queue_, &present_info);

    // No longer need acquire_index_ since we use per-image semaphores

    current_command_buffer_ = nullptr;

    while (!pending_command_buffer_resources_.empty() && pending_command_buffer_resources_.front().Finished())
    {
        pending_command_buffer_resources_.pop();
    }

    return result;
}

void VulkanContext::InitRenderResources()
{
    ASSERT(command_buffers_.empty());

    command_buffers_.resize(rhi_->GetMaxFramesInFlight());

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = static_cast<uint32_t>(command_buffers_.size());

    CHECK_VK_ERROR(vkAllocateCommandBuffers(device_, &alloc_info, command_buffers_.data()));

    const uint32_t swapchain_image_count = rhi_->GetMaxFramesInFlight();

    // Create more acquire semaphores than swapchain images to avoid reuse conflicts
    const uint32_t acquire_semaphore_count = swapchain_image_count * 2;
    image_acquire_semaphores_per_image_.resize(acquire_semaphore_count);

    acquire_semaphores_in_use_.resize(swapchain_image_count, VK_NULL_HANDLE);
    commands_finish_semaphores_per_image_.resize(swapchain_image_count);
    queue_finish_fences_.resize(swapchain_image_count);
    queue_finish_fences_for_image_.resize(swapchain_image_count);

    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (auto &semaphore : image_acquire_semaphores_per_image_)
    {
        CHECK_VK_ERROR(vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore));
    }

    for (auto &semaphore : commands_finish_semaphores_per_image_)
    {
        CHECK_VK_ERROR(vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore));
    }

    for (auto &fence : queue_finish_fences_)
    {
        CHECK_VK_ERROR(vkCreateFence(device_, &fence_info, nullptr, &fence));
    }

    std::ranges::fill(queue_finish_fences_for_image_, VK_NULL_HANDLE);
}

bool VulkanContext::CheckValidationLayerSupport()
{
    uint32_t layer_count;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);

    std::vector<VkLayerProperties> available_layers(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data());

#ifndef NDEBUG
    Log(Debug, "available validation layers:");
    for (const auto &layer_properties : available_layers)
    {
        Log(Debug, "\t{}", layer_properties.layerName);
    }
#endif

    std::unordered_set<std::string> required_validation_layers(validation_layers_.begin(), validation_layers_.end());

    for (const auto &layer_properties : available_layers)
    {
        required_validation_layers.erase(layer_properties.layerName);
    }

    return required_validation_layers.empty();
}

bool VulkanContext::CheckInstanceExtensionSupport()
{
    uint32_t extension_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);

    std::vector<VkExtensionProperties> extensions(extension_count);

    vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, extensions.data());

#ifndef NDEBUG
    Log(Debug, "available instance extensions:");
    for (const auto &extension : extensions)
    {
        Log(Debug, "\t{}", extension.extensionName);
    }
#endif

    return true;
}

void VulkanContext::BeginCommandBuffer()
{
    if (current_command_buffer_ != nullptr)
    {
        return;
    }

    ASSERT_F(temporary_command_buffer_ == nullptr,
             "A temporary command buffer is active, should not begin another one");

    temporary_command_buffer_ = new OneShotCommandBufferScope;
    current_command_buffer_ = temporary_command_buffer_->GetCommandBuffer();
}

void VulkanContext::SubmitCommandBuffer()
{
    if (current_command_buffer_ == nullptr)
    {
        return;
    }

    ASSERT_F(temporary_command_buffer_ != nullptr, "No active command buffer to submit");

    current_command_buffer_ = nullptr;
    delete temporary_command_buffer_;
    temporary_command_buffer_ = nullptr;
}

void VulkanContext::SetupMemoryAllocator()
{
    VmaAllocatorCreateInfo allocator_info = {};
    if (rhi_->SupportsHardwareRayTracing())
    {
        allocator_info.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }
    allocator_info.physicalDevice = physical_device_;
    allocator_info.device = device_;
    allocator_info.instance = instance_;

#if VULKAN_USE_VOLK
    VmaVulkanFunctions functions;
    // function pointers have been fetched by volk, just use them
    GetVMAFunctions(functions);
    allocator_info.pVulkanFunctions = &functions;
#endif
    vmaCreateAllocator(&allocator_info, &allocator_);
}

bool VulkanContext::PickPhysicalDevice()
{
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);

    ASSERT_F(device_count != 0, "failed to find GPUs with Vulkan support!");
    if (device_count == 0)
    {
        return false;
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());

    for (const auto &device : devices)
    {
        // always enable ray tracing when we can
        bool can_enable_ray_tracing = DeviceSupportHardwareRayTracing(device);
        if (can_enable_ray_tracing)
        {
            device_extensions_.insert(device_extensions_.end(), ray_tracing_extensions.begin(),
                                      ray_tracing_extensions.end());
        }

        if (IsDeviceSuitable(device, GetSurface(), device_extensions_))
        {
            physical_device_ = device;
            enable_ray_tracing_ = can_enable_ray_tracing;
            auto max_msaa_count = GetMaxUsableSampleCount();

            msaa_samples_ = std::min(rhi_->GetConfig().msaa_samples, max_msaa_count);
            if (msaa_samples_ > 1)
            {
                Log(Info, "MSAA enabled: {}", msaa_samples_);
            }

            break;
        }
    }

    ASSERT_F(physical_device_ != VK_NULL_HANDLE, "failed to find a suitable GPU!");
    return physical_device_ != VK_NULL_HANDLE;
}

bool VulkanContext::CreateInstance()
{
    if (rhi_->GetConfig().enable_validation)
    {
        if (CheckValidationLayerSupport())
        {
            enable_validation_ = true;
        }
        else
        {
            Log(Warn, "validation layers requested, but not available!");
        }
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Sparkle";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "Sparkle";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = ApiVersion;

    GetRequiredInstanceExtensions();
    CheckInstanceExtensionSupport();

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<uint32_t>(instance_extensions_.size());
    create_info.ppEnabledExtensionNames = instance_extensions_.data();
#ifdef __APPLE__
#if VK_KHR_portability_enumeration
    create_info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

    const int use_metal_argument_buffers = 1;
#ifndef NDEBUG
    const int mvk_debug_value = 1;
    const int mvk_log_level = 2;
#endif
    const VkLayerSettingEXT settings[] = {
        {"MoltenVK", "MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", VK_LAYER_SETTING_TYPE_INT32_EXT, 1,
         &use_metal_argument_buffers},
#ifndef NDEBUG
        {"MoltenVK", "MVK_CONFIG_DEBUG", VK_LAYER_SETTING_TYPE_INT32_EXT, 1, &mvk_debug_value},
        {"MoltenVK", "MVK_CONFIG_LOG_LEVEL", VK_LAYER_SETTING_TYPE_INT32_EXT, 1, &mvk_log_level}
#endif
    };
    const VkLayerSettingsCreateInfoEXT layer_settings_create_info = {
        .sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
        .pNext = nullptr,
        .settingCount = ARRAY_COUNT(settings),
        .pSettings = settings,
    };
    create_info.pNext = &layer_settings_create_info;
#endif //__APPLE__

    if (enable_validation_)
    {
        create_info.enabledLayerCount = static_cast<uint32_t>(validation_layers_.size());
        create_info.ppEnabledLayerNames = validation_layers_.data();
    }
    else
    {
        create_info.enabledLayerCount = 0;
    }

    auto result = vkCreateInstance(&create_info, nullptr, &instance_);
    auto success = result == VK_SUCCESS;
    ASSERT_F(success, "{}", static_cast<int>(result));

    if (success)
    {
        VulkanFunctionLoader::LoadInstance(instance_);
    }
    return success;
}

bool VulkanContext::CreateLogicalDevice()
{
    QueueFamilyIndices const indices = FindQueueFamilies(physical_device_, surface_);

    std::vector<VkDeviceQueueCreateInfo> queue_create_infos;
    std::unordered_set<uint32_t> const unique_queue_families = {indices.graphicsFamily, indices.presentFamily};

    float const queue_priority = 1.0f;
    for (uint32_t const queue_family : unique_queue_families)
    {
        VkDeviceQueueCreateInfo queue_create_info{};
        queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_create_info.queueFamilyIndex = queue_family;
        queue_create_info.queueCount = 1;
        queue_create_info.pQueuePriorities = &queue_priority;
        queue_create_infos.push_back(queue_create_info);
    }

    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

    create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size());
    create_info.pQueueCreateInfos = queue_create_infos.data();

    VkPhysicalDeviceFeatures device_features{};
    device_features.samplerAnisotropy = VK_TRUE;
    create_info.pEnabledFeatures = &device_features;

    create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions_.size());
    create_info.ppEnabledExtensionNames = device_extensions_.data();

    if (enable_validation_)
    {
        create_info.enabledLayerCount = static_cast<uint32_t>(validation_layers_.size());
        create_info.ppEnabledLayerNames = validation_layers_.data();
    }
    else
    {
        create_info.enabledLayerCount = 0;
    }

    VkPhysicalDeviceBufferDeviceAddressFeatures enabled_buffer_device_addres_features{};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enabled_acceleration_structure_features{};
    VkPhysicalDeviceRayQueryFeaturesKHR enabled_ray_query_features{};
    VkPhysicalDeviceDescriptorIndexingFeatures enabled_descriptor_indexing_features{};
    VkPhysicalDeviceRobustness2FeaturesEXT enabled_robustness_features{};
    // VkPhysicalDeviceScalarBlockLayoutFeaturesEXT enabled_scalar_block_layout_features{};

    if (rhi_->SupportsHardwareRayTracing())
    {
        enabled_buffer_device_addres_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        enabled_buffer_device_addres_features.bufferDeviceAddress = VK_TRUE;
        ChainVkStructurePtr(create_info, enabled_buffer_device_addres_features);

        enabled_acceleration_structure_features.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        enabled_acceleration_structure_features.accelerationStructure = VK_TRUE;
        enabled_acceleration_structure_features.descriptorBindingAccelerationStructureUpdateAfterBind = VK_TRUE;
        ChainVkStructurePtr(create_info, enabled_acceleration_structure_features);

        enabled_ray_query_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        enabled_ray_query_features.rayQuery = VK_TRUE;
        ChainVkStructurePtr(create_info, enabled_ray_query_features);

        enabled_descriptor_indexing_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        enabled_descriptor_indexing_features.descriptorBindingPartiallyBound = VK_TRUE;
        enabled_descriptor_indexing_features.runtimeDescriptorArray = VK_TRUE;
        // enabled_descriptor_indexing_features.descriptorBindingVariableDescriptorCount = VK_TRUE;
        enabled_descriptor_indexing_features.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        enabled_descriptor_indexing_features.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        ChainVkStructurePtr(create_info, enabled_descriptor_indexing_features);

        enabled_robustness_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
        enabled_robustness_features.nullDescriptor = VK_TRUE;
        ChainVkStructurePtr(create_info, enabled_robustness_features);

        // enabled_scalar_block_layout_features.sType =
        // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES_EXT;
        // enabled_scalar_block_layout_features.scalarBlockLayout = VK_TRUE;
        // ChainVkStructurePtr(create_info, enabled_scalar_block_layout_features);
    }

    auto success = vkCreateDevice(physical_device_, &create_info, nullptr, &device_) == VK_SUCCESS;
    ASSERT(success);

    if (success)
    {
        VulkanFunctionLoader::LoadDevice(device_);

        vkGetDeviceQueue(device_, indices.graphicsFamily, 0, &graphics_queue_);
        vkGetDeviceQueue(device_, indices.presentFamily, 0, &present_queue_);
    }

    return success;
}

void VulkanContext::CreateCommandPool()
{
    QueueFamilyIndices const queue_family_indices = FindQueueFamilies(physical_device_, surface_);

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = queue_family_indices.graphicsFamily;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    CHECK_VK_ERROR(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_));
}

uint32_t VulkanContext::GetMaxUsableSampleCount()
{
    VkPhysicalDeviceProperties physical_device_properties;
    vkGetPhysicalDeviceProperties(physical_device_, &physical_device_properties);

    VkSampleCountFlags const counts = physical_device_properties.limits.framebufferColorSampleCounts &
                                      physical_device_properties.limits.framebufferDepthSampleCounts;
    if (counts & VK_SAMPLE_COUNT_64_BIT)
    {
        return 64;
    }
    if (counts & VK_SAMPLE_COUNT_32_BIT)
    {
        return 32;
    }
    if (counts & VK_SAMPLE_COUNT_16_BIT)
    {
        return 16;
    }
    if (counts & VK_SAMPLE_COUNT_8_BIT)
    {
        return 8;
    }
    if (counts & VK_SAMPLE_COUNT_4_BIT)
    {
        return 4;
    }
    if (counts & VK_SAMPLE_COUNT_2_BIT)
    {
        return 2;
    }

    return 1;
}

void VulkanContext::GetRequiredInstanceExtensions()
{
    std::vector<const char *> required_extensions;
    rhi_->GetHardwareInterface()->GetVulkanRequiredExtensions(required_extensions);

    for (const auto *required_extension : required_extensions)
    {
        instance_extensions_.push_back(required_extension);
    }

    // TODO(tqjxlm): we may want to disable it in release builds
    instance_extensions_.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    Log(Info, "enabled instance extensions:");
    for (const auto *instance_extension : instance_extensions_)
    {
        Log(Info, "\t{}", instance_extension);
    }
}

void VulkanContext::SetDebugInfo(uint64_t objectHandle, VkObjectType objectType, const char *name)
{
    VkDebugUtilsObjectNameInfoEXT name_info = {};
    name_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    name_info.objectType = objectType;
    name_info.objectHandle = objectHandle;
    name_info.pObjectName = name;

    vkSetDebugUtilsObjectNameEXT(device_, &name_info);
}

void VulkanContext::RecreateSwapChain()
{
    if (swap_chain_)
    {
        swap_chain_->Recreate();
    }
    else
    {
        swap_chain_ = std::make_unique<VulkanSwapChain>();
    }
}
} // namespace sparkle

#endif
