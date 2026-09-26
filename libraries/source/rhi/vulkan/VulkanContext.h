#pragma once

#if ENABLE_VULKAN

#include "VulkanCommandBuffer.h"
#include "VulkanCommandContext.h"
#include "VulkanMemory.h"

#include <queue>

namespace sparkle
{
class VulkanSwapChain;
class VulkanDescriptorSetManager;

class VulkanContext
{
public:
    explicit VulkanContext(VulkanRHI *rhi);

    ~VulkanContext();

    VulkanRHI *GetRHI()
    {
        return rhi_;
    }

    [[nodiscard]] VkDevice GetDevice() const
    {
        return device_;
    }

    [[nodiscard]] VkPhysicalDevice GetPhysicalDevice() const
    {
        return physical_device_;
    }

    [[nodiscard]] VkSurfaceKHR GetSurface() const
    {
        return surface_;
    }

    [[nodiscard]] VkInstance GetInstance() const
    {
        return instance_;
    }

    [[nodiscard]] VkQueue GetGraphicsQueue() const
    {
        return graphics_queue_;
    }

    [[nodiscard]] VkCommandPool GetCommandPool() const
    {
        return command_pool_;
    }

    [[nodiscard]] VmaAllocator GetMemoryAllocator() const
    {
        return allocator_;
    }

    // see RHIContext::GetCommandContext
    [[nodiscard]] VulkanCommandContext *GetCommandContext() const
    {
        return command_context_;
    }

    [[nodiscard]] VulkanSwapChain *GetSwapChain() const
    {
        return swap_chain_.get();
    }

    [[nodiscard]] VulkanDescriptorSetManager &GetDescriptorSetManager() const
    {
        return *descriptor_set_manager_;
    }

    [[nodiscard]] bool SupportsHardwareRayTracing() const
    {
        return enable_ray_tracing_;
    }

    [[nodiscard]] uint32_t GetMinBufferOffsetAlignment() const
    {
        return min_buffer_offset_alignment_;
    }

    [[nodiscard]] bool SupportsSubgroupQuadOps() const
    {
        return supports_subgroup_quad_ops_;
    }

    [[nodiscard]] bool SupportsAstcHdr() const
    {
        return supports_astc_hdr_;
    }

    [[nodiscard]] bool SupportsDynamicRenderingLocalRead() const
    {
        return supports_dynamic_rendering_local_read_;
    }

    [[nodiscard]] bool SupportsUnifiedImageLayouts() const
    {
        return supports_unified_image_layouts_;
    }

    [[nodiscard]] bool CompressedImageBarriersNeedSync1() const
    {
        return compressed_image_barriers_need_sync1_;
    }

    bool Init();

    void BeginCommandBuffer();
    void SubmitCommandBuffer();

    [[nodiscard]] bool BeginFrame();
    VkResult EndFrame();

    void Cleanup();
    void ReleaseRenderResources();
    void DestroySurface();
    bool RecreateSurface();
    void RecreateSwapChain();

    void InitRenderResources();
    void CreateCommandPool();

    void SetDebugInfo(uint64_t objectHandle, VkObjectType objectType, const char *name);

    [[nodiscard]] bool IsValidationEnabled() const
    {
        return enable_validation_;
    }

    void EnqueueCommandBufferResource(OneShotCommandBufferScope::CommandBufferResources &&resources)
    {
        pending_command_buffer_resources_.push(std::move(resources));
    }

private:
    bool CreateInstance();
    bool CreateLogicalDevice();
    bool PickPhysicalDevice();
    void QuerySubgroupQuadSupport();
    void QueryOptionalDeviceFeatures();
    bool CheckValidationLayerSupport();
    static bool CheckInstanceExtensionSupport();
    void GetRequiredInstanceExtensions();
    void SetupDebugMessenger();
    void SetupMemoryAllocator();

    [[nodiscard]] VkPresentModeKHR ChooseSwapPresentMode(
        const std::vector<VkPresentModeKHR> &availablePresentModes) const;
    [[nodiscard]] VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities) const;
    uint32_t GetMaxUsableSampleCount();

    void GenerateMipmaps(VkImage image, VkFormat imageFormat, int32_t texWidth, int32_t texHeight, uint32_t mipLevels);

    void CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) const;

    const std::vector<const char *> validation_layers_ = {"VK_LAYER_KHRONOS_validation"};
    std::vector<const char *> instance_extensions_ = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
    std::vector<const char *> device_extensions_;

    VmaAllocator allocator_;

    VkInstance instance_;                               // an instance can hold multiple logical devices
    VkDevice device_;                                   // multiple logical devices can map to the same physical device
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE; // a physical devices maps to a piece of hardware
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;             // a surface maps to an output, such as a window
    VkQueue graphics_queue_;                            // multiple queues can be bound to the same logical device
    VkQueue present_queue_;                             // multiple queues can be bound to the same logical device

    // all graphics, compute, and transfer work uses one universal queue
    std::vector<VkFence> queue_finish_fences_;

    // due to the fact that we may not acquire image index sequentially, we need to manually assign fences to images
    std::vector<VkFence> queue_finish_fences_for_image_;

    // Use separate semaphores per swapchain image as recommended by validation layer
    // Each swapchain image gets its own acquire and signal semaphores
    std::vector<VkSemaphore> image_acquire_semaphores_per_image_;
    std::vector<VkSemaphore> commands_finish_semaphores_per_image_;

    // Track which acquire semaphore was used for the current frame
    std::vector<VkSemaphore> acquire_semaphores_in_use_;
    size_t next_acquire_semaphore_index_ = 0;

    std::unique_ptr<VulkanSwapChain> swap_chain_;

    // command buffer (one for each swap chain)
    VkCommandPool command_pool_;
    std::vector<VkCommandBuffer> command_buffers_;
    VulkanCommandContext frame_command_context_;
    VulkanCommandContext *command_context_ = nullptr;

    std::unique_ptr<VulkanDescriptorSetManager> descriptor_set_manager_;

    // config
    uint32_t msaa_samples_;

    uint32_t min_buffer_offset_alignment_ = 64;

    bool supports_subgroup_quad_ops_ = false;

    VkDebugUtilsMessengerEXT debug_messenger_;

    bool enable_validation_ = false;

    bool enable_ray_tracing_ = false;
    bool supports_astc_hdr_ = false;
    bool supports_dynamic_rendering_local_read_ = false;
    bool supports_unified_image_layouts_ = false;
    bool compressed_image_barriers_need_sync1_ = false;

    VulkanRHI *rhi_;

    class OneShotCommandBufferScope *temporary_command_buffer_ = nullptr;

    std::queue<OneShotCommandBufferScope::CommandBufferResources> pending_command_buffer_resources_;
};

// the life cycle is managed by VulkanRHI
// TODO(tqjxlm): avoid singleton
inline std::unique_ptr<VulkanContext> context;
} // namespace sparkle

#endif
