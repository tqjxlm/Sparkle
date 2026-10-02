#include "renderer/graph/RGTexturePool.h"

#include "RGCheck.h"
#include "rhi/RHI.h"

#include <algorithm>

namespace sparkle
{
void RGTexturePool::BeginGraph()
{
    RGCheck(!serving_, "a texture pool serves one graph at a time");
    serving_ = true;
    graph_++;
}

RHIImage *RGTexturePool::Acquire(const Key &key, const std::string &name)
{
    const auto found = std::ranges::find_if(entries_, [this, &key](const Entry &entry) {
        const auto &attributes = entry.image->GetAttributes();
        return entry.last_graph != graph_ && attributes.format == key.format && attributes.width == key.width &&
               attributes.height == key.height && !(key.usages & ~attributes.usages) &&
               attributes.memory_properties == key.memory_properties;
    });
    if (found != entries_.end())
    {
        found->last_graph = graph_;
        stats_.num_reused++;
        return found->image.get();
    }

    auto image = rhi_->CreateImage({.format = key.format,
                                    .width = key.width,
                                    .height = key.height,
                                    .usages = key.usages,
                                    .memory_properties = key.memory_properties},
                                   name);
    stats_.num_created++;
    return entries_.emplace_back(Entry{.image = std::move(image), .last_graph = graph_}).image.get();
}

void RGTexturePool::EndGraph()
{
    serving_ = false;
    stats_.num_released += std::erase_if(
        entries_, [this](const Entry &entry) { return graph_ - entry.last_graph >= UnusedGraphsBeforeRelease; });
}
} // namespace sparkle
