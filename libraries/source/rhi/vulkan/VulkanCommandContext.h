#pragma once

#if ENABLE_VULKAN

#include "rhi/RHITrackedState.h"
#include "rhi/VulkanRHI.h"

namespace sparkle
{
class VulkanCommandContext final : public RHICommandContext
{
public:
    using RHICommandContext::RHICommandContext;

    [[nodiscard]] VkCommandBuffer GetCommandBuffer() const
    {
        return command_buffer_;
    }

    // records into a command buffer that has begun, starting from no tracked bind state
    void Begin(VkCommandBuffer command_buffer);

    void End();

    void DrawMesh(const RHIResourceRef<RHIPipelineState> &pipeline_state, const DrawArgs &draw_args) override;

    void DispatchCompute(const RHIResourceRef<RHIPipelineState> &pipeline, Vector3UInt total_threads,
                         Vector3UInt thread_per_group) override;

    // per-command-buffer state set: every state record goes through here so redundant records are
    // dropped uniformly. raw vkCmd* state calls elsewhere would let this go stale and skip real changes.
    void BindPipeline(VkPipelineBindPoint bind_point, VkPipeline pipeline)
    {
        auto &tracked = bind_point == VK_PIPELINE_BIND_POINT_COMPUTE ? command_state_.compute_pipeline
                                                                     : command_state_.graphics_pipeline;
        if (tracked.Update(pipeline))
        {
            vkCmdBindPipeline(command_buffer_, bind_point, pipeline);
        }
    }

    void SetViewportAndScissor(const VkViewport &viewport, const VkRect2D &scissor)
    {
        CommandState::ViewportScissorKey key{};
        key.viewport = viewport;
        key.scissor = scissor;

        if (command_state_.viewport_scissor.Update(key))
        {
            vkCmdSetViewport(command_buffer_, 0, 1, &viewport);
            vkCmdSetScissor(command_buffer_, 0, 1, &scissor);
        }
    }

    void BindVertexBuffers(const VkBuffer *buffers, const VkDeviceSize *offsets, uint32_t count)
    {
        if (count > CommandState::MaxTrackedVertexBuffers)
        {
            command_state_.vertex_buffers.Reset();
            vkCmdBindVertexBuffers(command_buffer_, 0, count, buffers, offsets);
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
            vkCmdBindVertexBuffers(command_buffer_, 0, count, buffers, offsets);
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
            vkCmdBindIndexBuffer(command_buffer_, buffer, offset, type);
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
            vkCmdBindDescriptorSets(command_buffer_, bind_point, layout, set_id, 1, &set, offset_count,
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
            vkCmdBindDescriptorSets(command_buffer_, bind_point, layout, set_id, 1, &set, offset_count,
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

        vkCmdBindDescriptorSets(command_buffer_, bind_point, layout, first_set, count, sets, 0, nullptr);
    }

    void ResetCommandState()
    {
        command_state_.Reset();
    }

protected:
    void BarrierInternal(std::span<const RHIImageBarrier> image_barriers,
                         std::span<const RHIMemoryBarrier> memory_barriers) override;
    void CopyBufferInternal(const RHIBuffer *src, const RHIBuffer *dst) override;
    void CopyBufferToImageInternal(const RHIBuffer *src, const RHIImage *dst) override;
    void CopyImageToBufferInternal(const RHIImage *src, const RHIBuffer *dst) override;
    void BlitImageInternal(const RHIImage *src, const RHIImage *dst, RHISampler::FilteringMethod filter) override;
    void BeginDebugLabel(const std::string &name) const override;
    void EndDebugLabel() const override;
    void BeginRenderingInternal(const RHIRenderingInfo &info, const std::string &name, RHITimer *timer) override;
    void EndRenderingInternal() override;
    void BeginComputePassInternal(const RHIResourceRef<RHIComputePass> &pass) override;
    void EndComputePassInternal(const RHIResourceRef<RHIComputePass> &pass) override;

private:
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

    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    CommandState command_state_;
};
} // namespace sparkle

#endif
