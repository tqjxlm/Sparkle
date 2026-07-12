#pragma once

#include "rhi/RHIRenderTarget.h"

#include <vector>

namespace sparkle
{
class RHIContext;

// reuses render targets (and their pool-owned backing images) across frames and renderer recreations.
// Acquire returns a target whose attributes exactly match the request, creating one on miss.
// the pool keeps a reference to every target it hands out. a target becomes available for reuse
// once all external references to it and its images are dropped and the GPU can no longer be
// reading it: either max-frames-in-flight frames after its last use, or immediately after a
// device-idle flush. the pool never releases free targets on its own; call ReleaseUnused to
// reclaim their memory. like all RHI resource creation, it must only be used from the render thread.
class RHIRenderTargetPool
{
public:
    struct Stats
    {
        uint64_t num_created = 0;
        uint64_t num_reused = 0;
        uint64_t num_released = 0;
    };

    explicit RHIRenderTargetPool(RHIContext *rhi) : rhi_(rhi)
    {
    }

    // returns a render target matching attribute. every color or depth attribute with a valid
    // format gets a pool-owned image. back buffer render targets cannot be pooled.
    RHIResourceRef<RHIRenderTarget> Acquire(const RHIRenderTarget::Attribute &attribute, const std::string &name);

    // ages pooled targets: marks ones the GPU can no longer reference as reusable.
    // called once per frame by RHIContext.
    void Tick(uint64_t frame);

    // all previously submitted GPU work has finished: free targets become reusable immediately.
    void NotifyDeviceIdle();

    // releases every target not currently held by a user, returning how many were released.
    // actual GPU resource destruction still goes through the regular deferred-deletion path,
    // so it is safe to call while frames are in flight.
    size_t ReleaseUnused();

    // drops all pooled targets regardless of use. callers must guarantee the GPU is idle.
    void Clear()
    {
        entries_.clear();
    }

    [[nodiscard]] const Stats &GetStats() const
    {
        return stats_;
    }

    [[nodiscard]] size_t GetPooledCount() const
    {
        return entries_.size();
    }

private:
    struct Entry
    {
        RHIResourceRef<RHIRenderTarget> target;
        uint64_t last_used_frame = 0;
        bool gpu_safe = false;
    };

    // true when nothing outside the pool references the target or its images
    [[nodiscard]] static bool IsFree(const Entry &entry);

    RHIContext *rhi_;
    std::vector<Entry> entries_;
    Stats stats_;
    uint64_t current_frame_ = 0;
};
} // namespace sparkle
