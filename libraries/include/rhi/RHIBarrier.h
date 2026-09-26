#pragma once

#include "core/Enum.h"

#include <cstdint>
#include <optional>

namespace sparkle
{
class RHIImage;

enum class RHIImageLayout : uint8_t
{
    Undefined,
    General,
    Read,
    StorageWrite,
    ColorOutput,
    DepthStencilOutput,
    TransferSrc,
    TransferDst,
    PreInitialized,
    Present,
};

// each access implies its pipeline stages, except shader accesses whose stages come from RHIShaderStageMask
enum class RHIAccess : uint16_t
{
    None = 0,
    ColorWrite = 1u << 0,
    DepthWrite = 1u << 1,
    DepthTest = 1u << 2,
    Sampled = 1u << 3,
    StorageRead = 1u << 4,
    StorageWrite = 1u << 5,
    // copies and blits
    CopySrc = 1u << 6,
    CopyDst = 1u << 7,
    Uniform = 1u << 8,
    VertexInput = 1u << 9,
    IndexInput = 1u << 10,
    IndirectArgs = 1u << 11,
    AccelerationStructureBuild = 1u << 12,
    // ray queries
    AccelerationStructureRead = 1u << 13,
    Present = 1u << 14,
    HostRead = 1u << 15,
};

RegisterEnumAsFlag(RHIAccess);

enum class RHIShaderStageMask : uint8_t
{
    None = 0,
    Vertex = 1u << 0,
    Pixel = 1u << 1,
    Compute = 1u << 2,
    All = Vertex | Pixel | Compute,
};

RegisterEnumAsFlag(RHIShaderStageMask);

struct RHIResourceAccess
{
    RHIAccess access = RHIAccess::None;
    // stages of the shader accesses in `access` (sampled, storage, uniform, acceleration structure read)
    RHIShaderStageMask stages = RHIShaderStageMask::None;

    bool operator==(const RHIResourceAccess &) const = default;

    [[nodiscard]] RHIResourceAccess operator|(const RHIResourceAccess &other) const
    {
        return {.access = access | other.access, .stages = stages | other.stages};
    }

    [[nodiscard]] bool Contains(const RHIResourceAccess &other) const
    {
        return !(other.access & ~access) && !(other.stages & ~stages);
    }

    [[nodiscard]] bool HasWrite() const
    {
        return access & (RHIAccess::ColorWrite | RHIAccess::DepthWrite | RHIAccess::StorageWrite | RHIAccess::CopyDst |
                         RHIAccess::AccelerationStructureBuild);
    }
};

// the tracked state of one image subresource
struct RHIImageState
{
    RHIImageLayout layout;
    // the accesses the next barrier must wait for
    RHIResourceAccess access;

    bool operator==(const RHIImageState &) const = default;
};

// the transition rule for one subresource: `target` is the next access and the layout it needs. returns nullopt when it
// is a read already covered by the tracked reads in the same layout; otherwise it needs a barrier from `state`, and the
// result is the state after it. a read in the same layout widens the tracked reads, so later writes also wait for them.
[[nodiscard]] inline std::optional<RHIImageState> TransitionImageState(const RHIImageState &state,
                                                                       const RHIImageState &target)
{
    const bool read_after_read = state.layout == target.layout && !state.access.HasWrite() && !target.access.HasWrite();
    if (read_after_read && state.access.Contains(target.access))
    {
        return std::nullopt;
    }

    return RHIImageState{.layout = target.layout,
                         .access = read_after_read ? state.access | target.access : target.access};
}

// an Undefined from_layout discards the contents
struct RHIImageBarrier
{
    const RHIImage *image;
    unsigned base_mip;
    unsigned mip_count;
    unsigned base_array_layer;
    unsigned array_layer_count;
    RHIResourceAccess from;
    RHIResourceAccess to;
    RHIImageLayout from_layout;
    RHIImageLayout to_layout;
};

// orders every buffer and acceleration structure access in `from` before those in `to`
struct RHIMemoryBarrier
{
    RHIResourceAccess from;
    RHIResourceAccess to;
};
} // namespace sparkle
