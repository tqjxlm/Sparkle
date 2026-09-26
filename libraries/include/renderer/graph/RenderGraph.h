#pragma once

#include "renderer/RenderResolution.h"
#include "rhi/RHICommandContext.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace sparkle
{
class RGTexturePool;
class RenderGraph;
struct RenderConfig;

// a texture of one graph: an index into its resource table
struct RGTexture
{
    uint32_t index = std::numeric_limits<uint32_t>::max();

    [[nodiscard]] bool IsValid() const
    {
        return index != std::numeric_limits<uint32_t>::max();
    }

    bool operator==(const RGTexture &) const = default;
};

// resolved against RenderResolution when the graph compiles
enum class RGSizeClass : uint8_t
{
    Scene,
    Output,
    Absolute,
};

// a single-sampled 2D texture with one mip. its usage is the union of the accesses declared on it.
struct RGTextureDesc
{
    PixelFormat format = PixelFormat::Count;
    RGSizeClass size_class = RGSizeClass::Scene;
    // Absolute only
    uint32_t width = 0;
    uint32_t height = 0;
    // images carry the sampler shaders sample them with
    RHISampler::SamplerAttribute sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
                                            .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                                            .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                                            .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest};
};

enum class RGPassKind : uint8_t
{
    Raster,
    Compute,
    Copy,
    External,
};

// declares the accesses of one pass. a pass declares each texture once. shader stages default to the pass kind's
// (raster: pixel, compute: compute, external: all).
class RGBuilder
{
public:
    // color attachment at fragment output `slot`, cleared when `clear` is given
    void ColorWrite(RGTexture texture, uint8_t slot, std::optional<Vector4> clear = std::nullopt);

    void DepthWrite(RGTexture texture, std::optional<float> clear = std::nullopt);

    void DepthTest(RGTexture texture);

    void Sampled(RGTexture texture, RHIShaderStageMask stages = RHIShaderStageMask::None);

    void StorageRead(RGTexture texture, RHIShaderStageMask stages = RHIShaderStageMask::None);

    void StorageWrite(RGTexture texture, RHIShaderStageMask stages = RHIShaderStageMask::None);

    void StorageReadWrite(RGTexture texture, RHIShaderStageMask stages = RHIShaderStageMask::None);

    void CopySrc(RGTexture texture);

    void CopyDst(RGTexture texture);

    // the pass writes every texel of the textures it writes, so their previous contents are discarded
    void FullyOverwrites();

    // the pass runs even when nothing reads its outputs (readback, present)
    void SideEffect();

private:
    friend class RenderGraph;

    RGBuilder(RenderGraph &graph, uint32_t pass) : graph_(graph), pass_(pass)
    {
    }

    void Declare(RGTexture texture, RHIResourceAccess access, RHIImageLayout layout, uint8_t slot,
                 std::optional<Vector4> clear);

    void DeclareShaderAccess(RGTexture texture, RHIAccess access, RHIShaderStageMask stages, RHIImageLayout layout);

    RenderGraph &graph_;
    uint32_t pass_;
};

// what every pass context offers. each kind's context adds only the commands that kind may record.
class RGPassContext
{
public:
    // the image behind a texture the pass declared
    [[nodiscard]] RHIImage *GetImage(RGTexture texture) const;

protected:
    RGPassContext(const RenderGraph &graph, uint32_t pass, RHICommandContext &command_context)
        : command_context_(command_context), graph_(graph), pass_(pass)
    {
    }

    RHICommandContext &command_context_;

private:
    friend class RenderGraph;

    const RenderGraph &graph_;
    uint32_t pass_;
};

// draws inside the rendering the graph began over the pass's attachments
class RGRasterContext : public RGPassContext
{
public:
    static constexpr RGPassKind Kind = RGPassKind::Raster;

    void DrawMesh(const RHIResourceRef<RHIPipelineState> &pipeline_state, const DrawArgs &draw_args)
    {
        command_context_.DrawMesh(pipeline_state, draw_args);
    }

    [[nodiscard]] const RHIAttachmentSignature &GetAttachmentSignature() const
    {
        return command_context_.GetAttachmentSignature();
    }

private:
    friend class RenderGraph;
    using RGPassContext::RGPassContext;
};

class RGComputeContext : public RGPassContext
{
public:
    static constexpr RGPassKind Kind = RGPassKind::Compute;

    void DispatchCompute(const RHIResourceRef<RHIPipelineState> &pipeline, Vector3UInt total_threads,
                         Vector3UInt thread_per_group)
    {
        command_context_.DispatchCompute(pipeline, total_threads, thread_per_group);
    }

private:
    friend class RenderGraph;
    using RGPassContext::RGPassContext;
};

class RGCopyContext : public RGPassContext
{
public:
    static constexpr RGPassKind Kind = RGPassKind::Copy;

    void CopyToBuffer(RGTexture src, const RHIBuffer *dst)
    {
        command_context_.CopyImageToBuffer(GetImage(src), dst);
    }

private:
    friend class RenderGraph;
    using RGPassContext::RGPassContext;
};

