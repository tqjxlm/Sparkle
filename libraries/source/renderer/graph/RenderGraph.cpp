#include "renderer/graph/RenderGraph.h"

#include "RGCheck.h"
#include "RenderGraphInternal.h"
#include "renderer/RenderConfig.h"
#include "renderer/graph/RGPassTimers.h"
#include "renderer/graph/RGTexturePool.h"
#include "rhi/RHI.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <ranges>
#include <unordered_map>

namespace sparkle
{
static constexpr RHIAccess AttachmentAccess = RHIAccess::ColorWrite | RHIAccess::DepthWrite | RHIAccess::DepthTest;
static constexpr RHIAccess ShaderAccess =
    RHIAccess::Sampled | RHIAccess::StorageRead | RHIAccess::StorageWrite | RHIAccess::AccelerationStructureRead;
// what copy passes record: copies and acceleration structure builds
static constexpr RHIAccess CopyAccess = RHIAccess::CopySrc | RHIAccess::CopyDst | RHIAccess::AccelerationStructureBuild;
static constexpr RHIAccess ReadAccess =
    RHIAccess::DepthTest | RHIAccess::Sampled | RHIAccess::StorageRead | RHIAccess::CopySrc | RHIAccess::PixelLocalRead;
// the usages of a transient whose contents may stay in tile memory
static constexpr auto MemorylessUsages = RHIImage::ImageUsage::ColorAttachment |
                                         RHIImage::ImageUsage::DepthStencilAttachment |
                                         RHIImage::ImageUsage::InputAttachment;

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

// the given stages, or the pass kind's when none are given
static RHIShaderStageMask ResolveStages(RGPassKind kind, RHIShaderStageMask stages)
{
    if (stages != RHIShaderStageMask::None)
    {
        return stages;
    }

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
    add(RHIAccess::PixelLocalRead, RHIImage::ImageUsage::InputAttachment);
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

bool RenderGraph::Access::Overlaps(const Access &other) const
{
    return texture == other.texture && sparkle::Overlaps(subresources, other.subresources);
}

static Vector2UInt GetMipSize(uint32_t width, uint32_t height, unsigned mip)
{
    return {std::max(width >> mip, 1u), std::max(height >> mip, 1u)};
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
    case RHIShaderResourceReflection::ResourceType::InputAttachment:
        return RHIAccess::PixelLocalRead;
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

// whether one of `accesses` names `resource` through `member`
template <typename Accesses, typename Resource, typename Member>
static bool Declares(const Accesses &accesses, const Resource &resource, Member member)
{
    return std::ranges::find(accesses, resource, member) != std::ranges::end(accesses);
}

// matches an access attaching the subresources of `access` at `slot`
template <typename Access> static auto AttachmentOf(const Access &access, uint8_t slot)
{
    return [&access, slot](const Access &a) { return a.slot == slot && a.SameSubresources(access); };
}

// the resource behind a buffer or acceleration structure handle, which must name a resource of that kind
template <typename Resource, typename Buffer> static const RHIResourceRef<Resource> &GetResource(const Buffer &buffer)
{
    const auto *resource = std::get_if<RHIResourceRef<Resource>>(&buffer.resource);
    RGCheck(resource != nullptr, "{} is not {}", buffer.name,
            std::is_same_v<Resource, RHITLAS> ? "an acceleration structure" : "a buffer");
    return *resource;
}

// aborts unless a pass of `kind` may declare `access` to `name` and has not declared it yet
static void CheckDeclarable(RGPassKind kind, const std::string &pass, const std::string &name, RHIAccess access,
                            bool declared)
{
    RGCheck(KindAllows(kind, access), "a {} pass cannot declare this access to {}", Enum2Str(kind), name);
    RGCheck(!declared, "pass {} declares {} twice", pass, name);
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

    CheckDeclarable(pass.kind, pass.name, name, access.access,
                    std::ranges::any_of(pass.accesses, [&texture, &subresources](const RenderGraph::Access &declared) {
                        return declared.texture == texture.texture && Overlaps(declared.subresources, subresources);
                    }));
    RGCheck(slot == RenderGraph::NoSlot || !Declares(pass.accesses, slot, &RenderGraph::Access::slot),
            "pass {} binds two attachments to slot {}", pass.name, slot);

    pass.accesses.push_back({.texture = texture.texture,
                             .subresources = subresources,
                             .access = access,
                             .layout = layout,
                             .slot = slot,
                             .clear = std::move(clear),
                             .bindings = {},
                             .sampled_bindings = {},
                             .barriers = {},
                             .states = {},
                             .load_reason = {},
                             .store_reason = {},
                             .lowered_reason = {}});
}

void RGBuilder::PixelLocalRead(RGTextureRange texture, uint8_t slot)
{
    const auto &pass = graph_.passes_[pass_];
    RGCheck(pass.kind == RGPassKind::Raster, "{} pass {} cannot read pixel-locally", Enum2Str(pass.kind), pass.name);
    RGCheck(slot < MaxNumColorAttachments, "color slot {} out of range", slot);
    DeclareShaderAccess(texture, RHIAccess::Sampled, RHIShaderStageMask::Pixel, RHIImageLayout::Read);
    graph_.passes_[pass_].accesses.back().pixel_local_slot = slot;
}

void RGBuilder::DeclareShaderAccess(RGTextureRange texture, RHIAccess access, RHIShaderStageMask stages,
                                    RHIImageLayout layout)
{
    const auto kind = graph_.passes_[pass_].kind;
    Declare(texture, {.access = access, .stages = ResolveStages(kind, stages)}, layout, RenderGraph::NoSlot,
            std::nullopt);
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
    CheckDeclarable(pass.kind, pass.name, name, access.access,
                    Declares(pass.buffer_accesses, buffer, &RenderGraph::BufferAccess::buffer));

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
                  {.access = RHIAccess::AccelerationStructureRead, .stages = ResolveStages(kind, stages)});
}

RHIResourceRef<RHITLAS> RGBuilder::GetAccelerationStructure(RGAccelerationStructure acceleration_structure) const
{
    return GetResource<RHITLAS>(graph_.buffers_[acceleration_structure.index]);
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

RHIResourceRef<RHISampler> RGBuilder::GetSampler(RHIContext *rhi, const RHISampler::SamplerAttribute &sampler)
{
    return rhi->GetSampler(sampler);
}

void RGBuilder::BindLastAccess(ImageBinding binding)
{
    graph_.passes_[pass_].accesses.back().bindings.push_back(std::move(binding));
}

void RGBuilder::BindLastAccessWhenSampled(ImageBinding binding)
{
    graph_.passes_[pass_].accesses.back().sampled_bindings.push_back(std::move(binding));
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
    RGCheck(Declares(pass.accesses, texture, &RenderGraph::Access::texture),
            "pass {} uses a texture it did not declare", pass.name);
    return graph_.textures_[texture.index].image;
}

RHIBuffer *RGPassContext::GetBuffer(RGBuffer buffer) const
{
    CheckDeclared(buffer.index);
    return GetResource<RHIBuffer>(graph_.buffers_[buffer.index]).get();
}

RHITLAS *RGPassContext::GetAccelerationStructure(RGAccelerationStructure acceleration_structure) const
{
    CheckDeclared(acceleration_structure.index);
    return GetResource<RHITLAS>(graph_.buffers_[acceleration_structure.index]).get();
}

void RGPassContext::CheckDeclared(uint32_t buffer) const
{
    const auto &pass = graph_.passes_[pass_];
    RGCheck(Declares(pass.buffer_accesses, buffer, &RenderGraph::BufferAccess::buffer),
            "pass {} uses a buffer it did not declare", pass.name);
}

RHICommandContext &RGPassContext::GetNativeContext() const
{
    const auto &pass = graph_.passes_[pass_];
    RGCheck(pass.native_access, "pass {} records raw commands without declaring native access", pass.name);
    return command_context_;
}

bool RGPassContext::IsPixelLocal(RGTexture texture) const
{
    const auto &pass = graph_.passes_[pass_];
    const auto read = std::ranges::find_if(pass.accesses, [texture](const RenderGraph::Access &access) {
        return access.texture == texture && access.pixel_local_slot.has_value();
    });
    RGCheck(read != pass.accesses.end(), "pass {} has no pixel-local read of {}", pass.name,
            graph_.textures_[texture.index].name);
    return read->IsPixelLocal();
}

RenderGraph::RenderGraph(RHIContext *rhi, RGTexturePool &pool, const RenderConfig &config)
    : rhi_(rhi), pool_(pool), resolution_(config.GetResolution()), cull_(config.render_graph_cull),
      merge_(config.render_graph_merge), memoryless_(config.render_graph_memoryless),
      pixel_local_(config.render_graph_pixel_local && rhi->SupportsPixelLocalRead()),
      full_barriers_(config.render_graph_full_barriers),
      tile_budget_(config.render_graph_tile_budget > 0 ? std::optional(config.render_graph_tile_budget)
                                                       : rhi->GetTileBudget()),
      tile_budget_split_(config.render_graph_tile_budget_split)
{
}

RenderGraph::~RenderGraph()
{
    if (pool_serves_)
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

void RenderGraph::ReadOnHost(RGBuffer buffer)
{
    RGCheck(buffer.index < buffers_.size(), "the host reads an invalid buffer");
    buffers_[buffer.index].read_on_host = true;
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

const RenderGraph::Texture &RenderGraph::GetTexture(RGTexture texture) const
{
    RGCheck(texture.index < textures_.size(), "an invalid texture has no format or size");
    return textures_[texture.index];
}

bool RenderGraph::CanSample2D(RGTexture texture) const
{
    const auto &found = GetTexture(texture);
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
    const auto &found = GetTexture(texture);
    return found.imported ? found.imported->GetAttributes().format : found.desc.format;
}

Vector2UInt RenderGraph::GetSize(RGTexture texture) const
{
    const auto &found = GetTexture(texture);
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
                       .bindings = {},
                       .bound_resources = {},
                       .host_reads = {}});
    return static_cast<uint32_t>(passes_.size() - 1);
}

void RenderGraph::SetRecord(uint32_t pass, std::function<void(RHICommandContext &)> record)
{
    passes_[pass].record = std::move(record);
}

void RenderGraph::Compile()
{
    RGCheck(!pool_serves_, "the graph compiles once");

    Validate();
    Cull();
    FormPhysicalPasses();

    pool_.BeginGraph();
    pool_serves_ = true;

    ResolveTextures();
    ResolveBuffers();
    ResolveBindings();
    PlanBarriers();
    InferStoreOps();
    BuildRenderingInfos();
    compiled_ = true;
}

void RenderGraph::Validate() const
{
    std::vector<bool> written(textures_.size(), false);
    for (const auto &pass : passes_)
    {
        RGCheck(pass.kind != RGPassKind::Raster || std::ranges::any_of(pass.accesses, &Access::IsAttachment),
                "raster pass {} has no attachment", pass.name);
        RGCheck(pass.kind != RGPassKind::Compute || pass.compute_pass, "compute pass {} has no compute pass",
                pass.name);

        for (const auto &access : pass.accesses)
        {
            const auto &texture = textures_[access.texture.index];
            RGCheck(texture.imported || written[access.texture.index] || !ReadsContents(access.access),
                    "pass {} reads {} before any pass writes it", pass.name, texture.name);
            written[access.texture.index] = written[access.texture.index] || access.access.HasWrite();
            RGCheck(!access.pixel_local_slot || !Declares(pass.accesses, *access.pixel_local_slot, &Access::slot),
                    "pass {} reads slot {} pixel-locally, which it writes", pass.name,
                    access.pixel_local_slot.value_or(0));
        }
    }
}

// a pass lives when it has a side effect, writes an import, or writes contents a later live pass uses
void RenderGraph::Cull()
{
    // per texture, not per subresource: exact while transients have one subresource; imports keep every writer anyway
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
            const auto index = access.texture.index;
            needed[index] =
                ReadsContents(access.access) ||
                (needed[index] && UsesContents(access.access, access.clear.has_value(), pass.fully_overwrites));
        }
    }
}

template <typename Matches>
const RenderGraph::Access *RenderGraph::FindAccess(std::span<const uint32_t> members, const Matches &matches) const
{
    for (const auto member : members)
    {
        const auto &accesses = passes_[member].accesses;
        if (const auto found = std::ranges::find_if(accesses, matches); found != accesses.end())
        {
            return &*found;
        }
    }
    return nullptr;
}

Vector2UInt RenderGraph::GetAttachmentSize(const Access &attachment) const
{
    const auto &texture = textures_[attachment.texture.index];
    return GetMipSize(texture.width, texture.height, attachment.subresources.base_mip);
}

// consecutive members attaching the same subresources at the same slots share a step, unless the later one reads
// pixel-locally
void RenderGraph::FormPhysicalPasses()
{
    // whether `a` attaches every subresource `b` attaches, at the same slot
    const auto attaches_all_of = [](const Pass &a, const Pass &b) {
        return std::ranges::all_of(
            b.accesses | std::views::filter(&Access::IsAttachment), [&a](const Access &attachment) {
                return std::ranges::any_of(a.accesses, AttachmentOf(attachment, attachment.slot));
            });
    };

    for (auto index = 0u; index < passes_.size(); index++)
    {
        auto &pass = passes_[index];
        if (!pass.live)
        {
            continue;
        }

        if (!physical_passes_.empty())
        {
            auto &current = physical_passes_.back();
            current.breaks = FindBreaks(current, pass);
            if (current.breaks.empty())
            {
                const auto &previous = passes_[current.members.back()];
                pass.physical = previous.physical;
                ResolvePixelLocalReads(index);
                const bool reads_locally = std::ranges::any_of(pass.accesses, &Access::IsPixelLocal);
                pass.step =
                    previous.step +
                    (attaches_all_of(previous, pass) && attaches_all_of(pass, previous) && !reads_locally ? 0 : 1);
                current.members.push_back(index);
                current.name += "+" + pass.name;
                continue;
            }
        }

        pass.physical = static_cast<uint32_t>(physical_passes_.size());
        physical_passes_.push_back({.members = {index}, .name = pass.name, .breaks = {}, .attachments = {}});
        ResolvePixelLocalReads(index);
    }
}

bool RenderGraph::ReadsAttachment(const PhysicalPass &physical, const Access &access) const
{
    return access.pixel_local_slot &&
           FindAccess(physical.members, AttachmentOf(access, *access.pixel_local_slot)) != nullptr;
}

// a read of what an earlier member attaches becomes PixelLocalRead in the LocalRead layout, which the attachment takes
// for the whole physical pass. any other request stays Sampled, lowered for the reason the physical pass of the
// texture's last writer ended: its rules (FindBreak) keep a writer at the slot read in the reader's physical pass.
void RenderGraph::ResolvePixelLocalReads(uint32_t index)
{
    auto &pass = passes_[index];
    auto &physical = physical_passes_[pass.physical];
    for (auto &access :
         pass.accesses | std::views::filter([](const Access &a) { return a.pixel_local_slot.has_value(); }))
    {
        if (ReadsAttachment(physical, access))
        {
            access.access.access = RHIAccess::PixelLocalRead;
            access.layout = RHIImageLayout::LocalRead;
            for (const auto member : physical.members)
            {
                for (auto &attachment : passes_[member].accesses)
                {
                    if (attachment.IsAttachment() && attachment.SameSubresources(access))
                    {
                        attachment.layout = RHIImageLayout::LocalRead;
                    }
                }
            }
            continue;
        }

        std::ranges::move(access.sampled_bindings, std::back_inserter(access.bindings));
        access.sampled_bindings.clear();
        const Pass *writer = nullptr;
        for (auto earlier = index; earlier-- > 0 && !writer;)
        {
            const auto writes = [&access](const Access &a) { return a.access.HasWrite() && a.Overlaps(access); };
            if (passes_[earlier].live && std::ranges::any_of(passes_[earlier].accesses, writes))
            {
                writer = &passes_[earlier];
            }
        }
        if (!writer)
        {
            access.lowered_reason = "NoWriter";
            continue;
        }
        const auto &ended = physical_passes_[writer->physical];
        ASSERT_F(&ended != &physical, "pass {} reads {} in the physical pass of its writer, but not pixel-locally",
                 pass.name, textures_[access.texture.index].name);
        access.lowered_reason = ended.breaks.front().ToString(false);
    }
}

std::string RGBreak::ToString(bool all_resources) const
{
    const auto count = all_resources ? resources.size() : std::min<size_t>(resources.size(), 1);
    std::string named;
    for (const auto &resource : resources | std::views::take(count))
    {
        named += (named.empty() ? "" : ", ") + resource;
    }
    return std::string(Enum2Str(reason)) + (named.empty() ? "" : "(" + named + ")");
}

// the rules keep every subresource of a physical pass in one layout, with one barrier at most, recorded before the
// rendering, or inside it before a pixel-local read: members share slots and depth, and a shader access neither depends
// on another access of the physical pass nor touches what it attaches, unless it reads an attachment pixel-locally at
// its slot. a pass of another kind breaks only the kind rule.
std::vector<RGBreak> RenderGraph::FindBreaks(const PhysicalPass &physical, const Pass &next) const
{
    std::vector<RGBreak> breaks;
    const auto add = [&breaks](RGBreakReason reason, const std::string &resource) {
        if (breaks.empty() || breaks.back().reason != reason)
        {
            breaks.push_back({.reason = reason, .resources = {}});
        }
        auto &resources = breaks.back().resources;
        if (!resource.empty() && std::ranges::find(resources, resource) == resources.end())
        {
            resources.push_back(resource);
        }
    };

    const auto &first = passes_[physical.members.front()];
    if (first.kind == RGPassKind::External || next.kind == RGPassKind::External)
    {
        add(RGBreakReason::ExternalPass, {});
        return breaks;
    }
    if (first.kind != RGPassKind::Raster || next.kind != RGPassKind::Raster)
    {
        add(RGBreakReason::NonRasterPass, {});
        return breaks;
    }
    const auto size = [this](const Pass &pass) {
        return GetAttachmentSize(*std::ranges::find_if(pass.accesses, &Access::IsAttachment));
    };
    if (size(first) != size(next))
    {
        add(RGBreakReason::TargetSizeMismatch, {});
    }

    // `local`: `a` reads pixel-locally what a member attaches at the slot it reads, so every access of the physical
    // pass to that subresource is such an attachment or read
    using Conflicts = bool (*)(const Access &, const Access &, bool local);
    const std::array<std::pair<RGBreakReason, Conflicts>, 5> rules{{
        {RGBreakReason::DifferentDepth,
         [](const Access &a, const Access &m, bool) {
             return a.slot == DepthSlot && m.slot == DepthSlot && !a.SameSubresources(m);
         }},
        {RGBreakReason::SlotConflict,
         [](const Access &a, const Access &m, bool) {
             const bool color_attachments = a.IsAttachment() && m.IsAttachment() && a.slot != DepthSlot &&
                                            m.slot != DepthSlot && (a.slot == m.slot) != a.SameSubresources(m);
             const bool read_elsewhere =
                 a.pixel_local_slot && m.IsAttachment() && a.SameSubresources(m) && m.slot != *a.pixel_local_slot;
             return color_attachments || read_elsewhere;
         }},
        {RGBreakReason::ClearInPass,
         [](const Access &a, const Access &m, bool) { return a.clear && m.IsAttachment() && a.SameSubresources(m); }},
        {RGBreakReason::NonLocalRead,
         [](const Access &a, const Access &m, bool local) {
             return !local && !a.IsAttachment() && a.Overlaps(m) &&
                    (a.access.HasWrite() || m.access.HasWrite() || !m.access.Contains(a.access));
         }},
        {RGBreakReason::AttachmentReadInPass,
         [](const Access &a, const Access &m, bool) { return a.IsAttachment() && !m.IsAttachment() && a.Overlaps(m); }},
    }};

    const auto members =
        physical.members | std::views::transform([this](uint32_t member) -> const Pass & { return passes_[member]; });
    for (const auto &[reason, conflicts] : rules)
    {
        for (const auto &member : members)
        {
            for (const auto &access : next.accesses)
            {
                const bool local = ReadsAttachment(physical, access);
                if (std::ranges::any_of(member.accesses, [&conflicts, &access, local](const Access &m) {
                        return conflicts(access, m, local);
                    }))
                {
                    add(reason, textures_[access.texture.index].name);
                }
            }

            // ray queries in another shader stage would need a second barrier
            for (const auto &access : next.buffer_accesses)
            {
                if (reason == RGBreakReason::NonLocalRead &&
                    std::ranges::any_of(member.buffer_accesses, [&access](const BufferAccess &m) {
                        return m.buffer == access.buffer && !m.access.Contains(access.access);
                    }))
                {
                    add(reason, buffers_[access.buffer].name);
                }
            }
        }
    }

    if (tile_budget_split_ && tile_budget_ && GetColorBytesPerPixel(physical, &next) > *tile_budget_)
    {
        add(RGBreakReason::TileBudget, {});
    }

    if (!pixel_local_)
    {
        for (const auto &access : next.accesses)
        {
            if (ReadsAttachment(physical, access))
            {
                add(RGBreakReason::NoPixelLocalSupport, textures_[access.texture.index].name);
            }
        }
    }

    if (!merge_)
    {
        add(RGBreakReason::Disabled, {});
    }
    return breaks;
}

uint32_t RenderGraph::GetColorBytesPerPixel(const PhysicalPass &physical, const Pass *next) const
{
    std::vector<const Access *> attachments;
    const auto add = [&attachments](const Pass &pass) {
        for (const auto &access : pass.accesses)
        {
            if (access.IsAttachment() && access.slot != DepthSlot &&
                std::ranges::none_of(attachments, [&access](const Access *a) { return a->SameSubresources(access); }))
            {
                attachments.push_back(&access);
            }
        }
    };
    for (const auto member : physical.members)
    {
        add(passes_[member]);
    }
    if (next)
    {
        add(*next);
    }

    uint32_t bytes = 0;
    for (const auto *attachment : attachments)
    {
        bytes += GetPixelSize(GetFormat(attachment->texture));
    }
    return bytes;
}

// a transient only the attachments of one raster physical pass use is memoryless. transients whose lifetimes, in
// physical passes, do not overlap share an image when format, extent and memorylessness match.
void RenderGraph::ResolveTextures()
{
    for (auto pass_index = 0u; pass_index < passes_.size(); pass_index++)
    {
        for (const auto &access : passes_[pass_index].accesses)
        {
            auto &texture = textures_[access.texture.index];
            if (passes_[pass_index].live)
            {
                texture.lifetime.Extend(pass_index);
                texture.usages = texture.usages | GetImageUsage(access.access.access);
            }
        }
    }

    for (auto &texture : textures_)
    {
        const auto &lifetime = texture.lifetime;
        texture.memoryless = memoryless_ && !texture.imported && lifetime.first &&
                             passes_[*lifetime.first].kind == RGPassKind::Raster &&
                             passes_[*lifetime.first].physical == passes_[lifetime.last].physical &&
                             !(texture.usages & ~MemorylessUsages);
    }

    struct Physical
    {
        const Texture *first;
        RHIImage::ImageUsage usages;
        uint32_t last_pass;
        // a transient it backs is used before and at or after a member that reads pixel-locally
        bool across_pixel_local_barrier;
    };

    const auto across_pixel_local_barrier = [this](const Texture &texture) {
        return std::ranges::any_of(
            std::views::iota(*texture.lifetime.first + 1, texture.lifetime.last + 1),
            [this](uint32_t pass) { return std::ranges::any_of(passes_[pass].accesses, &Access::IsPixelLocal); });
    };

    std::vector<Physical> physicals;

    // in order of first use, so the assignment is the same every frame
    for (auto pass_index = 0u; pass_index < passes_.size(); pass_index++)
    {
        for (const auto &access : passes_[pass_index].accesses)
        {
            auto &texture = textures_[access.texture.index];
            if (texture.imported || texture.lifetime.first != pass_index || texture.physical)
            {
                continue;
            }

            const auto found = std::ranges::find_if(physicals, [this, &texture, pass_index](const Physical &physical) {
                const auto &first = *physical.first;
                return passes_[physical.last_pass].physical < passes_[pass_index].physical &&
                       first.desc.format == texture.desc.format && first.width == texture.width &&
                       first.height == texture.height && first.memoryless == texture.memoryless;
            });
            if (found == physicals.end())
            {
                texture.physical = static_cast<uint32_t>(physicals.size());
                physicals.push_back({.first = &texture,
                                     .usages = texture.usages,
                                     .last_pass = texture.lifetime.last,
                                     .across_pixel_local_barrier = across_pixel_local_barrier(texture)});
            }
            else
            {
                texture.physical = static_cast<uint32_t>(found - physicals.begin());
                found->usages = found->usages | texture.usages;
                found->last_pass = texture.lifetime.last;
                found->across_pixel_local_barrier =
                    found->across_pixel_local_barrier || across_pixel_local_barrier(texture);
            }
        }
    }

    // a memoryless transient gets an ordinary pooled image without memoryless storage for its format and usages, or
    // when it lives across a pixel-local barrier that drops memoryless contents
    std::vector<RHIImage *> images;
    images.reserve(physicals.size());
    for (const auto &physical : physicals)
    {
        const auto &first = *physical.first;
        const bool memoryless =
            first.memoryless && rhi_->SupportsMemorylessImage(first.desc.format, physical.usages) &&
            (!physical.across_pixel_local_barrier || rhi_->KeepsMemorylessAcrossPixelLocalBarrier());
        images.push_back(
            pool_.Acquire({.format = first.desc.format,
                           .width = first.width,
                           .height = first.height,
                           .usages = physical.usages,
                           .memory_properties = memoryless ? RHIMemoryProperty::Memoryless : RHIMemoryProperty::None},
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
            buffer.lifetime.Extend(pass_index);
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
            const auto &texture = textures_[access.texture.index];
            for (const auto &binding : access.bindings)
            {
                pass.bindings.push_back(binding(rhi_, *texture.image, access.subresources));
                pass.bound_resources.emplace_back(texture.name);
            }
        }
        for (const auto &access : pass.buffer_accesses)
        {
            std::ranges::copy(access.bindings, std::back_inserter(pass.bindings));
            pass.bound_resources.resize(pass.bindings.size(), buffers_[access.buffer].name);
        }
        for (const auto &[image, binding] : pass.placeholders)
        {
            pass.bindings.push_back(binding(rhi_, *image, GetAllSubresources(*image)));
            pass.bound_resources.emplace_back(std::nullopt);
        }
    }
}

// buffers and acceleration structures follow the access rule: the memory barrier `access` needs after `state`, which
// becomes the state after it. a build also waits for earlier builds: the BLAS it reads, submitted before the frame, and
// the scratch memory it reuses.
static std::optional<RHIMemoryBarrier> PlanMemoryBarrier(RHIResourceAccess &state, const RHIResourceAccess &access)
{
    const auto next = TransitionAccess(state, access);
    if (!next)
    {
        return std::nullopt;
    }

    const bool build = access.access & RHIAccess::AccelerationStructureBuild;
    const auto from = build ? state | access : state;
    state = *next;
    if (from.access == RHIAccess::None)
    {
        return std::nullopt;
    }
    return RHIMemoryBarrier{.from = from, .to = access};
}

// each access transitions its subresources from the states the previous accesses left, seeded from the tracked states.
// writes to contents nobody may use again discard them. an attachment an earlier member of the physical pass attached
// keeps its state without a barrier: rasterization order orders attachment accesses within a rendering.
void RenderGraph::PlanBarriers()
{
    // per physical image, the planned state of each subresource, layer by layer within a mip. transients sharing an
    // image share its states, so a later one waits for the accesses of the earlier one.
    std::unordered_map<const RHIImage *, std::vector<RHIImageState>> states;
    std::vector<const Pass *> last_writer(textures_.size(), nullptr);
    std::vector<RHIResourceAccess> buffer_states;
    std::ranges::transform(buffers_, std::back_inserter(buffer_states),
                           [](const Buffer &buffer) { return buffer.GetTracked().GetTrackedAccess(); });

    for (auto &pass : passes_ | std::views::filter(&Pass::live))
    {
        const auto &members = physical_passes_[pass.physical].members;
        const std::span earlier(members.begin(),
                                std::ranges::lower_bound(members, static_cast<uint32_t>(&pass - passes_.data())));

        for (auto &access : pass.buffer_accesses)
        {
            auto &state = buffer_states[access.buffer];
            access.barrier = PlanMemoryBarrier(state, access.access);
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
            const bool attachment = access.IsAttachment();
            if (const auto *attached = attachment ? FindAccess(earlier, AttachmentOf(access, access.slot)) : nullptr)
            {
                access.states = attached->states;
            }
            else
            {
                PlanAccess(access, discard, image_states);
            }

            if (access.access.HasWrite())
            {
                last_writer[access.texture.index] = &pass;
            }

            if (pass.kind != RGPassKind::Raster || !attachment)
            {
                continue;
            }

            if (access.clear)
            {
                access.load_op = RHILoadOp::Clear;
                access.load_reason = "clear";
            }
            else if (!uses_contents)
            {
                access.load_op = RHILoadOp::DontCare;
                access.load_reason = "fully overwritten";
            }
            else if (discard)
            {
                access.load_op = RHILoadOp::DontCare;
                access.load_reason = "no earlier writer";
            }
            else
            {
                access.load_reason = writer ? "written by " + writer->name : "imported";
            }
        }
    }

    // a buffer the host reads becomes visible to it right after its last live pass, from the state that pass left
    for (auto index = 0u; index < buffers_.size(); index++)
    {
        const auto &buffer = buffers_[index];
        if (!buffer.read_on_host || !buffer.lifetime.first)
        {
            continue;
        }

        auto &state = buffer_states[index];
        const RHIResourceAccess host_read{.access = RHIAccess::HostRead};
        const auto barrier = PlanMemoryBarrier(state, host_read);
        passes_[buffer.lifetime.last].host_reads.push_back(
            {.buffer = index, .access = host_read, .bindings = {}, .barrier = barrier, .state = state});
    }
}

// the subresources of each access move into its layout and access with one barrier per run of mips in one state,
// spanning every layer when each mip's layers share a state, otherwise within each layer
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
    bool layers_agree = true;
    ForEachSubresource(subresources, [&state_of, &subresources, &layers_agree](unsigned mip, unsigned layer) {
        layers_agree = layers_agree && state_of(mip, layer) == state_of(mip, subresources.base_layer);
    });
    const unsigned layer_step = layers_agree ? subresources.layer_count : 1;

    const unsigned mip_end = subresources.base_mip + subresources.mip_count;
    const unsigned layer_end = subresources.base_layer + subresources.layer_count;
    for (unsigned layer = subresources.base_layer; layer < layer_end; layer += layer_step)
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
                        .layer_count = static_cast<uint8_t>(layer_step)});
            mip = run_end;
        }
    }

    // the store op of the attachment a pixel-local read reads writes the image after the read
    const RHIResourceAccess store{.access = access.IsPixelLocal() ? RHIAccess::ColorWrite : RHIAccess::None};
    ForEachSubresource(subresources, [&access, &state_of, &store](unsigned mip, unsigned layer) {
        auto &state = state_of(mip, layer);
        state.access = state.access | store;
        access.states.push_back(state);
    });
}

