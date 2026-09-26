#pragma once

#if ENABLE_VULKAN

#include "VulkanCommandContext.h"

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

    explicit OneShotCommandBufferScope(bool should_block_next_frame = false);

    ~OneShotCommandBufferScope();

    [[nodiscard]] VulkanCommandContext &GetCommandContext()
    {
        return command_context_;
    }

private:
    CommandBufferResources resources_;
    VulkanCommandContext command_context_;
    bool should_block_next_frame_ = false;
};
} // namespace sparkle

#endif
