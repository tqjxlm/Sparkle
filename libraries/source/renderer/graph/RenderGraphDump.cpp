#include "renderer/graph/RenderGraph.h"

#include <nlohmann/json.hpp>

#include <array>
#include <utility>

namespace sparkle
{
template <typename Flags, size_t N>
static std::string JoinFlags(Flags flags, const std::array<std::pair<Flags, const char *>, N> &names)
{
    std::string joined;
    for (const auto &[flag, name] : names)
    {
        if (flags & flag)
        {
            joined += (joined.empty() ? "" : "|") + std::string(name);
        }
    }
    return joined.empty() ? "None" : joined;
}

static std::string ToString(const RHIResourceAccess &access)
{
    constexpr std::array<std::pair<RHIAccess, const char *>, 16> AccessNames{{
        {RHIAccess::ColorWrite, "ColorWrite"},
        {RHIAccess::DepthWrite, "DepthWrite"},
        {RHIAccess::DepthTest, "DepthTest"},
        {RHIAccess::Sampled, "Sampled"},
        {RHIAccess::StorageRead, "StorageRead"},
        {RHIAccess::StorageWrite, "StorageWrite"},
        {RHIAccess::CopySrc, "CopySrc"},
        {RHIAccess::CopyDst, "CopyDst"},
        {RHIAccess::Uniform, "Uniform"},
        {RHIAccess::VertexInput, "VertexInput"},
        {RHIAccess::IndexInput, "IndexInput"},
        {RHIAccess::IndirectArgs, "IndirectArgs"},
        {RHIAccess::AccelerationStructureBuild, "AccelerationStructureBuild"},
        {RHIAccess::AccelerationStructureRead, "AccelerationStructureRead"},
        {RHIAccess::Present, "Present"},
        {RHIAccess::HostRead, "HostRead"},
    }};
    constexpr std::array<std::pair<RHIShaderStageMask, const char *>, 3> StageNames{{
        {RHIShaderStageMask::Vertex, "Vertex"},
        {RHIShaderStageMask::Pixel, "Pixel"},
        {RHIShaderStageMask::Compute, "Compute"},
    }};

    auto name = JoinFlags(access.access, AccessNames);
    if (access.stages != RHIShaderStageMask::None)
    {
        name += "(" + JoinFlags(access.stages, StageNames) + ")";
    }
    return name;
}

static std::string ToString(RHIImage::ImageUsage usages)
{
    constexpr std::array<std::pair<RHIImage::ImageUsage, const char *>, 6> UsageNames{{
        {RHIImage::ImageUsage::ColorAttachment, "ColorAttachment"},
        {RHIImage::ImageUsage::DepthStencilAttachment, "DepthStencilAttachment"},
        {RHIImage::ImageUsage::Texture, "Texture"},
        {RHIImage::ImageUsage::UAV, "UAV"},
        {RHIImage::ImageUsage::TransferSrc, "TransferSrc"},
        {RHIImage::ImageUsage::TransferDst, "TransferDst"},
    }};
    return JoinFlags(usages, UsageNames);
}

static const char *ToString(RHILoadOp load_op)
{
    switch (load_op)
    {
    case RHILoadOp::Load:
        return "Load";
    case RHILoadOp::Clear:
        return "Clear";
    default:
        return "DontCare";
    }
}

static const char *ToString(RHIStoreOp store_op)
{
    return store_op == RHIStoreOp::Store ? "Store" : "DontCare";
}

nlohmann::json RenderGraph::Dump() const
{
    auto passes = nlohmann::json::array();
    for (const auto &pass : passes_)
    {
        nlohmann::json dumped{{"name", pass.name}, {"kind", Enum2Str(pass.kind)}, {"culled", !pass.live}};
        if (!pass.live)
        {
            dumped["cull_reason"] = pass.cull_reason;
        }

        auto accesses = nlohmann::json::array();
        auto barriers = nlohmann::json::array();
        auto attachments = nlohmann::json::array();
        for (const auto &access : pass.accesses)
        {
            const auto &resource = textures_[access.texture.index].name;
            nlohmann::json dumped_access{{"resource", resource}, {"access", ToString(access.access)}};
            if (access.clear)
            {
                dumped_access["clear"] = true;
            }
            accesses.push_back(std::move(dumped_access));

            if (access.barrier)
            {
                barriers.push_back({{"resource", resource},
                                    {"from_layout", Enum2Str(access.barrier->from_layout)},
                                    {"to_layout", Enum2Str(access.barrier->to_layout)},
                                    {"from", ToString(access.barrier->from)},
                                    {"to", ToString(access.barrier->to)}});
            }

            if (!pass.live || pass.kind != RGPassKind::Raster || access.slot == NoSlot)
            {
                continue;
            }

            const auto &info = pass.rendering_info;
            const bool depth = access.slot == DepthSlot;
            const auto load_op = depth ? info.depth_attachment.load_op : info.color_attachments[access.slot].load_op;
            const auto store_op = depth ? info.depth_attachment.store_op : info.color_attachments[access.slot].store_op;
            attachments.push_back({{"resource", resource},
                                   {"slot", depth ? nlohmann::json("depth") : nlohmann::json(access.slot)},
                                   {"load", ToString(load_op)},
                                   {"load_reason", access.load_reason},
                                   {"store", ToString(store_op)},
                                   {"store_reason", access.store_reason}});
        }
        dumped["accesses"] = std::move(accesses);
        dumped["barriers"] = std::move(barriers);
        dumped["attachments"] = std::move(attachments);
        passes.push_back(std::move(dumped));
    }

    auto resources = nlohmann::json::array();
    for (const auto &texture : textures_)
    {
        nlohmann::json dumped{{"name", texture.name}, {"kind", texture.imported ? "Imported" : "Transient"}};
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
        if (texture.first_pass)
        {
            dumped["first_use"] = passes_[*texture.first_pass].name;
            dumped["last_use"] = passes_[texture.last_pass].name;
            dumped["usage"] = ToString(texture.usages);
        }
        if (texture.physical)
        {
            dumped["physical"] = *texture.physical;
        }
        resources.push_back(std::move(dumped));
    }

    return {{"passes", std::move(passes)}, {"resources", std::move(resources)}};
}
} // namespace sparkle
