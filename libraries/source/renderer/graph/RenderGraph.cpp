#include "renderer/graph/RenderGraph.h"

#include "RGCheck.h"
#include "renderer/RenderConfig.h"
#include "renderer/graph/RGTexturePool.h"

#include <algorithm>
#include <ranges>
#include <unordered_map>

namespace sparkle
{
static const RHIAccess AttachmentAccess = RHIAccess::ColorWrite | RHIAccess::DepthWrite | RHIAccess::DepthTest;
static const RHIAccess ShaderAccess = RHIAccess::Sampled | RHIAccess::StorageRead | RHIAccess::StorageWrite;
static const RHIAccess CopyAccess = RHIAccess::CopySrc | RHIAccess::CopyDst;
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

// the store op of a depth attachment writes it even when the pass only tests depth (StoreOp::None lowers to
// DONT_CARE), so barriers treat depth tests as depth writes
static RHIResourceAccess GetSyncAccess(const RHIResourceAccess &access)
{
    return access.access & RHIAccess::DepthTest ? RHIResourceAccess{.access = RHIAccess::DepthWrite} : access;
}

static RHIImageState GetUniformState(const RHIImage &image)
{
    const auto state = image.GetState(0, 0);
    for (auto mip = 0u; mip < image.GetAttributes().mip_levels; mip++)
    {
        for (auto layer = 0u; layer < image.GetArrayLayerCount(); layer++)
        {
            RGCheck(image.GetState(mip, layer) == state, "subresources of {} are in different states", image.GetName());
        }
    }
    return state;
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

void RGBuilder::Declare(RGTexture texture, RHIResourceAccess access, RHIImageLayout layout, uint8_t slot,
                        std::optional<Vector4> clear)
{
    auto &pass = graph_.passes_[pass_];
    RGCheck(texture.index < graph_.textures_.size(), "pass {} declares an invalid texture", pass.name);

    const auto &name = graph_.textures_[texture.index].name;
    RGCheck(KindAllows(pass.kind, access.access), "a {} pass cannot declare this access to {}", Enum2Str(pass.kind),
            name);
    for (const auto &declared : pass.accesses)
    {
        RGCheck(declared.texture != texture, "pass {} declares {} twice", pass.name, name);
        RGCheck(slot == RenderGraph::NoSlot || declared.slot != slot, "pass {} binds two attachments to slot {}",
                pass.name, slot);
    }

    pass.accesses.push_back({.texture = texture,
                             .access = access,
                             .layout = layout,
                             .slot = slot,
                             .clear = std::move(clear),
                             .load_reason = {},
                             .store_reason = {}});
}

void RGBuilder::DeclareShaderAccess(RGTexture texture, RHIAccess access, RHIShaderStageMask stages,
                                    RHIImageLayout layout)
{
    const auto kind = graph_.passes_[pass_].kind;
    Declare(texture, {.access = access, .stages = stages == RHIShaderStageMask::None ? GetDefaultStages(kind) : stages},
            layout, RenderGraph::NoSlot, std::nullopt);
}

void RGBuilder::ColorWrite(RGTexture texture, uint8_t slot, std::optional<Vector4> clear)
{
    RGCheck(slot < MaxNumColorAttachments, "color slot {} out of range", slot);
    Declare(texture, {.access = RHIAccess::ColorWrite}, RHIImageLayout::ColorOutput, slot, std::move(clear));
}

void RGBuilder::DepthWrite(RGTexture texture, std::optional<float> clear)
{
    Declare(texture, {.access = RHIAccess::DepthWrite}, RHIImageLayout::DepthStencilOutput, RenderGraph::DepthSlot,
            clear ? std::optional<Vector4>(Vector4(*clear, 0.f, 0.f, 0.f)) : std::nullopt);
}

void RGBuilder::DepthTest(RGTexture texture)
{
    Declare(texture, {.access = RHIAccess::DepthTest}, RHIImageLayout::DepthStencilOutput, RenderGraph::DepthSlot,
            std::nullopt);
}

void RGBuilder::Sampled(RGTexture texture, RHIShaderStageMask stages)
{
    DeclareShaderAccess(texture, RHIAccess::Sampled, stages, RHIImageLayout::Read);
}

void RGBuilder::StorageRead(RGTexture texture, RHIShaderStageMask stages)
{
    DeclareShaderAccess(texture, RHIAccess::StorageRead, stages, RHIImageLayout::StorageWrite);
}

void RGBuilder::StorageWrite(RGTexture texture, RHIShaderStageMask stages)
{
    DeclareShaderAccess(texture, RHIAccess::StorageWrite, stages, RHIImageLayout::StorageWrite);
}

void RGBuilder::StorageReadWrite(RGTexture texture, RHIShaderStageMask stages)
{
    DeclareShaderAccess(texture, RHIAccess::StorageRead | RHIAccess::StorageWrite, stages,
                        RHIImageLayout::StorageWrite);
}

void RGBuilder::CopySrc(RGTexture texture)
{
    Declare(texture, {.access = RHIAccess::CopySrc}, RHIImageLayout::TransferSrc, RenderGraph::NoSlot, std::nullopt);
}

void RGBuilder::CopyDst(RGTexture texture)
{
    Declare(texture, {.access = RHIAccess::CopyDst}, RHIImageLayout::TransferDst, RenderGraph::NoSlot, std::nullopt);
}

void RGBuilder::FullyOverwrites()
{
    graph_.passes_[pass_].fully_overwrites = true;
}

void RGBuilder::SideEffect()
{
    graph_.passes_[pass_].side_effect = true;
}

RHIImage *RGPassContext::GetImage(RGTexture texture) const
{
    const auto &pass = graph_.passes_[pass_];
    RGCheck(std::ranges::any_of(pass.accesses, [texture](const auto &access) { return access.texture == texture; }),
            "pass {} uses a texture it did not declare", pass.name);
    return graph_.textures_[texture.index].image;
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
    RGCheck(std::ranges::none_of(textures_, [&image](const Texture &texture) { return texture.imported == image; }),
            "{} is imported twice", image->GetName());

    textures_.push_back({.name = std::move(name),
                         .imported = image,
                         .image = image.get(),
                         .width = image->GetWidth(),
                         .height = image->GetHeight()});
    return {.index = static_cast<uint32_t>(textures_.size() - 1)};
}

uint32_t RenderGraph::NewPass(std::string name, RGPassKind kind, RHIResourceRef<RHIComputePass> compute_pass)
{
    passes_.push_back({.name = std::move(name),
                       .kind = kind,
                       .compute_pass = std::move(compute_pass),
                       .accesses = {},
                       .record = {},
                       .cull_reason = {}});
    return static_cast<uint32_t>(passes_.size() - 1);
}

void RenderGraph::Compile()
{
    RGCheck(!compiled_, "the graph compiles once");

    Validate();
    Cull();

    pool_.BeginGraph();
    compiled_ = true;

    ResolveTextures();
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
        pass.live = !cull_ || pass.side_effect || std::ranges::any_of(pass.accesses, keeps_alive);
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

// each access transitions its image from the state the previous access left, seeded from the tracked state. writes to
// contents nobody may use again discard them.
void RenderGraph::PlanBarriers()
{
    std::unordered_map<const RHIImage *, RHIImageState> states;
    std::vector<const Pass *> last_writer(textures_.size(), nullptr);

    for (auto &pass : passes_ | std::views::filter(&Pass::live))
    {
        for (auto &access : pass.accesses)
        {
            const auto &texture = textures_[access.texture.index];
            const auto *image = texture.image;
            auto [found, inserted] = states.try_emplace(image);
            if (inserted)
            {
                found->second = GetUniformState(*image);
            }
            auto &state = found->second;

            const auto *writer = last_writer[access.texture.index];
            const bool fully_overwritten = pass.fully_overwrites && access.access.HasWrite();
            const bool discard = access.clear || fully_overwritten ||
                                 (access.access.HasWrite() && writer == nullptr && !texture.imported);

            const auto sync_access = GetSyncAccess(access.access);
            const bool attachment = access.slot != NoSlot;
            if (const auto next = TransitionImageState(state, {.layout = access.layout, .access = sync_access}))
            {
                // an attachment's own access in the source chains a swap chain image's first write to the acquire
                access.barrier = RHIImageBarrier{.image = image,
                                                 .base_mip = 0,
                                                 .mip_count = image->GetAttributes().mip_levels,
                                                 .base_array_layer = 0,
                                                 .array_layer_count = image->GetArrayLayerCount(),
                                                 .from = attachment ? state.access | sync_access : state.access,
                                                 .to = sync_access,
                                                 .from_layout = discard ? RHIImageLayout::Undefined : state.layout,
                                                 .to_layout = access.layout};
                state = *next;
            }
            access.state = state;

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
            else if (fully_overwritten)
            {
                load_op = RHILoadOp::None;
                access.load_reason = "fully overwritten";
            }
            else if (discard)
            {
                load_op = RHILoadOp::None;
                access.load_reason = "no earlier writer";
            }
            else
            {
                access.load_reason = writer ? "written by " + writer->name : "imported";
            }

            auto &info = pass.rendering_info;
            RGCheck(info.width == 0 || (info.width == texture.width && info.height == texture.height),
                    "attachments of pass {} differ in size", pass.name);
            info.width = texture.width;
            info.height = texture.height;
            if (access.slot == DepthSlot)
            {
                info.depth_attachment = {
                    .image = texture.image, .load_op = load_op, .clear_depth = access.clear ? access.clear->x() : 1.f};
            }
            else
            {
                info.color_attachments[access.slot] = {.image = texture.image,
                                                       .load_op = load_op,
                                                       .clear_color =
                                                           access.clear.value_or(Vector4(0.f, 0.f, 0.f, 1.f))};
            }
        }
    }
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
            auto store_op = RHIStoreOp::None;
            access.store_reason = "no later reader";
            if (textures_[access.texture.index].imported)
            {
                store_op = RHIStoreOp::Store;
                access.store_reason = "imported";
            }

            for (const auto &later : passes_ | std::views::drop(pass_index + 1) | std::views::filter(&Pass::live))
            {
                const auto next =
                    std::ranges::find(later.accesses, access.texture, [](const Access &a) { return a.texture; });
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
                    store_op = RHIStoreOp::None;
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
void RenderGraph::Execute(RHICommandContext &command_context)
{
    RGCheck(compiled_ && !executed_, "the graph executes once, after compiling");
    executed_ = true;

    for (auto &pass : passes_ | std::views::filter(&Pass::live))
    {
        std::vector<RHIImageBarrier> barriers;
        for (const auto &access : pass.accesses)
        {
            if (access.barrier)
            {
                barriers.push_back(*access.barrier);
            }
            auto *image = textures_[access.texture.index].image;
            image->SetState(access.state, 0, image->GetAttributes().mip_levels, 0, image->GetArrayLayerCount());
        }

        switch (pass.kind)
        {
        case RGPassKind::Raster:
            command_context.BeginRendering(pass.rendering_info, pass.name, nullptr, barriers);
            pass.record(command_context);
            command_context.EndRendering();
            break;
        case RGPassKind::Compute:
            command_context.BeginComputePass(pass.compute_pass);
            command_context.Barrier(barriers, {});
            pass.record(command_context);
            command_context.EndComputePass(pass.compute_pass);
            break;
        case RGPassKind::Copy:
            command_context.Barrier(barriers, {});
            pass.record(command_context);
            break;
        case RGPassKind::External:
            command_context.Barrier(barriers, {});
            pass.record(command_context);
            CheckExternalContract(pass);
            break;
        default:
            UnImplemented(pass.kind);
        }
    }
}

void RenderGraph::CheckExternalContract(const Pass &pass) const
{
    for (const auto &access : pass.accesses)
    {
        const auto &texture = textures_[access.texture.index];
        const auto state = GetUniformState(*texture.image);
        RGCheck(state.layout == access.state.layout && access.state.access.Contains(state.access),
                "external pass {} left {} in layout {} with accesses beyond its declaration", pass.name, texture.name,
                Enum2Str(state.layout));
    }
}
} // namespace sparkle
