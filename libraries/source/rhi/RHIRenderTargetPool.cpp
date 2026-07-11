#include "rhi/RHIRenderTargetPool.h"

#include "core/Logger.h"
#include "core/ThreadManager.h"
#include "rhi/RHI.h"

#include <algorithm>
#include <format>

namespace sparkle
{
bool RHIRenderTargetPool::IsFree(const Entry &entry)
{
    if (entry.target.use_count() > 1)
    {
        return false;
    }

    for (const auto &image : entry.target->GetColorImages())
    {
        if (image && image.use_count() > 1)
        {
            return false;
        }
    }

    const auto &depth_image = entry.target->GetDepthImage();
    return !depth_image || depth_image.use_count() == 1;
}

RHIResourceRef<RHIRenderTarget> RHIRenderTargetPool::Acquire(const RHIRenderTarget::Attribute &attribute,
                                                             const std::string &name)
{
    ASSERT(ThreadManager::IsInRenderThread());

    for (auto &entry : entries_)
    {
        if (entry.gpu_safe && IsFree(entry) && entry.target->GetAttribute() == attribute)
        {
            entry.last_used_frame = current_frame_;
            entry.gpu_safe = false;
            stats_.num_reused++;

            Log(Debug, "Render target pool: {} serves request {}", entry.target->GetName(), name);

            return entry.target;
        }
    }

    RHIRenderTarget::ColorImageArray color_images;
    for (auto i = 0u; i < RHIRenderTarget::MaxNumColorImage; i++)
    {
        const auto &color_attribute = attribute.GetColorAttribute(i);
        if (color_attribute.format == PixelFormat::Count)
        {
            continue;
        }
        color_images[i] = rhi_->CreateImage(color_attribute, std::format("{}_Color{}", name, i));
    }

    RHIResourceRef<RHIImage> depth_image;
    if (attribute.GetDepthAttribute().format != PixelFormat::Count)
    {
        depth_image = rhi_->CreateImage(attribute.GetDepthAttribute(), std::format("{}_Depth", name));
    }

    ASSERT_F(depth_image || std::ranges::any_of(color_images, [](const auto &image) { return image != nullptr; }),
             "Render target pool request {} has no valid attachment", name);

    auto target = rhi_->CreateRenderTarget(attribute, color_images, depth_image, name);

    entries_.push_back({.target = target, .last_used_frame = current_frame_, .gpu_safe = false});
    stats_.num_created++;

    return target;
}

void RHIRenderTargetPool::Tick(uint64_t frame)
{
    current_frame_ = frame;

    for (auto &entry : entries_)
    {
        if (!IsFree(entry))
        {
            entry.last_used_frame = frame;
            entry.gpu_safe = false;
        }
        else if (!entry.gpu_safe && frame > entry.last_used_frame + rhi_->GetMaxFramesInFlight())
        {
            // frames in flight can no longer reference the target
            entry.gpu_safe = true;
        }
    }
}

size_t RHIRenderTargetPool::ReleaseUnused()
{
    ASSERT(ThreadManager::IsInRenderThread());

    const auto released = std::erase_if(entries_, [](const Entry &entry) {
        if (!IsFree(entry))
        {
            return false;
        }

        Log(Debug, "Render target pool: release {}", entry.target->GetName());
        return true;
    });

    stats_.num_released += released;

    return released;
}

void RHIRenderTargetPool::NotifyDeviceIdle()
{
    for (auto &entry : entries_)
    {
        if (IsFree(entry))
        {
            entry.gpu_safe = true;
        }
    }
}
} // namespace sparkle
