#pragma once

#include "rhi/RHIImage.h"

#include <cstdint>
#include <vector>

namespace sparkle
{
class RHIContext;

// the images behind render graph transients. it serves one graph at a time, which owns every image it acquired until it
// is destroyed; the next graph may reuse them at once, because frames execute in order on one queue and the graph
// plans barriers from each image's tracked state. requests in the same order get the same images, so descriptor caches
// keep hitting. images no graph used for UnusedGraphsBeforeRelease graphs are released through deferred deletion.
class RGTexturePool
{
public:
    static constexpr uint64_t UnusedGraphsBeforeRelease = 8;

    struct Stats
    {
        uint64_t num_created = 0;
        uint64_t num_reused = 0;
        uint64_t num_released = 0;
    };

    explicit RGTexturePool(RHIContext *rhi) : rhi_(rhi)
    {
    }

    [[nodiscard]] const Stats &GetStats() const
    {
        return stats_;
    }

    [[nodiscard]] size_t GetImageCount() const
    {
        return entries_.size();
    }

private:
    friend class RenderGraph;

    struct Key
    {
        PixelFormat format;
        uint32_t width;
        uint32_t height;
        // a pooled image serves any request whose usages it covers
        RHIImage::ImageUsage usages;
    };

    void BeginGraph();

    [[nodiscard]] RHIImage *Acquire(const Key &key, const std::string &name);

    void EndGraph();

    struct Entry
    {
        RHIResourceRef<RHIImage> image;
        uint64_t last_graph;
    };

    RHIContext *rhi_;
    std::vector<Entry> entries_;
    Stats stats_;
    uint64_t graph_ = 0;
    bool serving_ = false;
};
} // namespace sparkle
