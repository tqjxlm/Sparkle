#pragma once

#if ENABLE_VULKAN

#include "rhi/VulkanRHI.h"

namespace sparkle
{
class OneShotCommandBufferScope
{
public:
    struct CommandBufferResources
    {
        VkCommandBuffer command_buffer = nullptr;
        VkFence fence = nullptr;

        CommandBufferResources() = default;

        CommandBufferResources(CommandBufferResources &&other) noexcept
        {
            command_buffer = other.command_buffer;
            other.command_buffer = nullptr;
            fence = other.fence;
            other.fence = nullptr;
        }

        ~CommandBufferResources();

        [[nodiscard]] bool Finished() const;
    };

    explicit OneShotCommandBufferScope(bool use_external_if_possible = false, bool should_block_next_frame = false);

    ~OneShotCommandBufferScope();

    [[nodiscard]] VkCommandBuffer GetCommandBuffer() const
    {
        return resources_.command_buffer;
    }

private:
    CommandBufferResources resources_;
    bool use_external_command_buffer_ = false;
    bool should_block_next_frame_ = false;
};
} // namespace sparkle

#endif