// an attachment is stored when the next live pass after its physical pass touching it uses its contents, or when it is
// imported
void RenderGraph::InferStoreOps()
{
    for (auto &pass : passes_)
    {
        if (!pass.live || pass.kind != RGPassKind::Raster)
        {
            continue;
        }

        for (auto &access : pass.accesses | std::views::filter(&Access::IsAttachment))
        {
            access.store_reason = "no later reader";
            if (textures_[access.texture.index].imported)
            {
                access.store_op = RHIStoreOp::Store;
                access.store_reason = "imported";
            }

            const auto end = physical_passes_[pass.physical].members.back() + 1;
            for (const auto &later : passes_ | std::views::drop(end) | std::views::filter(&Pass::live))
            {
                const auto next =
                    std::ranges::find_if(later.accesses, [&access](const Access &a) { return a.Overlaps(access); });
                if (next == later.accesses.end())
                {
                    continue;
                }

                if (UsesContents(next->access, next->clear.has_value(), later.fully_overwrites))
                {
                    access.store_op = RHIStoreOp::Store;
                    access.store_reason = "read by " + later.name;
                }
                else
                {
                    access.store_op = RHIStoreOp::DontCare;
                    access.store_reason = "overwritten by " + later.name;
                }
                break;
            }
        }
    }
}

