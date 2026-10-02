#include "renderer/graph/RenderGraph.h"

#include "RenderGraphInternal.h"

#include <magic_enum/magic_enum_flags.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <functional>
#include <iterator>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace sparkle
{
// the set flags in bit order, e.g. "Sampled|StorageRead", or "None"
template <EnumType Flags> static std::string JoinFlags(Flags flags)
{
    auto joined = magic_enum::enum_flags_name(flags);
    return joined.empty() ? "None" : joined;
}

static std::string ToString(const RHIResourceAccess &access)
{
    auto name = JoinFlags(access.access);
    if (access.stages != RHIShaderStageMask::None)
    {
        name += "(" + JoinFlags(access.stages) + ")";
    }
    return name;
}

// e.g. "mip 1 layer 2" or "mips 0-4"; empty when the subresources cover every mip and layer
static std::string ToString(unsigned base_mip, unsigned mip_count, unsigned base_layer, unsigned layer_count,
                            unsigned mips, unsigned layers)
{
    const auto range = [](const char *name, unsigned base, unsigned count) {
        return count == 1 ? std::format("{} {}", name, base) : std::format("{}s {}-{}", name, base, base + count - 1);
    };
    std::string subresources = mip_count == mips ? "" : range("mip", base_mip, mip_count);
    if (layer_count != layers)
    {
        subresources += (subresources.empty() ? "" : " ") + range("layer", base_layer, layer_count);
    }
    return subresources;
}

// vertex or compute work waiting for fragment work, which a tiler cannot overlap with the fragment work it waits for
static bool IsBackward(const RHIResourceAccess &from, const RHIResourceAccess &to)
{
    constexpr auto FragmentAccess =
        RHIAccess::ColorWrite | RHIAccess::DepthWrite | RHIAccess::DepthTest | RHIAccess::PixelLocalRead;
    return ((from.access & FragmentAccess) || (from.stages & RHIShaderStageMask::Pixel)) &&
           (to.stages & (RHIShaderStageMask::Vertex | RHIShaderStageMask::Compute));
}

static nlohmann::json DumpBarrier(nlohmann::json dumped, const RHIResourceAccess &from, const RHIResourceAccess &to)
{
    dumped["from"] = ToString(from);
    dumped["to"] = ToString(to);
    if (IsBackward(from, to))
    {
        dumped["backward"] = true;
    }
    return dumped;
}

static nlohmann::json DumpMemoryBarrier(const std::string &resource, const RHIMemoryBarrier &barrier)
{
    return DumpBarrier({{"resource", resource}}, barrier.from, barrier.to);
}

static std::string Join(const std::vector<std::string> &parts, std::string_view separator)
{
    std::string joined;
    for (const auto &part : parts)
    {
        joined += (joined.empty() ? "" : std::string(separator)) + part;
    }
    return joined;
}

static std::string ToMegabytes(uint64_t bytes)
{
    return std::format("{:.2f} MB", static_cast<double>(bytes) / 1e6);
}

// the indices of the first and last live pass using the resource, and the union of its usages
static void DumpUses(nlohmann::json &dumped, const RGLifetime &lifetime, std::string usage)
{
    if (lifetime.first)
    {
        dumped["first_use"] = *lifetime.first;
        dumped["last_use"] = lifetime.last;
        dumped["usage"] = std::move(usage);
    }
}

nlohmann::json RenderGraph::Dump() const
{
    const auto dump_subresources = [this](const Access &access) {
        const auto &texture = textures_[access.texture.index];
        const auto &range = access.subresources;
        return ToString(range.base_mip, range.mip_count, range.base_layer, range.layer_count, texture.mips,
                        texture.layers);
    };

    struct
    {
        uint64_t render_passes = 0;
        uint64_t barriers = 0;
        uint64_t load_bytes = 0;
        uint64_t store_bytes = 0;
        uint64_t transient_bytes = 0;
        uint64_t memoryless_bytes = 0;
    } totals;

    auto passes = nlohmann::json::array();
    for (const auto &pass : passes_)
    {
        nlohmann::json dumped{{"name", pass.name}, {"kind", Enum2Str(pass.kind)}, {"culled", !pass.live}};
        if (pass.live)
        {
            dumped["physical_pass"] = pass.physical;
            dumped["step"] = pass.step;
        }
        else
        {
            dumped["cull_reason"] = pass.cull_reason;
        }

        auto accesses = nlohmann::json::array();
        auto barriers = nlohmann::json::array();
        for (const auto &access : pass.accesses)
        {
            const auto &texture = textures_[access.texture.index];
            const auto &resource = texture.name;
            const auto subresources = dump_subresources(access);
            nlohmann::json dumped_access{{"resource", resource}, {"access", ToString(access.access)}};
            if (!subresources.empty())
            {
                dumped_access["subresources"] = subresources;
            }
            if (access.clear)
            {
                dumped_access["clear"] = true;
            }
            if (access.pixel_local_slot)
            {
                dumped_access["pixel_local_slot"] = *access.pixel_local_slot;
            }
            if (!access.lowered_reason.empty())
            {
                dumped_access["lowered_reason"] = access.lowered_reason;
            }
            accesses.push_back(std::move(dumped_access));

            for (const auto &barrier : access.barriers)
            {
                auto dumped_barrier = DumpBarrier({{"resource", resource},
                                                   {"from_layout", Enum2Str(barrier.from_layout)},
                                                   {"to_layout", Enum2Str(barrier.to_layout)}},
                                                  barrier.from, barrier.to);
                if (const auto barrier_subresources =
                        ToString(barrier.base_mip, barrier.mip_count, barrier.base_array_layer,
                                 barrier.array_layer_count, texture.mips, texture.layers);
                    !barrier_subresources.empty())
                {
                    dumped_barrier["subresources"] = barrier_subresources;
                }
                if (access.IsPixelLocal())
                {
                    dumped_barrier["in_rendering"] = true;
                }
                barriers.push_back(std::move(dumped_barrier));
            }
        }
        for (const auto &access : pass.buffer_accesses)
        {
            const auto &resource = buffers_[access.buffer].name;
            accesses.push_back({{"resource", resource}, {"access", ToString(access.access)}});
            if (access.barrier)
            {
                barriers.push_back(DumpMemoryBarrier(resource, *access.barrier));
            }
        }
        totals.barriers += barriers.size();
        dumped["accesses"] = std::move(accesses);
        dumped["barriers"] = std::move(barriers);
        auto barriers_after = nlohmann::json::array();
        for (const auto &host_read : pass.host_reads)
        {
            if (host_read.barrier)
            {
                barriers_after.push_back(DumpMemoryBarrier(buffers_[host_read.buffer].name, *host_read.barrier));
            }
        }
        if (!barriers_after.empty())
        {
            totals.barriers += barriers_after.size();
            dumped["barriers_after"] = std::move(barriers_after);
        }
        if (pass.cpu_ms >= 0.f)
        {
            dumped["cpu_ms"] = pass.cpu_ms;
        }
        passes.push_back(std::move(dumped));
    }

    // the opportunity report: each physical pass with the rules its next one breaks, its attachment loads and stores
    // with the bytes they move at the resolved size, and its tile budget excess, the most bytes first
    std::vector<std::pair<uint64_t, std::string>> opportunities;
    auto physical_passes = nlohmann::json::array();
    for (auto index = 0u; index < physical_passes_.size(); index++)
    {
        const auto &physical = physical_passes_[index];
        const auto &info = physical.rendering_info;
        std::vector<std::string> sections;
        uint64_t moved = 0;
        auto attachments = nlohmann::json::array();
        for (const auto *access : physical.attachments)
        {
            const auto &name = textures_[access->texture.index].name;
            nlohmann::json attachment{
                {"resource", name},
                {"slot", access->slot == DepthSlot ? nlohmann::json("depth") : nlohmann::json(access->slot)},
                {"load", Enum2Str(access->load_op)},
                {"load_reason", access->load_reason},
                {"store", Enum2Str(access->store_op)},
                {"store_reason", access->store_reason}};
            if (const auto subresources = dump_subresources(*access); !subresources.empty())
            {
                attachment["subresources"] = subresources;
            }
            attachments.push_back(std::move(attachment));

            const uint64_t bytes = uint64_t{info.width} * info.height * GetPixelSize(GetFormat(access->texture));
            for (auto [moves, action, total] :
                 {std::tuple{access->load_op == RHILoadOp::Load, "Load", &totals.load_bytes},
                  std::tuple{access->store_op == RHIStoreOp::Store, "Store", &totals.store_bytes}})
            {
                if (moves)
                {
                    sections.push_back(std::format("{} {} {}", action, name, ToMegabytes(bytes)));
                    moved += bytes;
                    *total += bytes;
                }
            }
        }

        nlohmann::json dumped{{"members", physical.members}, {"attachments", std::move(attachments)}};
        std::vector<std::string> breaks;
        std::ranges::transform(physical.breaks, std::back_inserter(breaks),
                               [](const RGBreak &broken) { return broken.ToString(true); });
        if (!breaks.empty())
        {
            sections.insert(sections.begin(), Join(breaks, ", "));
            const auto &first = physical.breaks.front();
            dumped["break_reason"] = Enum2Str(first.reason);
            if (!first.resources.empty())
            {
                dumped["break_resource"] = first.resources.front();
            }
            dumped["breaks"] = std::move(breaks);
        }
        if (passes_[physical.members.front()].kind == RGPassKind::Raster)
        {
            totals.render_passes++;
            const auto color_bytes = GetColorBytesPerPixel(physical);
            dumped["color_bytes_per_pixel"] = color_bytes;
            if (tile_budget_ && color_bytes > *tile_budget_)
            {
                dumped["over_tile_budget"] = true;
                sections.push_back(std::format("{} B/pixel over the {} B tile budget", color_bytes, *tile_budget_));
            }
        }
        if (full_barriers_)
        {
            dumped["full_barrier"] = true;
        }
        if (physical.gpu_ms >= 0.f)
        {
            dumped["gpu_ms"] = physical.gpu_ms;
        }
        physical_passes.push_back(std::move(dumped));

        // e.g. "ToneMapping|Present: SlotConflict(BackBuffer), NonLocalRead(Screen); Store Screen 3.69 MB"
        auto report = passes_[physical.members.back()].name;
        if (index + 1 < physical_passes_.size())
        {
            report += "|" + passes_[physical_passes_[index + 1].members.front()].name;
        }
        opportunities.emplace_back(moved, report + (sections.empty() ? "" : ": " + Join(sections, "; ")));
    }
    std::ranges::stable_sort(opportunities, std::ranges::greater{}, &std::pair<uint64_t, std::string>::first);

    // transients sharing an image count it once
    std::vector<bool> counted(textures_.size(), false);
    auto resources = nlohmann::json::array();
    for (const auto &texture : textures_)
    {
        nlohmann::json dumped{
            {"name", texture.name}, {"type", "Texture"}, {"kind", texture.imported ? "Imported" : "Transient"}};
        if (!texture.imported)
        {
            dumped["format"] = Enum2Str(texture.desc.format);
            dumped["size_class"] = Enum2Str(texture.desc.size_class);
            if (texture.desc.size_class == RGSizeClass::Absolute)
            {
                dumped["width"] = texture.width;
                dumped["height"] = texture.height;
            }
        }
        DumpUses(dumped, texture.lifetime, JoinFlags(texture.usages));
        if (texture.physical)
        {
            const bool backed_memoryless =
                texture.image->GetAttributes().memory_properties & RHIMemoryProperty::Memoryless;
            dumped["physical"] = *texture.physical;
            dumped["memoryless"] = texture.memoryless;
            dumped["backing"] = backed_memoryless ? "memoryless" : "pooled";
            if (!counted[*texture.physical])
            {
                counted[*texture.physical] = true;
                const uint64_t bytes = uint64_t{texture.width} * texture.height * GetPixelSize(texture.desc.format);
                totals.transient_bytes += bytes;
                totals.memoryless_bytes += backed_memoryless ? bytes : 0;
            }
        }
        resources.push_back(std::move(dumped));
    }

    // buffers and acceleration structures are imports; their usage is the union of their accesses
    for (const auto &buffer : buffers_)
    {
        const bool acceleration_structure = std::holds_alternative<RHIResourceRef<RHITLAS>>(buffer.resource);
        nlohmann::json dumped{{"name", buffer.name},
                              {"type", acceleration_structure ? "AccelerationStructure" : "Buffer"},
                              {"kind", "Imported"}};
        DumpUses(dumped, buffer.lifetime, ToString(buffer.accesses));
        resources.push_back(std::move(dumped));
    }

    auto report = nlohmann::json::array();
    for (auto &[bytes, line] : opportunities)
    {
        report.push_back(std::move(line));
    }

    nlohmann::json dump{{"passes", std::move(passes)},
                        {"physical_passes", std::move(physical_passes)},
                        {"resources", std::move(resources)},
                        {"totals",
                         {{"render_passes", totals.render_passes},
                          {"barriers", totals.barriers},
                          {"load_bytes", totals.load_bytes},
                          {"store_bytes", totals.store_bytes},
                          {"transient_bytes", totals.transient_bytes},
                          {"memoryless_bytes", totals.memoryless_bytes}}},
                        {"opportunities", std::move(report)}};
    if (tile_budget_)
    {
        dump["tile_budget"] = *tile_budget_;
    }
    return dump;
}
} // namespace sparkle
