#pragma once

#if ENABLE_VULKAN

#include "VulkanCommandBuffer.h"
#include "VulkanMemory.h"
#include "rhi/RHITrackedState.h"

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

    [[nodiscard]] const VkCommandBuffer &GetCurrentCommandBuffer() const
    {
        return current_command_buffer_;
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

    // per-command-buffer state set: every state record goes through here so redundant records are
    // dropped uniformly. raw vkCmd* state calls elsewhere would let this go stale and skip real changes.
    void BindPipeline(VkPipelineBindPoint bind_point, VkPipeline pipeline)
    {
        auto &tracked = bind_point == VK_PIPELINE_BIND_POINT_COMPUTE ? command_state_.compute_pipeline
                                                                     : command_state_.graphics_pipeline;
        if (tracked.Update(pipeline))
        {
            vkCmdBindPipeline(GetCurrentCommandBuffer(), bind_point, pipeline);
        }
    }

    void SetViewportAndScissor(const VkViewport &viewport, const VkRect2D &scissor)
    {
        CommandState::ViewportScissorKey key{};
        key.viewport = viewport;
        key.scissor = scissor;

        if (command_state_.viewport_scissor.Update(key))
        {
            vkCmdSetViewport(GetCurrentCommandBuffer(), 0, 1, &viewport);
            vkCmdSetScissor(GetCurrentCommandBuffer(), 0, 1, &scissor);
        }
    }

    void BindVertexBuffers(const VkBuffer *buffers, const VkDeviceSize *offsets, uint32_t count)
    {
        if (count > CommandState::MaxTrackedVertexBuffers)
        {
            command_state_.vertex_buffers.Reset();
            vkCmdBindVertexBuffers(GetCurrentCommandBuffer(), 0, count, buffers, offsets);
            return;
        }

        CommandState::VertexBufferKey key{};
        key.count = count;
        for (uint32_t i = 0; i < count; i++)
        {
            key.buffers[i] = buffers[i];
            key.offsets[i] = offsets[i];
        }

        if (command_state_.vertex_buffers.Update(key))
        {
            vkCmdBindVertexBuffers(GetCurrentCommandBuffer(), 0, count, buffers, offsets);
        }
    }

    void BindIndexBuffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType type)
    {
        CommandState::IndexBufferKey key{};
        key.buffer = buffer;
        key.offset = offset;
        key.type = type;

        if (command_state_.index_buffer.Update(key))
        {
            vkCmdBindIndexBuffer(GetCurrentCommandBuffer(), buffer, offset, type);
        }
    }

    void BindDescriptorSet(VkPipelineBindPoint bind_point, VkPipelineLayout layout, uint32_t set_id,
                           VkDescriptorSet set, const uint32_t *dynamic_offsets, uint32_t offset_count)
    {
        auto &slots =
            bind_point == VK_PIPELINE_BIND_POINT_COMPUTE ? command_state_.compute_sets : command_state_.graphics_sets;
        if (set_id >= CommandState::MaxTrackedSets || offset_count > CommandState::MaxTrackedOffsets)
        {
            if (set_id < CommandState::MaxTrackedSets)
            {
                slots[set_id].Reset();
            }
            vkCmdBindDescriptorSets(GetCurrentCommandBuffer(), bind_point, layout, set_id, 1, &set, offset_count,
                                    dynamic_offsets);
            return;
        }

        CommandState::DescriptorSetKey key{};
        key.set = set;
        key.layout = layout;
        key.offset_count = offset_count;
        for (uint32_t i = 0; i < offset_count; i++)
        {
            key.dynamic_offsets[i] = dynamic_offsets[i];
        }

        if (slots[set_id].Update(key))
        {
            vkCmdBindDescriptorSets(GetCurrentCommandBuffer(), bind_point, layout, set_id, 1, &set, offset_count,
                                    dynamic_offsets);
        }
    }

    // multi-set raw path (e.g. NRD): records unconditionally and invalidates the touched slots
    void BindDescriptorSets(VkPipelineBindPoint bind_point, VkPipelineLayout layout, uint32_t first_set,
                            const VkDescriptorSet *sets, uint32_t count)
    {
        auto &slots =
            bind_point == VK_PIPELINE_BIND_POINT_COMPUTE ? command_state_.compute_sets : command_state_.graphics_sets;
        for (uint32_t i = first_set; i < first_set + count && i < CommandState::MaxTrackedSets; i++)
        {
            slots[i].Reset();
        }

        vkCmdBindDescriptorSets(GetCurrentCommandBuffer(), bind_point, layout, first_set, count, sets, 0, nullptr);
    }

    void ResetCommandState()
    {
        command_state_.Reset();
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
    VkCommandBuffer current_command_buffer_ = nullptr;

    std::unique_ptr<VulkanDescriptorSetManager> descriptor_set_manager_;

    // config
    uint32_t msaa_samples_;

    uint32_t min_buffer_offset_alignment_ = 64;

    bool supports_subgroup_quad_ops_ = false;

    VkDebugUtilsMessengerEXT debug_messenger_;

    bool enable_validation_ = false;

    struct CommandState
    {
        static constexpr uint32_t MaxTrackedVertexBuffers = 8;
        static constexpr uint32_t MaxTrackedSets = 8;
        static constexpr uint32_t MaxTrackedOffsets = 8;

        struct ViewportScissorKey
        {
            VkViewport viewport;
            VkRect2D scissor;
        };

        struct VertexBufferKey
        {
            std::array<VkBuffer, MaxTrackedVertexBuffers> buffers;
            std::array<VkDeviceSize, MaxTrackedVertexBuffers> offsets;
            uint32_t count;
        };

        struct IndexBufferKey
        {
            VkBuffer buffer;
            VkDeviceSize offset;
            VkIndexType type;
        };

        struct DescriptorSetKey
        {
            VkDescriptorSet set;
            VkPipelineLayout layout;
            std::array<uint32_t, MaxTrackedOffsets> dynamic_offsets;
            uint32_t offset_count;
        };

        RHITrackedState<VkPipeline> graphics_pipeline;
        RHITrackedState<VkPipeline> compute_pipeline;
        RHITrackedState<ViewportScissorKey> viewport_scissor;
        RHITrackedState<VertexBufferKey> vertex_buffers;
        RHITrackedState<IndexBufferKey> index_buffer;
        std::array<RHITrackedState<DescriptorSetKey>, MaxTrackedSets> graphics_sets;
        std::array<RHITrackedState<DescriptorSetKey>, MaxTrackedSets> compute_sets;

        void Reset()
        {
            graphics_pipeline.Reset();
            compute_pipeline.Reset();
            viewport_scissor.Reset();
            vertex_buffers.Reset();
            index_buffer.Reset();
            for (auto &slot : graphics_sets)
            {
                slot.Reset();
            }
            for (auto &slot : compute_sets)
            {
                slot.Reset();
            }
        }
    };

    CommandState command_state_;
    bool enable_ray_tracing_ = false;

    VulkanRHI *rhi_;

    class OneShotCommandBufferScope *temporary_command_buffer_ = nullptr;

    std::queue<OneShotCommandBufferScope::CommandBufferResources> pending_command_buffer_resources_;
};

// the life cycle is managed by VulkanRHI
// TODO(tqjxlm): avoid singleton
inline std::unique_ptr<VulkanContext> context;
} // namespace sparkle

#endif
