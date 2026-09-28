#include "renderer/graph/RenderGraph.h"

#include "RGCheck.h"
#include "RenderGraphInternal.h"
#include "renderer/RenderConfig.h"
#include "renderer/graph/RGPassTimers.h"
#include "renderer/graph/RGTexturePool.h"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <unordered_map>

namespace sparkle
{
static const RHIAccess AttachmentAccess = RHIAccess::ColorWrite | RHIAccess::DepthWrite | RHIAccess::DepthTest;
static const RHIAccess ShaderAccess =
    RHIAccess::Sampled | RHIAccess::StorageRead | RHIAccess::StorageWrite | RHIAccess::AccelerationStructureRead;
// what copy passes record: copies and acceleration structure builds
static const RHIAccess CopyAccess = RHIAccess::CopySrc | RHIAccess::CopyDst | RHIAccess::AccelerationStructureBuild;
static const RHIAccess ReadAccess =
    RHIAccess::DepthTest | RHIAccess::Sampled | RHIAccess::StorageRead | RHIAccess::CopySrc;

static bool KindAllows(RGPassKind kind, RHIAccess access)
{
    switch (kind)
    {
    case RGPassKind::Raster:
        return !(access & CopyAccess);
    case RGPassKind::Compute:
        return !(access & (AttachmentAccess | CopyAccess));
    case RGPassKind::Copy:
        return !(access & (AttachmentAccess | ShaderAccess));
    case RGPassKind::External:
        return true;
    default:
        UnImplemented(kind);
        return false;
    }
}

static RHIShaderStageMask GetDefaultStages(RGPassKind kind)
{
    switch (kind)
    {
    case RGPassKind::Raster:
        return RHIShaderStageMask::Pixel;
    case RGPassKind::Compute:
        return RHIShaderStageMask::Compute;
    default:
        return RHIShaderStageMask::All;
    }
}

static RHIBuffer::BufferUsage GetBufferUsage(RHIAccess access)
{
    auto usages = RHIBuffer::BufferUsage::None;
    if (access & RHIAccess::CopySrc)
    {
        usages = usages | RHIBuffer::BufferUsage::TransferSrc;
    }
    if (access & RHIAccess::CopyDst)
    {
        usages = usages | RHIBuffer::BufferUsage::TransferDst;
    }
    return usages;
}

static RHIImage::ImageUsage GetImageUsage(RHIAccess access)
{
    auto usages = RHIImage::ImageUsage::Undefined;
    const auto add = [access, &usages](RHIAccess bits, RHIImage::ImageUsage usage) {
        if (access & bits)
        {
            usages = usages | usage;
        }
    };
    add(RHIAccess::ColorWrite, RHIImage::ImageUsage::ColorAttachment);
    add(RHIAccess::DepthWrite | RHIAccess::DepthTest, RHIImage::ImageUsage::DepthStencilAttachment);
    add(RHIAccess::Sampled, RHIImage::ImageUsage::Texture);
    add(RHIAccess::StorageRead | RHIAccess::StorageWrite, RHIImage::ImageUsage::UAV);
    add(RHIAccess::CopySrc, RHIImage::ImageUsage::TransferSrc);
    add(RHIAccess::CopyDst, RHIImage::ImageUsage::TransferDst);
    return usages;
}

// the store op of a depth attachment writes it even when the pass only tests depth (DontCare may write), so barriers
// treat depth tests as depth writes
static RHIResourceAccess GetSyncAccess(const RHIResourceAccess &access)
{
    return access.access & RHIAccess::DepthTest ? RHIResourceAccess{.access = RHIAccess::DepthWrite} : access;
}

// calls `function(mip, layer)` for each subresource, mip by mip
template <typename Function> static void ForEachSubresource(const RGSubresources &subresources, Function &&function)
{
    for (auto mip = subresources.base_mip; mip < subresources.base_mip + subresources.mip_count; mip++)
    {
        for (auto layer = subresources.base_layer; layer < subresources.base_layer + subresources.layer_count; layer++)
        {
            function(mip, layer);
        }
    }
}

// for resolved counts
static bool Overlaps(const RGSubresources &a, const RGSubresources &b)
{
    return a.base_mip < b.base_mip + b.mip_count && b.base_mip < a.base_mip + a.mip_count &&
           a.base_layer < b.base_layer + b.layer_count && b.base_layer < a.base_layer + a.layer_count;
}

// for resolved counts
static bool Contains(const RGSubresources &outer, const RGSubresources &inner)
{
    return outer.base_mip <= inner.base_mip && inner.base_mip + inner.mip_count <= outer.base_mip + outer.mip_count &&
           outer.base_layer <= inner.base_layer &&
           inner.base_layer + inner.layer_count <= outer.base_layer + outer.layer_count;
}

static RGSubresources GetAllSubresources(const RHIImage &image)
{
    return {.base_mip = 0,
            .mip_count = image.GetAttributes().mip_levels,
            .base_layer = 0,
            .layer_count = static_cast<uint8_t>(image.GetArrayLayerCount())};
}

static RGSubresources GetViewedSubresources(const RHIImageView::Attribute &view)
{
    return {.base_mip = static_cast<uint8_t>(view.base_mip_level),
            .mip_count = static_cast<uint8_t>(view.mip_level_count),
            .base_layer = static_cast<uint8_t>(view.base_array_layer),
            .layer_count = static_cast<uint8_t>(view.array_layer_count)};
}

// the accesses a binding of `type` makes to a graph resource. graph buffers are only copied, so no binding may hold
// one.
static RHIAccess GetBindingAccess(RHIShaderResourceReflection::ResourceType type)
{
    switch (type)
    {
    case RHIShaderResourceReflection::ResourceType::Texture2D:
        return RHIAccess::Sampled;
    case RHIShaderResourceReflection::ResourceType::StorageImage2D:
        return RHIAccess::StorageRead | RHIAccess::StorageWrite;
    case RHIShaderResourceReflection::ResourceType::AccelerationStructure:
        return RHIAccess::AccelerationStructureRead;
    default:
        return RHIAccess::None;
    }
}

static bool ReadsContents(const RHIResourceAccess &access)
{
    return access.access & ReadAccess;
}

// whether the access depends on what earlier passes left in the texture
static bool UsesContents(const RHIResourceAccess &access, bool clear, bool fully_overwrites)
{
    return ReadsContents(access) || !(clear || (fully_overwrites && access.HasWrite()));
}

void RGBuilder::Declare(RGTextureRange texture, RHIResourceAccess access, RHIImageLayout layout, uint8_t slot,
                        std::optional<Vector4> clear)
{
    auto &pass = graph_.passes_[pass_];
    RGCheck(texture.texture.index < graph_.textures_.size(), "pass {} declares an invalid texture", pass.name);

    const auto &declared_texture = graph_.textures_[texture.texture.index];
    const auto &name = declared_texture.name;
    auto subresources = texture.subresources;
    const auto resolve = [](uint8_t base, uint8_t count, uint8_t total) {
        return count == RGSubresources::All ? static_cast<uint8_t>(total - base) : count;
    };
    subresources.mip_count = resolve(subresources.base_mip, subresources.mip_count, declared_texture.mips);
    subresources.layer_count = resolve(subresources.base_layer, subresources.layer_count, declared_texture.layers);
    RGCheck(subresources.base_mip < declared_texture.mips && subresources.mip_count > 0 &&
                subresources.base_mip + subresources.mip_count <= declared_texture.mips &&
                subresources.base_layer < declared_texture.layers && subresources.layer_count > 0 &&
                subresources.base_layer + subresources.layer_count <= declared_texture.layers,
            "pass {} declares subresources {} does not have", pass.name, name);
    RGCheck(slot == RenderGraph::NoSlot || (subresources.mip_count == 1 && subresources.layer_count == 1),
            "pass {} attaches more than one subresource of {}", pass.name, name);

    RGCheck(KindAllows(pass.kind, access.access), "a {} pass cannot declare this access to {}", Enum2Str(pass.kind),
            name);
    for (const auto &declared : pass.accesses)
    {
        RGCheck(declared.texture != texture.texture || !Overlaps(declared.subresources, subresources),
                "pass {} declares {} twice", pass.name, name);
        RGCheck(slot == RenderGraph::NoSlot || declared.slot != slot, "pass {} binds two attachments to slot {}",
                pass.name, slot);
    }

    pass.accesses.push_back({.texture = texture.texture,
                             .subresources = subresources,
                             .access = access,
                             .layout = layout,
                             .slot = slot,
                             .clear = std::move(clear),
                             .bindings = {},
                             .barriers = {},
                             .states = {},
                             .load_reason = {},
                             .store_reason = {}});
}

void RGBuilder::DeclareShaderAccess(RGTextureRange texture, RHIAccess access, RHIShaderStageMask stages,
                                    RHIImageLayout layout)
{
    const auto kind = graph_.passes_[pass_].kind;
    Declare(texture, {.access = access, .stages = stages == RHIShaderStageMask::None ? GetDefaultStages(kind) : stages},
            layout, RenderGraph::NoSlot, std::nullopt);
}

void RGBuilder::ColorWrite(RGTextureRange texture, uint8_t slot, std::optional<Vector4> clear)
{
    RGCheck(slot < MaxNumColorAttachments, "color slot {} out of range", slot);
    Declare(texture, {.access = RHIAccess::ColorWrite}, RHIImageLayout::ColorOutput, slot, std::move(clear));
}

void RGBuilder::DepthWrite(RGTextureRange texture, std::optional<float> clear)
{
    Declare(texture, {.access = RHIAccess::DepthWrite}, RHIImageLayout::DepthStencilOutput, RenderGraph::DepthSlot,
            clear ? std::optional<Vector4>(Vector4(*clear, 0.f, 0.f, 0.f)) : std::nullopt);
}

void RGBuilder::DepthTest(RGTextureRange texture)
{
    Declare(texture, {.access = RHIAccess::DepthTest}, RHIImageLayout::DepthStencilOutput, RenderGraph::DepthSlot,
            std::nullopt);
}

void RGBuilder::Sampled(RGTextureRange texture, RHIShaderStageMask stages)
{
    DeclareShaderAccess(texture, RHIAccess::Sampled, stages, RHIImageLayout::Read);
}

void RGBuilder::StorageWrite(RGTextureRange texture, RHIShaderStageMask stages)
{
    DeclareShaderAccess(texture, RHIAccess::StorageWrite, stages, RHIImageLayout::StorageWrite);
}

void RGBuilder::StorageReadWrite(RGTextureRange texture, RHIShaderStageMask stages)
{
    DeclareShaderAccess(texture, RHIAccess::StorageRead | RHIAccess::StorageWrite, stages,
                        RHIImageLayout::StorageWrite);
}

void RGBuilder::CopySrc(RGTextureRange texture)
{
    Declare(texture, {.access = RHIAccess::CopySrc}, RHIImageLayout::TransferSrc, RenderGraph::NoSlot, std::nullopt);
}

void RGBuilder::CopyDst(RGTextureRange texture)
{
    Declare(texture, {.access = RHIAccess::CopyDst}, RHIImageLayout::TransferDst, RenderGraph::NoSlot, std::nullopt);
}

void RGBuilder::DeclareBuffer(uint32_t buffer, RHIResourceAccess access)
{
    auto &pass = graph_.passes_[pass_];
    RGCheck(buffer < graph_.buffers_.size(), "pass {} declares an invalid buffer", pass.name);

    const auto &name = graph_.buffers_[buffer].name;
    RGCheck(KindAllows(pass.kind, access.access), "a {} pass cannot declare this access to {}", Enum2Str(pass.kind),
            name);
    RGCheck(
        std::ranges::none_of(pass.buffer_accesses,
                             [buffer](const RenderGraph::BufferAccess &declared) { return declared.buffer == buffer; }),
        "pass {} declares {} twice", pass.name, name);

    pass.buffer_accesses.push_back({.buffer = buffer, .access = access, .bindings = {}});
}

void RGBuilder::CopySrc(RGBuffer buffer)
{
    DeclareBuffer(buffer.index, {.access = RHIAccess::CopySrc});
}

void RGBuilder::CopyDst(RGBuffer buffer)
{
    DeclareBuffer(buffer.index, {.access = RHIAccess::CopyDst});
}

void RGBuilder::AccelerationStructureBuild(RGAccelerationStructure acceleration_structure)
{
    DeclareBuffer(acceleration_structure.index, {.access = RHIAccess::AccelerationStructureBuild});
}

void RGBuilder::AccelerationStructureRead(RGAccelerationStructure acceleration_structure, RHIShaderStageMask stages)
{
    const auto kind = graph_.passes_[pass_].kind;
    DeclareBuffer(acceleration_structure.index,
                  {.access = RHIAccess::AccelerationStructureRead,
                   .stages = stages == RHIShaderStageMask::None ? GetDefaultStages(kind) : stages});
}

RHIResourceRef<RHITLAS> RGBuilder::GetAccelerationStructure(RGAccelerationStructure acceleration_structure) const
{
    return std::get<RHIResourceRef<RHITLAS>>(graph_.buffers_[acceleration_structure.index].resource);
}

void RGBuilder::BindLastBufferAccess(RHIMemberBinding binding)
{
    graph_.passes_[pass_].buffer_accesses.back().bindings.push_back(std::move(binding));
}

RHIResourceRef<RHIImageView> RGBuilder::GetView(RHIContext *rhi, RHIImage &image, const RGSubresources &subresources,
                                                bool storage)
{
    if (!storage)
    {
        RGCheck(subresources == GetAllSubresources(image), "a sampled binding of {} views every subresource",
                image.GetName());
        return image.GetDefaultView(rhi);
    }

    RGCheck(subresources.mip_count == 1, "a storage binding of {} views one mip", image.GetName());
    const bool cube = image.GetAttributes().type == RHIImage::ImageType::Image2DCube;
    return image.GetView(
        rhi, {.type = cube ? RHIImageView::ImageViewType::Image2DArray : RHIImageView::ImageViewType::Image2D,
              .base_mip_level = subresources.base_mip,
              .mip_level_count = 1,
              .base_array_layer = subresources.base_layer,
              .array_layer_count = subresources.layer_count});
}

void RGBuilder::BindLastAccess(ImageBinding binding)
{
    graph_.passes_[pass_].accesses.back().bindings.push_back(std::move(binding));
}

void RGBuilder::BindPlaceholder(const RHIResourceRef<RHIImage> &placeholder, ImageBinding binding)
{
    graph_.passes_[pass_].placeholders.emplace_back(placeholder, std::move(binding));
}

void RGBuilder::FullyOverwrites()
{
    graph_.passes_[pass_].fully_overwrites = true;
}

void RGBuilder::SideEffect()
{
    graph_.passes_[pass_].side_effect = true;
}

void RGBuilder::NativeAccess()
{
    auto &pass = graph_.passes_[pass_];
    RGCheck(pass.kind == RGPassKind::Raster, "{} pass {} cannot declare native access", Enum2Str(pass.kind), pass.name);
    pass.native_access = true;
}

RHIImage *RGPassContext::GetImage(RGTexture texture) const
{
    const auto &pass = graph_.passes_[pass_];
    RGCheck(std::ranges::any_of(pass.accesses, [texture](const auto &access) { return access.texture == texture; }),
            "pass {} uses a texture it did not declare", pass.name);
    return graph_.textures_[texture.index].image;
}

RHIBuffer *RGPassContext::GetBuffer(RGBuffer buffer) const
{
    CheckDeclared(buffer.index);
    return std::get<RHIResourceRef<RHIBuffer>>(graph_.buffers_[buffer.index].resource).get();
}

RHITLAS *RGPassContext::GetAccelerationStructure(RGAccelerationStructure acceleration_structure) const
{
    CheckDeclared(acceleration_structure.index);
    return std::get<RHIResourceRef<RHITLAS>>(graph_.buffers_[acceleration_structure.index].resource).get();
}

void RGPassContext::CheckDeclared(uint32_t buffer) const
{
    const auto &pass = graph_.passes_[pass_];
    RGCheck(std::ranges::any_of(pass.buffer_accesses,
                                [buffer](const RenderGraph::BufferAccess &access) { return access.buffer == buffer; }),
            "pass {} uses a buffer it did not declare", pass.name);
}

RHICommandContext &RGPassContext::GetNativeContext() const
{
    const auto &pass = graph_.passes_[pass_];
    RGCheck(pass.native_access, "pass {} records raw commands without declaring native access", pass.name);
    return command_context_;
}

RenderGraph::RenderGraph(RGTexturePool &pool, const RenderConfig &config)
    : pool_(pool), resolution_(config.GetResolution()), cull_(config.render_graph_cull)
{
}

RenderGraph::~RenderGraph()
{
    if (compiled_)
    {
        pool_.EndGraph();
    }
}

RGTexture RenderGraph::CreateTexture(std::string name, const RGTextureDesc &desc)
{
    Texture texture{.name = std::move(name), .desc = desc, .imported = nullptr};
    switch (desc.size_class)
    {
    case RGSizeClass::Scene:
        texture.width = resolution_.scene.x();
        texture.height = resolution_.scene.y();
        break;
    case RGSizeClass::Output:
        texture.width = resolution_.output.x();
        texture.height = resolution_.output.y();
        break;
    case RGSizeClass::Absolute:
        texture.width = desc.width;
        texture.height = desc.height;
        break;
    default:
        UnImplemented(desc.size_class);
    }
    RGCheck(desc.format != PixelFormat::Count && texture.width > 0 && texture.height > 0, "{} has no format or size",
            texture.name);

    textures_.push_back(std::move(texture));
    return {.index = static_cast<uint32_t>(textures_.size() - 1)};
}

RGTexture RenderGraph::Import(std::string name, const RHIResourceRef<RHIImage> &image)
{
    RGCheck(image && image->GetAttributes().msaa_samples == 1, "import {} is not a single-sampled image", name);
    if (const auto found =
            std::ranges::find_if(textures_, [&image](const Texture &texture) { return texture.imported == image; });
        found != textures_.end())
    {
        return {.index = static_cast<uint32_t>(found - textures_.begin())};
    }

    textures_.push_back({.name = std::move(name),
                         .imported = image,
                         .image = image.get(),
                         .width = image->GetWidth(),
                         .height = image->GetHeight(),
                         .mips = image->GetAttributes().mip_levels,
                         .layers = static_cast<uint8_t>(image->GetArrayLayerCount())});
    return {.index = static_cast<uint32_t>(textures_.size() - 1)};
}

RGBuffer RenderGraph::Import(std::string name, const RHIResourceRef<RHIBuffer> &buffer)
{
    RGCheck(buffer, "import {} is not a buffer", name);
    return {.index = ImportBuffer({.name = std::move(name), .resource = buffer})};
}

RGAccelerationStructure RenderGraph::Import(std::string name, const RHIResourceRef<RHITLAS> &acceleration_structure)
{
    RGCheck(acceleration_structure, "import {} is not an acceleration structure", name);
    return {.index = ImportBuffer({.name = std::move(name), .resource = acceleration_structure})};
}

uint32_t RenderGraph::ImportBuffer(Buffer buffer)
{
    const auto found = std::ranges::find(buffers_, buffer.Get(), &Buffer::Get);
    if (found != buffers_.end())
    {
        return static_cast<uint32_t>(found - buffers_.begin());
    }

    buffers_.push_back(std::move(buffer));
    return static_cast<uint32_t>(buffers_.size() - 1);
}

RGTexture RenderGraph::FindTexture(std::string_view name) const
{
    const auto found = std::ranges::find(textures_, name, &Texture::name);
    return found == textures_.end() ? RGTexture{}
                                    : RGTexture{.index = static_cast<uint32_t>(found - textures_.begin())};
}

bool RenderGraph::CanSample2D(RGTexture texture) const
{
    const auto &found = textures_[texture.index];
    if (found.imported)
    {
        return found.layers == 1 && found.imported->GetAttributes().usages & RHIImage::ImageUsage::Texture;
    }
    return std::ranges::any_of(passes_, [texture](const Pass &pass) {
        return std::ranges::any_of(pass.accesses, [texture](const Access &access) {
            return access.texture == texture && access.access.HasWrite();
        });
    });
}

PixelFormat RenderGraph::GetFormat(RGTexture texture) const
{
    const auto &found = textures_[texture.index];
    return found.imported ? found.imported->GetAttributes().format : found.desc.format;
}

Vector2UInt RenderGraph::GetSize(RGTexture texture) const
{
    const auto &found = textures_[texture.index];
    return {found.width, found.height};
}

uint32_t RenderGraph::NewPass(std::string name, RGPassKind kind, RHIResourceRef<RHIComputePass> compute_pass)
{
    passes_.push_back({.name = std::move(name),
                       .kind = kind,
                       .compute_pass = std::move(compute_pass),
                       .accesses = {},
                       .buffer_accesses = {},
                       .placeholders = {},
                       .record = {},
                       .cull_reason = {},
                       .bindings = {}});
    return static_cast<uint32_t>(passes_.size() - 1);
}

void RenderGraph::SetRecord(uint32_t pass, std::function<void(RHICommandContext &)> record)
{
    passes_[pass].record = std::move(record);
}

void RenderGraph::Compile()
{
    RGCheck(!compiled_, "the graph compiles once");

    Validate();
    Cull();

    pool_.BeginGraph();
    compiled_ = true;

    ResolveTextures();
    ResolveBuffers();
    ResolveBindings();
    PlanBarriers();
    InferStoreOps();
}

void RenderGraph::Validate() const
{
    std::vector<bool> written(textures_.size(), false);
    for (const auto &pass : passes_)
    {
        RGCheck(pass.kind != RGPassKind::Raster ||
                    std::ranges::any_of(pass.accesses, [](const Access &access) { return access.slot != NoSlot; }),
                "raster pass {} has no attachment", pass.name);
        RGCheck(pass.kind != RGPassKind::Compute || pass.compute_pass, "compute pass {} has no compute pass",
                pass.name);

        for (const auto &access : pass.accesses)
        {
            const auto &texture = textures_[access.texture.index];
            RGCheck(texture.imported || written[access.texture.index] || !ReadsContents(access.access),
                    "pass {} reads {} before any pass writes it", pass.name, texture.name);
            written[access.texture.index] = written[access.texture.index] || access.access.HasWrite();
        }
    }
}

// a pass lives when it has a side effect, writes an import, or writes contents a later live pass uses
void RenderGraph::Cull()
{
    std::vector<bool> needed(textures_.size(), false);
    for (auto &pass : passes_ | std::views::reverse)
    {
        const auto keeps_alive = [this, &needed](const Access &access) {
            return access.access.HasWrite() &&
                   (textures_[access.texture.index].imported || needed[access.texture.index]);
        };
        // buffers and acceleration structures are imports
        const auto writes_buffer = [](const BufferAccess &access) { return access.access.HasWrite(); };
        pass.live = !cull_ || pass.side_effect || std::ranges::any_of(pass.accesses, keeps_alive) ||
                    std::ranges::any_of(pass.buffer_accesses, writes_buffer);
        if (!pass.live)
        {
            std::string unread;
            for (const auto &access :
                 pass.accesses | std::views::filter([](const Access &access) { return access.access.HasWrite(); }))
            {
                unread += (unread.empty() ? "" : ", ") + textures_[access.texture.index].name;
            }
            pass.cull_reason = unread.empty() ? "no outputs" : "unread outputs: " + unread;
            continue;
        }

        for (const auto &access : pass.accesses)
        {
            needed[access.texture.index] = UsesContents(access.access, access.clear.has_value(), pass.fully_overwrites);
        }
    }
}

// transients whose lifetimes do not overlap share an image when format, extent and sampler match
void RenderGraph::ResolveTextures()
{
    for (auto pass_index = 0u; pass_index < passes_.size(); pass_index++)
    {
        for (const auto &access : passes_[pass_index].accesses)
        {
            auto &texture = textures_[access.texture.index];
            if (passes_[pass_index].live)
            {
                texture.first_pass = texture.first_pass.value_or(pass_index);
                texture.last_pass = pass_index;
                texture.usages = texture.usages | GetImageUsage(access.access.access);
            }
        }
    }

    struct Physical
    {
        const Texture *first;
        RHIImage::ImageUsage usages;
        uint32_t last_pass;
    };

    std::vector<Physical> physicals;

    // in order of first use, so the assignment is the same every frame
    for (auto pass_index = 0u; pass_index < passes_.size(); pass_index++)
    {
        for (const auto &access : passes_[pass_index].accesses)
        {
            auto &texture = textures_[access.texture.index];
            if (texture.imported || texture.first_pass != pass_index || texture.physical)
            {
                continue;
            }

            const auto found = std::ranges::find_if(physicals, [&texture, pass_index](const Physical &physical) {
                const auto &first = *physical.first;
                return physical.last_pass < pass_index && first.desc.format == texture.desc.format &&
                       first.width == texture.width && first.height == texture.height &&
                       first.desc.sampler == texture.desc.sampler;
            });
            if (found == physicals.end())
            {
                texture.physical = static_cast<uint32_t>(physicals.size());
                physicals.push_back({.first = &texture, .usages = texture.usages, .last_pass = texture.last_pass});
            }
            else
            {
                texture.physical = static_cast<uint32_t>(found - physicals.begin());
                found->usages = found->usages | texture.usages;
                found->last_pass = texture.last_pass;
            }
        }
    }

    std::vector<RHIImage *> images;
    images.reserve(physicals.size());
    for (const auto &physical : physicals)
    {
        const auto &first = *physical.first;
        images.push_back(pool_.Acquire({.format = first.desc.format,
                                        .width = first.width,
                                        .height = first.height,
                                        .sampler = first.desc.sampler,
                                        .usages = physical.usages},
                                       first.name));
    }

    for (auto &texture : textures_)
    {
        if (texture.physical)
        {
            texture.image = images[*texture.physical];
        }
        RGCheck(!texture.imported || !(texture.usages & ~texture.image->GetAttributes().usages),
                "import {} lacks the usages its accesses need", texture.name);
    }
}

void RenderGraph::ResolveBuffers()
{
    for (auto pass_index = 0u; pass_index < passes_.size(); pass_index++)
    {
        if (!passes_[pass_index].live)
        {
            continue;
        }

        for (const auto &access : passes_[pass_index].buffer_accesses)
        {
            auto &buffer = buffers_[access.buffer];
            buffer.first_pass = buffer.first_pass.value_or(pass_index);
            buffer.last_pass = pass_index;
            buffer.accesses = buffer.accesses | access.access;
        }
    }

    for (const auto &buffer : buffers_)
    {
        const auto *imported = std::get_if<RHIResourceRef<RHIBuffer>>(&buffer.resource);
        RGCheck(imported == nullptr || !(GetBufferUsage(buffer.accesses.access) & ~(*imported)->GetUsage()),
                "import {} lacks the usages its accesses need", buffer.name);
    }
}

// image views are created at compile, so recording only binds them
void RenderGraph::ResolveBindings()
{
    for (auto &pass : passes_ | std::views::filter(&Pass::live))
    {
        for (const auto &access : pass.accesses)
        {
            for (const auto &binding : access.bindings)
            {
                pass.bindings.push_back(
                    binding(pool_.rhi_, *textures_[access.texture.index].image, access.subresources));
            }
        }
        for (const auto &access : pass.buffer_accesses)
        {
            std::ranges::copy(access.bindings, std::back_inserter(pass.bindings));
        }
        for (const auto &[image, binding] : pass.placeholders)
        {
            pass.bindings.push_back(binding(pool_.rhi_, *image, GetAllSubresources(*image)));
        }
    }
}

// each access transitions its subresources from the states the previous accesses left, seeded from the tracked states.
// writes to contents nobody may use again discard them.
void RenderGraph::PlanBarriers()
{
    // per image, the planned state of each subresource, layer by layer within a mip
    std::unordered_map<const RHIImage *, std::vector<RHIImageState>> states;
    std::vector<const Pass *> last_writer(textures_.size(), nullptr);
    std::vector<RHIResourceAccess> buffer_states;
    std::ranges::transform(buffers_, std::back_inserter(buffer_states),
                           [](const Buffer &buffer) { return buffer.GetTracked().GetTrackedAccess(); });

    for (auto &pass : passes_ | std::views::filter(&Pass::live))
    {
        // memory barriers follow the image rule without layouts. a build also waits for earlier builds: the BLAS it
        // reads, submitted before the frame, and the scratch memory it reuses.
        for (auto &access : pass.buffer_accesses)
        {
            auto &state = buffer_states[access.buffer];
            if (const auto next = TransitionImageState({.layout = RHIImageLayout::Undefined, .access = state},
                                                       {.layout = RHIImageLayout::Undefined, .access = access.access}))
            {
                const bool build = access.access.access & RHIAccess::AccelerationStructureBuild;
                const auto from = build ? state | access.access : state;
                if (from.access != RHIAccess::None)
                {
                    access.barrier = RHIMemoryBarrier{.from = from, .to = access.access};
                }
                state = next->access;
            }
            access.state = state;
        }

        for (auto &access : pass.accesses)
        {
            const auto &texture = textures_[access.texture.index];
            const auto *image = texture.image;
            auto [found, inserted] = states.try_emplace(image);
            auto &image_states = found->second;
            if (inserted)
            {
                ForEachSubresource(GetAllSubresources(*image), [image, &image_states](unsigned mip, unsigned layer) {
                    image_states.push_back(image->GetState(mip, layer));
                });
            }

            const auto *writer = last_writer[access.texture.index];
            const bool uses_contents = UsesContents(access.access, access.clear.has_value(), pass.fully_overwrites);
            const bool discard = !uses_contents || (access.access.HasWrite() && writer == nullptr && !texture.imported);
            PlanAccess(access, discard, image_states);

            const bool attachment = access.slot != NoSlot;
            if (access.access.HasWrite())
            {
                last_writer[access.texture.index] = &pass;
            }

            if (pass.kind != RGPassKind::Raster || !attachment)
            {
                continue;
            }

            RHILoadOp load_op = RHILoadOp::Load;
            if (access.clear)
            {
                load_op = RHILoadOp::Clear;
                access.load_reason = "clear";
            }
            else if (!uses_contents)
            {
                load_op = RHILoadOp::DontCare;
                access.load_reason = "fully overwritten";
            }
            else if (discard)
            {
                load_op = RHILoadOp::DontCare;
                access.load_reason = "no earlier writer";
            }
            else
            {
                access.load_reason = writer ? "written by " + writer->name : "imported";
            }

            const auto mip = access.subresources.base_mip;
            const auto layer = access.subresources.base_layer;
            const auto width = std::max(texture.width >> mip, 1u);
            const auto height = std::max(texture.height >> mip, 1u);
            auto &info = pass.rendering_info;
            RGCheck(info.width == 0 || (info.width == width && info.height == height),
                    "attachments of pass {} differ in size", pass.name);
            info.width = width;
            info.height = height;
            if (access.slot == DepthSlot)
            {
                info.depth_attachment = {.image = texture.image,
                                         .mip_level = mip,
                                         .array_layer = layer,
                                         .load_op = load_op,
                                         .clear_depth = access.clear ? access.clear->x() : 1.f};
            }
            else
            {
                info.color_attachments[access.slot] = {.image = texture.image,
                                                       .mip_level = mip,
                                                       .array_layer = layer,
                                                       .load_op = load_op,
                                                       .clear_color =
                                                           access.clear.value_or(Vector4(0.f, 0.f, 0.f, 1.f))};
            }
        }
    }
}

// the subresources of each access move into its layout and access: with one barrier when they share a state, otherwise
// one per run of mips in one state within a layer
void RenderGraph::PlanAccess(Access &access, bool discard, std::vector<RHIImageState> &states) const
{
    const auto *image = textures_[access.texture.index].image;
    const auto layers = image->GetArrayLayerCount();
    const auto state_of = [&states, layers](unsigned mip, unsigned layer) -> RHIImageState & {
        return states[mip * layers + layer];
    };
    const RHIImageState target{.layout = access.layout, .access = GetSyncAccess(access.access)};

    const auto transition = [&](RGSubresources subresources) {
        const auto state = state_of(subresources.base_mip, subresources.base_layer);
        const auto next = TransitionImageState(state, target);
        if (!next)
        {
            return;
        }

        access.barriers.push_back({.image = image,
                                   .base_mip = subresources.base_mip,
                                   .mip_count = subresources.mip_count,
                                   .base_array_layer = subresources.base_layer,
                                   .array_layer_count = subresources.layer_count,
                                   .from = state.access,
                                   .to = target.access,
                                   .from_layout = discard ? RHIImageLayout::Undefined : state.layout,
                                   .to_layout = access.layout});
        ForEachSubresource(subresources,
                           [&state_of, &next](unsigned mip, unsigned layer) { state_of(mip, layer) = *next; });
    };

    const auto &subresources = access.subresources;
    const auto first = state_of(subresources.base_mip, subresources.base_layer);
    bool uniform = true;
    ForEachSubresource(subresources, [&state_of, &first, &uniform](unsigned mip, unsigned layer) {
        uniform = uniform && state_of(mip, layer) == first;
    });

    if (uniform)
    {
        transition(subresources);
    }
    else
    {
        const unsigned mip_end = subresources.base_mip + subresources.mip_count;
        const unsigned layer_end = subresources.base_layer + subresources.layer_count;
        for (unsigned layer = subresources.base_layer; layer < layer_end; layer++)
        {
            for (unsigned mip = subresources.base_mip; mip < mip_end;)
            {
                auto run_end = mip + 1;
                while (run_end < mip_end && state_of(run_end, layer) == state_of(mip, layer))
                {
                    run_end++;
                }
                transition({.base_mip = static_cast<uint8_t>(mip),
                            .mip_count = static_cast<uint8_t>(run_end - mip),
                            .base_layer = static_cast<uint8_t>(layer),
                            .layer_count = 1});
                mip = run_end;
            }
        }
    }

    ForEachSubresource(subresources, [&access, &state_of](unsigned mip, unsigned layer) {
        access.states.push_back(state_of(mip, layer));
    });
}

// an attachment is stored when the next live pass touching it uses its contents, or when it is imported
void RenderGraph::InferStoreOps()
{
    for (auto pass_index = 0u; pass_index < passes_.size(); pass_index++)
    {
        auto &pass = passes_[pass_index];
        if (!pass.live || pass.kind != RGPassKind::Raster)
        {
            continue;
        }

        for (auto &access : pass.accesses | std::views::filter([](const Access &a) { return a.slot != NoSlot; }))
        {
            auto store_op = RHIStoreOp::DontCare;
            access.store_reason = "no later reader";
            if (textures_[access.texture.index].imported)
            {
                store_op = RHIStoreOp::Store;
                access.store_reason = "imported";
            }

            for (const auto &later : passes_ | std::views::drop(pass_index + 1) | std::views::filter(&Pass::live))
            {
                const auto next = std::ranges::find_if(later.accesses, [&access](const Access &a) {
                    return a.texture == access.texture && Overlaps(a.subresources, access.subresources);
                });
                if (next == later.accesses.end())
                {
                    continue;
                }

                if (UsesContents(next->access, next->clear.has_value(), later.fully_overwrites))
                {
                    store_op = RHIStoreOp::Store;
                    access.store_reason = "read by " + later.name;
                }
                else
                {
                    store_op = RHIStoreOp::DontCare;
                    access.store_reason = "overwritten by " + later.name;
                }
                break;
            }

            auto &info = pass.rendering_info;
            if (access.slot == DepthSlot)
            {
                info.depth_attachment.store_op = store_op;
            }
            else
            {
                info.color_attachments[access.slot].store_op = store_op;
            }
        }
    }
}

// the graph writes each pass's planned state through to the tracked state before recording the pass, so foreign code
// and the next frame start from it
void RenderGraph::Execute(RHICommandContext &command_context, RGPassTimers *timers)
{
    RGCheck(compiled_ && !executed_, "the graph executes once, after compiling");
    executed_ = true;

    for (auto &pass : passes_ | std::views::filter(&Pass::live))
    {
        std::vector<RHIImageBarrier> barriers;
        for (const auto &access : pass.accesses)
        {
            barriers.insert(barriers.end(), access.barriers.begin(), access.barriers.end());
            auto *image = textures_[access.texture.index].image;
            auto state = access.states.begin();
            ForEachSubresource(access.subresources, [image, &state](unsigned mip, unsigned layer) {
                image->SetState(*state++, mip, 1, layer, 1);
            });
        }
        std::vector<RHIMemoryBarrier> memory_barriers;
        for (const auto &access : pass.buffer_accesses)
        {
            if (access.barrier)
            {
                memory_barriers.push_back(*access.barrier);
            }
            buffers_[access.buffer].GetTracked().SetTrackedAccess(access.state);
        }

        command_context.SetBindings(pass.bindings);
        RHIPass *timed_pass = nullptr;
        switch (pass.kind)
        {
        case RGPassKind::Raster:
            timed_pass = timers ? timers->Get(pass.name) : nullptr;
            command_context.BeginRendering(pass.rendering_info, pass.name, timed_pass, barriers, memory_barriers);
            pass.record(command_context);
            command_context.EndRendering();
            break;
        case RGPassKind::Compute:
            timed_pass = pass.compute_pass.get();
            command_context.BeginComputePass(pass.compute_pass, barriers, memory_barriers);
            pass.record(command_context);
            command_context.EndComputePass(pass.compute_pass);
            break;
        case RGPassKind::Copy:
        case RGPassKind::External: {
            const RHICommandContext::DebugLabelScope label(command_context, pass.name);
            command_context.Barrier(barriers, memory_barriers);
            pass.record(command_context);
            break;
        }
        default:
            UnImplemented(pass.kind);
        }
        CheckDeclaredStates(pass);
        CheckBindingsApplied(pass, command_context);
        CheckBoundResourcesDeclared(pass, command_context);
        command_context.SetBindings({});

        if (timed_pass)
        {
            pass.gpu_ms = timed_pass->GetExecutionTime();
        }
    }
}

// a declared binding that no pipeline the pass drew or dispatched has would bind nothing. a pass that drew nothing (an
// empty scene) bound nothing to check.
void RenderGraph::CheckBindingsApplied(const Pass &pass, const RHICommandContext &command_context) const
{
    if (command_context.GetPipelines().empty())
    {
        return;
    }

    size_t index = 0;
    const auto check = [&pass, &command_context, &index](size_t count, const std::string &resource) {
        for (const auto end = index + count; index < end; index++)
        {
            RGCheck(command_context.IsBindingApplied(index),
                    "pass {} binds {} to a resource table no pipeline it drew or dispatched has", pass.name, resource);
        }
    };
    for (const auto &access : pass.accesses)
    {
        check(access.bindings.size(), textures_[access.texture.index].name);
    }
    for (const auto &access : pass.buffer_accesses)
    {
        check(access.bindings.size(), buffers_[access.buffer].name);
    }
}

// a pipeline's resource tables keep what earlier passes and frames bound, so a raster or compute pass must declare
// every graph resource left bound in a pipeline it drew or dispatched. external passes bind foreign resources.
void RenderGraph::CheckBoundResourcesDeclared(const Pass &pass, const RHICommandContext &command_context) const
{
    if (pass.kind != RGPassKind::Raster && pass.kind != RGPassKind::Compute)
    {
        return;
    }

    for (const auto *pipeline : command_context.GetPipelines())
    {
        for (const auto &table : pipeline->GetResourceTables())
        {
            if (!table)
            {
                continue;
            }
            for (const auto *binding : table->GetBindings())
            {
                if (binding->GetResource() && !binding->IsBindless())
                {
                    CheckBindingDeclared(pass, *binding);
                }
            }
        }
    }
}

// the pass must declare a graph resource bound at `binding` with an access the binding makes, over the subresources
// its view covers
void RenderGraph::CheckBindingDeclared(const Pass &pass, const RHIShaderResourceBinding &binding) const
{
    const auto check = [&pass, &binding](bool declared, const std::string &name) {
        RGCheck(declared, "pass {} binds {} to {} without declaring the access that binding makes", pass.name, name,
                binding.GetReflection()->name);
    };
    const auto type = binding.GetType();
    const auto needed = GetBindingAccess(type);

    if (type == RHIShaderResourceReflection::ResourceType::Texture2D ||
        type == RHIShaderResourceReflection::ResourceType::StorageImage2D)
    {
        const auto *view = static_cast<const RHIImageView *>(binding.GetResource());
        const auto texture = std::ranges::find(textures_, view->GetImage(), &Texture::image);
        if (texture != textures_.end())
        {
            const auto viewed = GetViewedSubresources(view->GetAttribute());
            check(std::ranges::any_of(pass.accesses,
                                      [this, view, needed, &viewed](const Access &access) {
                                          return textures_[access.texture.index].image == view->GetImage() &&
                                                 access.access.access & needed && Contains(access.subresources, viewed);
                                      }),
                  texture->name);
        }
        return;
    }

    const auto *resource = binding.GetResource();
    const auto buffer = std::ranges::find(buffers_, resource, &Buffer::Get);
    if (buffer != buffers_.end())
    {
        const auto index = static_cast<uint32_t>(buffer - buffers_.begin());
        check(std::ranges::any_of(pass.buffer_accesses,
                                  [index, needed](const BufferAccess &access) {
                                      return access.buffer == index && access.access.access & needed;
                                  }),
              buffer->name);
    }
}

// a pass that transitions a declared image behind the graph's back would desync the plan from the tracked state
void RenderGraph::CheckDeclaredStates(const Pass &pass) const
{
    for (const auto &access : pass.accesses)
    {
        const auto &texture = textures_[access.texture.index];
        auto planned = access.states.begin();
        ForEachSubresource(access.subresources, [&pass, &texture, &planned](unsigned mip, unsigned layer) {
            const auto state = texture.image->GetState(mip, layer);
            RGCheck(state.layout == planned->layout && planned->access.Contains(state.access),
                    "pass {} left {} in layout {} with accesses beyond its declaration", pass.name, texture.name,
                    Enum2Str(state.layout));
            planned++;
        });
    }
}
} // namespace sparkle