void RenderGraph::BuildRenderingInfos()
{
    for (auto &physical : physical_passes_)
    {
        if (passes_[physical.members.front()].kind != RGPassKind::Raster)
        {
            continue;
        }

        auto &info = physical.rendering_info;
        for (const auto member : physical.members)
        {
            const auto &pass = passes_[member];
            for (const auto &access : pass.accesses | std::views::filter(&Access::IsAttachment))
            {
                const auto &texture = textures_[access.texture.index];
                const auto mip = access.subresources.base_mip;
                const auto layer = access.subresources.base_layer;
                const auto size = GetAttachmentSize(access);
                RGCheck(info.width == 0 || (info.width == size.x() && info.height == size.y()),
                        "attachments of pass {} differ in size", pass.name);
                info.width = size.x();
                info.height = size.y();
                const bool depth = access.slot == DepthSlot;
                if (depth ? info.depth_attachment.image : info.color_attachments[access.slot].image)
                {
                    continue;
                }
                if (depth)
                {
                    info.depth_attachment = {.image = texture.image,
                                             .mip_level = mip,
                                             .array_layer = layer,
                                             .load_op = access.load_op,
                                             .store_op = access.store_op,
                                             .clear_depth = access.clear ? access.clear->x() : 1.f};
                }
                else
                {
                    info.color_attachments[access.slot] = {.image = texture.image,
                                                           .mip_level = mip,
                                                           .array_layer = layer,
                                                           .layout = access.layout,
                                                           .load_op = access.load_op,
                                                           .store_op = access.store_op,
                                                           .clear_color =
                                                               access.clear.value_or(Vector4(0.f, 0.f, 0.f, 1.f))};
                }
                RGCheck(!texture.memoryless ||
                            (access.load_op != RHILoadOp::Load && access.store_op == RHIStoreOp::DontCare),
                        "memoryless {} is loaded or stored", texture.name);
                physical.attachments.push_back(&access);
            }
        }

        uint8_t color_slots = 0;
        for (auto slot = 0u; slot < MaxNumColorAttachments; slot++)
        {
            if (info.color_attachments[slot].image)
            {
                color_slots |= static_cast<uint8_t>(1u << slot);
            }
        }
        for (const auto member : physical.members)
        {
            auto &pass = passes_[member];
            pass.unwritten_color_slots = color_slots;
            pass.depth_unused = info.depth_attachment.image != nullptr;
            for (const auto &access : pass.accesses | std::views::filter(&Access::IsAttachment))
            {
                if (access.slot == DepthSlot)
                {
                    pass.depth_unused = false;
                }
                else
                {
                    pass.unwritten_color_slots &= static_cast<uint8_t>(~(1u << access.slot));
                }
            }
        }
    }
}

