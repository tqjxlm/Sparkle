#pragma once

#include "renderer/graph/RenderGraph.h"

#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sparkle
{
template <std::ranges::input_range Parts> std::string Join(Parts &&parts, std::string_view separator)
{
    std::string joined;
    for (const auto &part : parts)
    {
        joined += (joined.empty() ? "" : std::string(separator)) + part;
    }
    return joined;
}

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
    // of a PixelLocalRead request: the color slot it reads, and the bindings it makes only when lowered to Sampled
    std::optional<uint8_t> pixel_local_slot = std::nullopt;
    std::vector<RGBuilder::ImageBinding> sampled_bindings;

    // compiled
    std::vector<RHIImageBarrier> barriers;
    // of each subresource after the pass's barriers, mip by mip
    std::vector<RHIImageState> states;
    // of an attachment of a raster pass
    RHILoadOp load_op = RHILoadOp::Load;
    std::string load_reason;
    RHIStoreOp store_op = RHIStoreOp::DontCare;
    std::string store_reason;
    // of a PixelLocalRead request lowered to Sampled: the break reason of the physical pass of its last writer, or
    // NoWriter
    std::string lowered_reason;

    [[nodiscard]] bool IsAttachment() const
    {
        return slot != NoSlot;
    }

    // a PixelLocalRead request that stays pixel-local
    [[nodiscard]] bool IsPixelLocal() const
    {
        return access.access & RHIAccess::PixelLocalRead;
    }

    [[nodiscard]] bool SameSubresources(const Access &other) const
    {
        return texture == other.texture && subresources == other.subresources;
    }

    [[nodiscard]] bool Overlaps(const Access &other) const;
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
    // of a live pass: its physical pass, and its step there
    uint32_t physical = 0;
    uint32_t step = 0;
    // the attachments of its physical pass that a raster pass leaves untouched, as in RHIAttachmentSignature
    uint8_t unwritten_color_slots = 0;
    bool depth_unused = false;
    std::vector<RHIMemberBinding> bindings;
    // for each binding, the name of the graph resource it binds; none for a placeholder
    std::vector<std::optional<std::string>> bound_resources;
    // the host reads of the buffers whose last live pass this is, recorded after the pass
    std::vector<BufferAccess> host_reads;

    // executed: the CPU time its record function took
    float cpu_ms = -1.f;
};

// why the next live pass does not join a physical pass
enum class RGBreakReason : uint8_t
{
    None,
    NonRasterPass,
    ExternalPass,
    TargetSizeMismatch,
    DifferentDepth,
    SlotConflict,
    ClearInPass,
    NonLocalRead,
    AttachmentReadInPass,
    TileBudget,
    NoPixelLocalSupport,
    Disabled,
};

// a rule the next live pass breaks, with the resources it names
struct RGBreak
{
    RGBreakReason reason;
    std::vector<std::string> resources;

    // e.g. "NonLocalRead(Screen)", naming only the first resource unless `all_resources`
    [[nodiscard]] std::string ToString(bool all_resources) const;
};

// consecutive live passes recorded as one: raster passes in one rendering, any other pass on its own
struct RenderGraph::PhysicalPass
{
    // pass indices, in order
    std::vector<uint32_t> members;
    // the member names joined by '+', which also key its timer
    std::string name;
    // every rule the next live pass breaks, in rule order. the first one is why it does not join.
    std::vector<RGBreak> breaks;

    // compiled, of a raster physical pass: the union of the members' attachments, and the access of the first member
    // attaching each, whose load op applies
    RHIRenderingInfo rendering_info{};
    std::vector<const Access *> attachments;

    // executed: the GPU time in ms its timer reports for this frame's slot, -1 when unknown
    float gpu_ms = -1.f;
};

struct RenderGraph::Texture
{
    std::string name;
    RGTextureDesc desc{};
    RHIResourceRef<RHIImage> imported;

    // an import's image; a transient's pooled image once compiled
    RHIImage *image = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t mips = 1;
    uint8_t layers = 1;

    // compiled
    RHIImage::ImageUsage usages = RHIImage::ImageUsage::Undefined;
    RGLifetime lifetime{};
    // the transient's index among the images the pool backs this graph with
    std::optional<uint32_t> physical = std::nullopt;
    // a transient whose contents never leave the tile memory of its one physical pass
    bool memoryless = false;
};

// an imported buffer or acceleration structure
struct RenderGraph::Buffer
{
    std::string name;
    std::variant<RHIResourceRef<RHIBuffer>, RHIResourceRef<RHITLAS>> resource;
    bool read_on_host = false;

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
