#pragma once

#include "renderer/graph/RenderGraph.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace sparkle
{
// the live passes that use a resource, from the first to the last
struct RGLifetime
{
    std::optional<uint32_t> first = std::nullopt;
    uint32_t last = 0;

    void Extend(uint32_t pass)
    {
        first = first.value_or(pass);
        last = pass;
    }
};

struct RenderGraph::Access
{
    RGTexture texture;
    // resolved against the texture's mips and layers
    RGSubresources subresources;
    RHIResourceAccess access;
    RHIImageLayout layout;
    // the color slot or DepthSlot of an attachment, NoSlot otherwise
    uint8_t slot;
    std::optional<Vector4> clear;
    // make the bindings of the access's image, for shader accesses given binding members
    std::vector<RGBuilder::ImageBinding> bindings;

    // compiled
    std::vector<RHIImageBarrier> barriers;
    // of each subresource after the pass's barriers, mip by mip
    std::vector<RHIImageState> states;
    // of an attachment of a raster pass
    RHILoadOp load_op = RHILoadOp::Load;
    std::string load_reason;
    RHIStoreOp store_op = RHIStoreOp::DontCare;
    std::string store_reason;
};

// an access to a buffer or acceleration structure
struct RenderGraph::BufferAccess
{
    uint32_t buffer;
    RHIResourceAccess access;
    std::vector<RHIMemberBinding> bindings;

    // compiled
    std::optional<RHIMemoryBarrier> barrier = std::nullopt;
    // after the pass's barriers
    RHIResourceAccess state{};
};

struct RenderGraph::Pass
{
    std::string name;
    RGPassKind kind;
    RHIResourceRef<RHIComputePass> compute_pass;
    std::vector<Access> accesses;
    std::vector<BufferAccess> buffer_accesses;
    // images outside the graph bound in place of missing inputs, with the bindings they make
    std::vector<std::pair<RHIResourceRef<RHIImage>, RGBuilder::ImageBinding>> placeholders;
    bool fully_overwrites = false;
    bool side_effect = false;
    bool native_access = false;
    std::function<void(RHICommandContext &)> record;

    // compiled
    bool live = true;
    std::string cull_reason;
    RHIRenderingInfo rendering_info{};
    std::vector<RHIMemberBinding> bindings;
    // for each binding, the name of the graph resource it binds; none for a placeholder
    std::vector<std::optional<std::string>> bound_resources;

    // executed: the GPU time in ms its timer reports for this frame's slot, -1 when unknown
    float gpu_ms = -1.f;
};

struct RenderGraph::Texture
{
    std::string name;
    RGTextureDesc desc{};
    RHIResourceRef<RHIImage> imported;

    // compiled
    RHIImage *image = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t mips = 1;
    uint8_t layers = 1;
    RHIImage::ImageUsage usages = RHIImage::ImageUsage::Undefined;
    RGLifetime lifetime{};
    // the transient's index among the images the pool backs this graph with
    std::optional<uint32_t> physical = std::nullopt;
};

// an imported buffer or acceleration structure
struct RenderGraph::Buffer
{
    std::string name;
    std::variant<RHIResourceRef<RHIBuffer>, RHIResourceRef<RHITLAS>> resource;

    // compiled
    RGLifetime lifetime{};
    RHIResourceAccess accesses{};

    [[nodiscard]] RHIResource *Get() const
    {
        return std::visit([](const auto &imported) -> RHIResource * { return imported.get(); }, resource);
    }

    [[nodiscard]] RHITrackedAccess &GetTracked() const
    {
        return std::visit([](const auto &imported) -> RHITrackedAccess & { return imported->GetTracked(); }, resource);
    }
};
} // namespace sparkle