// writes the state each buffer access leaves through to its buffer, returning the accesses' barriers
template <typename BufferAccesses, typename Buffers>
static std::vector<RHIMemoryBarrier> WriteThrough(const BufferAccesses &accesses, const Buffers &buffers)
{
    std::vector<RHIMemoryBarrier> barriers;
    for (const auto &access : accesses)
    {
        if (access.barrier)
        {
            barriers.push_back(*access.barrier);
        }
        buffers[access.buffer].GetTracked().SetTrackedAccess(access.state);
    }
    return barriers;
}

// the graph writes the planned states of a physical pass's members through to the tracked states before recording it,
// so foreign code and the next frame start from them, and records their barriers in one batch before it, except those
// of pixel-local reads, which it records inside the rendering right before their member. with full barriers, each
// physical pass also waits for every earlier command. right after the last pass using a buffer the host reads, a
// barrier makes the buffer visible to the host.
void RenderGraph::Execute(RHICommandContext &command_context, RGPassTimers *timers)
{
    RGCheck(compiled_ && !executed_, "the graph executes once, after compiling");
    executed_ = true;
    const RHIContext::GraphExecutionScope graph_execution(*rhi_);

    // after a throw, no pass stays open and no binding into the graph stays set
    struct RecordingScope
    {
        RHICommandContext &command_context;

        ~RecordingScope()
        {
            if (command_context.IsRendering())
            {
                command_context.EndRendering();
            }
            if (const auto compute_pass = command_context.GetCurrentComputePass())
            {
                command_context.EndComputePass(compute_pass);
            }
            command_context.SetBindings({});
        }
    };

    const RecordingScope recording{.command_context = command_context};

    const auto record = [this, &command_context](Pass &pass) {
        command_context.SetBindings(pass.bindings);
        const auto start = std::chrono::steady_clock::now();
        pass.record(command_context);
        pass.cpu_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
        CheckBindingsApplied(pass, command_context);
        CheckBoundResourcesDeclared(pass, command_context);
        command_context.SetBindings({});
    };

    for (auto &physical : physical_passes_)
    {
        std::vector<RHIImageBarrier> barriers;
        std::vector<RHIMemoryBarrier> memory_barriers;
        for (const auto member : physical.members)
        {
            const auto &pass = passes_[member];
            for (const auto &access : pass.accesses)
            {
                if (!access.IsPixelLocal())
                {
                    barriers.insert(barriers.end(), access.barriers.begin(), access.barriers.end());
                }
                auto *image = textures_[access.texture.index].image;
                auto state = access.states.begin();
                ForEachSubresource(access.subresources, [image, &state](unsigned mip, unsigned layer) {
                    image->SetState(*state++, mip, 1, layer, 1);
                });
            }
            std::ranges::copy(WriteThrough(pass.buffer_accesses, buffers_), std::back_inserter(memory_barriers));
        }
        if (full_barriers_)
        {
            memory_barriers.push_back({.from = {.access = RHIAccess::Any}, .to = {.access = RHIAccess::Any}});
        }

        auto &first = passes_[physical.members.front()];
        RHIPass *timed_pass = nullptr;
        switch (first.kind)
        {
        case RGPassKind::Raster:
            timed_pass = timers ? timers->Get(physical.name) : nullptr;
            command_context.BeginRendering(physical.rendering_info, physical.name, timed_pass, barriers,
                                           memory_barriers);
            for (const auto member : physical.members)
            {
                auto &pass = passes_[member];
                std::optional<RHICommandContext::DebugLabelScope> label;
                if (physical.members.size() > 1)
                {
                    label.emplace(command_context, pass.name);
                }
                std::vector<RHIImageBarrier> local_barriers;
                for (const auto &access : pass.accesses | std::views::filter(&Access::IsPixelLocal))
                {
                    local_barriers.insert(local_barriers.end(), access.barriers.begin(), access.barriers.end());
                }
                command_context.PixelLocalBarrier(local_barriers);
                command_context.SetUnusedAttachments(pass.unwritten_color_slots, pass.depth_unused);
                record(pass);
            }
            command_context.EndRendering();
            break;
        case RGPassKind::Compute:
            timed_pass = first.compute_pass.get();
            command_context.BeginComputePass(first.compute_pass, barriers, memory_barriers);
            record(first);
            command_context.EndComputePass(first.compute_pass);
            break;
        case RGPassKind::Copy:
        case RGPassKind::External: {
            const RHICommandContext::DebugLabelScope label(command_context, first.name);
            command_context.Barrier(barriers, memory_barriers);
            record(first);
            break;
        }
        default:
            UnImplemented(first.kind);
        }

        for (const auto member : physical.members)
        {
            auto &pass = passes_[member];
            CheckDeclaredStates(pass);
            command_context.Barrier({}, WriteThrough(pass.host_reads, buffers_));
        }

        if (timed_pass)
        {
            physical.gpu_ms = timed_pass->GetExecutionTime();
        }
    }
}