// records foreign code outside any pass. every declared image must be left in the declared layout, with no accesses
// beyond the declared ones pending; the graph aborts otherwise.
class RGExternalContext : public RGPassContext
{
public:
    static constexpr RGPassKind Kind = RGPassKind::External;

    [[nodiscard]] RHICommandContext &GetCommandContext() const
    {
        return command_context_;
    }

private:
    friend class RenderGraph;
    using RGPassContext::RGPassContext;
};

// one frame's passes and textures, rebuilt every frame. passes run in declaration order; each depends on the last
// writer before it. Compile culls unused passes, backs transients with pooled images and plans barriers and load/store
// actions without recording anything; Execute records the live passes. planning reads the images' tracked states, so
// nothing else may record on them between the two. declaration errors abort in every build.
class RenderGraph
{
public:
    RenderGraph(RGTexturePool &pool, const RenderConfig &config);

    ~RenderGraph();

    RenderGraph(const RenderGraph &) = delete;
    RenderGraph &operator=(const RenderGraph &) = delete;

    // a transient texture, backed by a pooled image while the graph lives
    [[nodiscard]] RGTexture CreateTexture(std::string name, const RGTextureDesc &desc);

    // a persistent single-sampled image. passes writing it are never culled; its tracked state is where planning
    // starts, and the graph writes each pass's resulting state back to it
    [[nodiscard]] RGTexture Import(std::string name, const RHIResourceRef<RHIImage> &image);

    // `setup` declares the pass's accesses on the builder and returns the function that records the pass
    template <typename Setup> void AddRasterPass(std::string name, Setup &&setup)
    {
        AddPass<RGRasterContext>(std::move(name), nullptr, std::forward<Setup>(setup));
    }

    // `compute_pass` brackets the recording, labelling and timing it
    template <typename Setup>
    void AddComputePass(std::string name, RHIResourceRef<RHIComputePass> compute_pass, Setup &&setup)
    {
        AddPass<RGComputeContext>(std::move(name), std::move(compute_pass), std::forward<Setup>(setup));
    }

    template <typename Setup> void AddCopyPass(std::string name, Setup &&setup)
    {
        AddPass<RGCopyContext>(std::move(name), nullptr, std::forward<Setup>(setup));
    }

    template <typename Setup> void AddExternalPass(std::string name, Setup &&setup)
    {
        AddPass<RGExternalContext>(std::move(name), nullptr, std::forward<Setup>(setup));
    }

    void Compile();

    void Execute(RHICommandContext &command_context);

    // the compiled graph. it names size classes instead of pixel sizes, so it does not depend on the resolution.
    [[nodiscard]] nlohmann::json Dump() const;

private:
    friend class RGBuilder;
    friend class RGPassContext;

    static constexpr uint8_t NoSlot = std::numeric_limits<uint8_t>::max();
    static constexpr uint8_t DepthSlot = MaxNumColorAttachments;

    struct Access
    {
        RGTexture texture;
        RHIResourceAccess access;
        RHIImageLayout layout;
        // the color slot or DepthSlot of an attachment, NoSlot otherwise
        uint8_t slot;
        std::optional<Vector4> clear;

        // compiled
        std::optional<RHIImageBarrier> barrier = std::nullopt;
        // after the pass's barriers
        RHIImageState state{};
        std::string load_reason;
        std::string store_reason;
    };

    struct Pass
    {
        std::string name;
        RGPassKind kind;
        RHIResourceRef<RHIComputePass> compute_pass;
        std::vector<Access> accesses;
        bool fully_overwrites = false;
        bool side_effect = false;
        std::function<void(RHICommandContext &)> record;

        // compiled
        bool live = true;
        std::string cull_reason;
        RHIRenderingInfo rendering_info{};
    };

    struct Texture
    {
        std::string name;
        RGTextureDesc desc{};
        RHIResourceRef<RHIImage> imported;

        // compiled
        RHIImage *image = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        RHIImage::ImageUsage usages = RHIImage::ImageUsage::Undefined;
        std::optional<uint32_t> first_pass = std::nullopt;
        uint32_t last_pass = 0;
        // the transient's index among the images the pool backs this graph with
        std::optional<uint32_t> physical = std::nullopt;
    };

    template <typename Context, typename Setup>
    void AddPass(std::string name, RHIResourceRef<RHIComputePass> compute_pass, Setup &&setup)
    {
        const auto index = NewPass(std::move(name), Context::Kind, std::move(compute_pass));
        RGBuilder builder(*this, index);
        passes_[index].record = [this, index,
                                 record = std::function<void(Context &)>(std::forward<Setup>(setup)(builder))](
                                    RHICommandContext &command_context) {
            Context context(*this, index, command_context);
            record(context);
        };
    }

    uint32_t NewPass(std::string name, RGPassKind kind, RHIResourceRef<RHIComputePass> compute_pass);

    void Validate() const;

    void Cull();

    void ResolveTextures();

    void PlanBarriers();

    void InferStoreOps();

    void CheckExternalContract(const Pass &pass) const;

    RGTexturePool &pool_;
    RenderResolution resolution_;
    bool cull_;
    std::vector<Pass> passes_;
    std::vector<Texture> textures_;
    bool compiled_ = false;
    bool executed_ = false;
};
} // namespace sparkle