// a declared binding that no pipeline the pass drew or dispatched has would bind nothing. a pass that drew nothing (an
// empty scene) bound nothing to check. placeholders stand in for missing inputs and are not checked.
void RenderGraph::CheckBindingsApplied(const Pass &pass, const RHICommandContext &command_context)
{
    if (command_context.GetPipelines().empty())
    {
        return;
    }

    for (size_t index = 0; index < pass.bindings.size(); index++)
    {
        const auto &resource = pass.bound_resources[index];
        RGCheck(!resource || command_context.IsBindingApplied(index),
                "pass {} binds {} to a resource table no pipeline it drew or dispatched has", pass.name,
                resource.value_or(""));
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
        type == RHIShaderResourceReflection::ResourceType::StorageImage2D ||
        type == RHIShaderResourceReflection::ResourceType::InputAttachment)
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

// a pass that transitions a declared image behind the graph's back would desync the plan from the tracked state. the
// image holds the state of the last access of its physical pass.
void RenderGraph::CheckDeclaredStates(const Pass &pass) const
{
    const auto &members = physical_passes_[pass.physical].members;
    const std::span later(std::ranges::upper_bound(members, static_cast<uint32_t>(&pass - passes_.data())),
                          members.end());
    for (const auto &access : pass.accesses)
    {
        if (FindAccess(later, [&access](const Access &a) { return a.Overlaps(access); }) != nullptr)
        {
            continue;
        }

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
