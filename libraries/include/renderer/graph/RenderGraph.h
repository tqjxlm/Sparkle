#pragma once

#include "renderer/RenderResolution.h"
#include "rhi/RHICommandContext.h"
#include "rhi/RHIRayTracing.h"

#include <nlohmann/json_fwd.hpp>

#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace sparkle
{
class RGPassTimers;
class RGTexturePool;
class RHIContext;
class RenderGraph;
struct RenderConfig;
struct RGTextureRange;

// a texture of one graph: an index into its resource table
struct RGTexture
{
    uint32_t index = std::numeric_limits<uint32_t>::max();

    [[nodiscard]] bool IsValid() const
    {
        return index != std::numeric_limits<uint32_t>::max();
    }

    // mip `mip` of every layer
    [[nodiscard]] RGTextureRange Mip(uint8_t mip) const;

    // mip `mip` of layer `layer`
    [[nodiscard]] RGTextureRange Subresource(uint8_t mip, uint8_t layer) const;

    bool operator==(const RGTexture &) const = default;
};

// mips and array layers of a texture. a count of All reaches the last mip or layer.
struct RGSubresources
{
    static constexpr uint8_t All = std::numeric_limits<uint8_t>::max();

    uint8_t base_mip = 0;
    uint8_t mip_count = All;
    uint8_t base_layer = 0;
    uint8_t layer_count = All;

    bool operator==(const RGSubresources &) const = default;
};

// subresources of a texture: every one, unless narrowed with RGTexture::Mip or Subresource
struct RGTextureRange
{
    // a texture stands for all of its subresources
    RGTextureRange(RGTexture whole) : texture(whole) // NOLINT(google-explicit-constructor)
    {
    }

    RGTextureRange(RGTexture of, RGSubresources range) : texture(of), subresources(range)
    {
    }

    RGTexture texture;
    RGSubresources subresources;
};

inline RGTextureRange RGTexture::Mip(uint8_t mip) const
{
    return {*this, {.base_mip = mip, .mip_count = 1}};
}

inline RGTextureRange RGTexture::Subresource(uint8_t mip, uint8_t layer) const
{
    return {*this, {.base_mip = mip, .mip_count = 1, .base_layer = layer, .layer_count = 1}};
}

// a buffer of one graph: an index into its table of buffers and acceleration structures
struct RGBuffer
{
    uint32_t index = std::numeric_limits<uint32_t>::max();

    bool operator==(const RGBuffer &) const = default;
};

// an acceleration structure of one graph, synchronized like a buffer
struct RGAccelerationStructure
{
    uint32_t index = std::numeric_limits<uint32_t>::max();

    bool operator==(const RGAccelerationStructure &) const = default;
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

// a binding member of a shader's ResourceTable that a texture binds to
template <class Table, RHIShaderResourceReflection::ResourceType Type>
using RGBinding = RHIShaderResourceBindingTyped<Type, false> &(Table::*)();

template <class Table> using RGSampledBinding = RGBinding<Table, RHIShaderResourceReflection::ResourceType::Texture2D>;

template <class Table>
using RGStorageBinding = RGBinding<Table, RHIShaderResourceReflection::ResourceType::StorageImage2D>;

template <class Table> using RGSamplerBinding = RGBinding<Table, RHIShaderResourceReflection::ResourceType::Sampler>;

template <class Table>
using RGAccelerationStructureBinding =
    RGBinding<Table, RHIShaderResourceReflection::ResourceType::AccelerationStructure>;

enum class RGPassKind : uint8_t
{
    Raster,
    Compute,
    Copy,
    External,
};

// declares the accesses of one pass. a pass declares each subresource of a texture, and each buffer and acceleration
// structure, once; an attachment is one subresource. shader stages default to the pass kind's (raster: pixel, compute:
// compute, external: all). a shader access given a `binding` member of a shader's ResourceTable binds the resource
// there in every pipeline the pass draws or dispatches whose shader uses that table: a sampled binding views the whole
// image, a storage binding the access's single mip (a cube's layers as a 2D array). a sampled access given a
// `sampler_binding` member too binds the sampler the image carries there.
class RGBuilder
{
public:
    // color attachment at fragment output `slot`, cleared when `clear` is given
    void ColorWrite(RGTextureRange texture, uint8_t slot, std::optional<Vector4> clear = std::nullopt);

    void DepthWrite(RGTextureRange texture, std::optional<float> clear = std::nullopt);

    void DepthTest(RGTextureRange texture);

    void Sampled(RGTextureRange texture, RHIShaderStageMask stages = RHIShaderStageMask::None);

    void StorageWrite(RGTextureRange texture, RHIShaderStageMask stages = RHIShaderStageMask::None);

    void StorageReadWrite(RGTextureRange texture, RHIShaderStageMask stages = RHIShaderStageMask::None);

    template <class Table>
    void Sampled(RGTextureRange texture, RGSampledBinding<Table> binding,
                 RHIShaderStageMask stages = RHIShaderStageMask::None)
    {
        Sampled(texture, stages);
        Bind(binding);
    }

    template <class Table>
    void Sampled(RGTextureRange texture, RGSampledBinding<Table> binding, RGSamplerBinding<Table> sampler_binding,
                 RHIShaderStageMask stages = RHIShaderStageMask::None)
    {
        Sampled(texture, binding, stages);
        BindLastAccess(SamplerBinding(sampler_binding));
    }

    // Sampled when `texture` is valid. otherwise the input is missing, and `placeholder`, an image outside the graph,
    // binds there with its sampler, so no pipeline keeps an image an earlier graph bound.
    template <class Table>
    void SampledOrPlaceholder(RGTexture texture, const RHIResourceRef<RHIImage> &placeholder,
                              RGSampledBinding<Table> binding, RGSamplerBinding<Table> sampler_binding)
    {
        if (texture.IsValid())
        {
            Sampled(texture, binding, sampler_binding);
            return;
        }
        BindPlaceholder(placeholder, ViewBinding(binding));
        BindPlaceholder(placeholder, SamplerBinding(sampler_binding));
    }

    template <class Table>
    void StorageWrite(RGTextureRange texture, RGStorageBinding<Table> binding,
                      RHIShaderStageMask stages = RHIShaderStageMask::None)
    {
        StorageWrite(texture, stages);
        Bind(binding);
    }

    template <class Table>
    void StorageReadWrite(RGTextureRange texture, RGStorageBinding<Table> binding,
                          RHIShaderStageMask stages = RHIShaderStageMask::None)
    {
        StorageReadWrite(texture, stages);
        Bind(binding);
    }

    void CopySrc(RGTextureRange texture);

    void CopyDst(RGTextureRange texture);

    void CopySrc(RGBuffer buffer);

    void CopyDst(RGBuffer buffer);

    // builds or refits the acceleration structure
    void AccelerationStructureBuild(RGAccelerationStructure acceleration_structure);

    // ray queries
    void AccelerationStructureRead(RGAccelerationStructure acceleration_structure,
                                   RHIShaderStageMask stages = RHIShaderStageMask::None);

    template <class Table>
    void AccelerationStructureRead(RGAccelerationStructure acceleration_structure,
                                   RGAccelerationStructureBinding<Table> binding,
                                   RHIShaderStageMask stages = RHIShaderStageMask::None)
    {
        AccelerationStructureRead(acceleration_structure, stages);
        BindLastBufferAccess(RHIMemberBinding(binding, GetAccelerationStructure(acceleration_structure)));
    }

    // the pass writes every texel of the textures it writes, so the previous contents of those it does not also read
    // are discarded
    void FullyOverwrites();

    // the pass runs even when nothing reads its outputs
    void SideEffect();

    // the raster pass records foreign commands (ImGui) through the raw command context, inside the rendering the graph
    // begins over its attachments. the foreign code resets the backend state it records around.
    void NativeAccess();

private:
    friend class RenderGraph;

    RGBuilder(RenderGraph &graph, uint32_t pass) : graph_(graph), pass_(pass)
    {
    }

    void Declare(RGTextureRange texture, RHIResourceAccess access, RHIImageLayout layout, uint8_t slot,
                 std::optional<Vector4> clear);

    void DeclareShaderAccess(RGTextureRange texture, RHIAccess access, RHIShaderStageMask stages,
                             RHIImageLayout layout);

    void DeclareBuffer(uint32_t buffer, RHIResourceAccess access);

    [[nodiscard]] RHIResourceRef<RHITLAS> GetAccelerationStructure(
        RGAccelerationStructure acceleration_structure) const;

    // adds a binding of the buffer access declared last
    void BindLastBufferAccess(RHIMemberBinding binding);

    // makes the binding of subresources of an image
    using ImageBinding = std::function<RHIMemberBinding(RHIContext *, RHIImage &, const RGSubresources &)>;

    // the view a sampled or storage binding of `subresources` binds
    [[nodiscard]] static RHIResourceRef<RHIImageView> GetView(RHIContext *rhi, RHIImage &image,
                                                              const RGSubresources &subresources, bool storage);

    template <class Table, RHIShaderResourceReflection::ResourceType Type>
    static ImageBinding ViewBinding(RGBinding<Table, Type> binding)
    {
        return [binding](RHIContext *rhi, RHIImage &image, const RGSubresources &subresources) {
            return RHIMemberBinding(
                binding,
                GetView(rhi, image, subresources, Type == RHIShaderResourceReflection::ResourceType::StorageImage2D));
        };
    }

    template <class Table> static ImageBinding SamplerBinding(RGSamplerBinding<Table> binding)
    {
        return [binding](RHIContext *, RHIImage &image, const RGSubresources &) {
            return RHIMemberBinding(binding, image.GetSampler());
        };
    }

    template <class Table, RHIShaderResourceReflection::ResourceType Type> void Bind(RGBinding<Table, Type> binding)
    {
        BindLastAccess(ViewBinding(binding));
    }

    // adds a binding of the image of the access declared last
    void BindLastAccess(ImageBinding binding);

    // adds a binding of `placeholder` to the pass
    void BindPlaceholder(const RHIResourceRef<RHIImage> &placeholder, ImageBinding binding);

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

    // the buffer behind a buffer the pass declared
    [[nodiscard]] RHIBuffer *GetBuffer(RGBuffer buffer) const;

    [[nodiscard]] RHITLAS *GetAccelerationStructure(RGAccelerationStructure acceleration_structure) const;

    // the raw command context of a pass that declared NativeAccess
    [[nodiscard]] RHICommandContext &GetNativeContext() const;

    RHICommandContext &command_context_;

private:
    // aborts unless the pass declared the buffer or acceleration structure
    void CheckDeclared(uint32_t buffer) const;

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

    using RGPassContext::GetNativeContext;

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

    void CopyToBuffer(RGTexture src, RGBuffer dst)
    {
        command_context_.CopyImageToBuffer(GetImage(src), GetBuffer(dst));
    }

    void CopyFromBuffer(RGBuffer src, RGTexture dst)
    {
        command_context_.CopyBufferToImage(GetBuffer(src), GetImage(dst));
    }

    // records the build or refit staged on the acceleration structure
    void BuildAccelerationStructure(RGAccelerationStructure acceleration_structure)
    {
        GetAccelerationStructure(acceleration_structure)->RecordBuild(command_context_);
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

// declares a pass's accesses on the builder and returns the function that records the pass through a `Context`
template <typename Setup, typename Context>
concept RGPassSetup =
    std::invocable<Setup, RGBuilder &> && std::invocable<std::invoke_result_t<Setup, RGBuilder &>, Context &>;

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
    // starts, and the graph writes each pass's resulting state back to it. importing an image again returns the first
    // import's texture and name.
    [[nodiscard]] RGTexture Import(std::string name, const RHIResourceRef<RHIImage> &image);

    // a persistent buffer, imported like an image: planning starts from its tracked access
    [[nodiscard]] RGBuffer Import(std::string name, const RHIResourceRef<RHIBuffer> &buffer);

    // a top-level acceleration structure, imported like a buffer
    [[nodiscard]] RGAccelerationStructure Import(std::string name,
                                                 const RHIResourceRef<RHITLAS> &acceleration_structure);

    // the first texture created or imported as `name`, invalid when there is none
    [[nodiscard]] RGTexture FindTexture(std::string_view name) const;

    // whether a pass added now may sample the texture through a 2D binding: a single-layer import with texture usage,
    // or a transient an earlier pass writes
    [[nodiscard]] bool CanSample2D(RGTexture texture) const;

    [[nodiscard]] PixelFormat GetFormat(RGTexture texture) const;

    // the size in pixels, known once the texture is created or imported
    [[nodiscard]] Vector2UInt GetSize(RGTexture texture) const;

    // `setup` declares the pass's accesses on the builder and returns the function that records the pass
    template <RGPassSetup<RGRasterContext> Setup> void AddRasterPass(std::string name, Setup &&setup)
    {
        AddPass<RGRasterContext>(std::move(name), nullptr, std::forward<Setup>(setup));
    }

    // `compute_pass` brackets the recording, labelling and timing it
    template <RGPassSetup<RGComputeContext> Setup>
    void AddComputePass(std::string name, RHIResourceRef<RHIComputePass> compute_pass, Setup &&setup)
    {
        AddPass<RGComputeContext>(std::move(name), std::move(compute_pass), std::forward<Setup>(setup));
    }

    template <RGPassSetup<RGCopyContext> Setup> void AddCopyPass(std::string name, Setup &&setup)
    {
        AddPass<RGCopyContext>(std::move(name), nullptr, std::forward<Setup>(setup));
    }

    template <RGPassSetup<RGExternalContext> Setup> void AddExternalPass(std::string name, Setup &&setup)
    {
        AddPass<RGExternalContext>(std::move(name), nullptr, std::forward<Setup>(setup));
    }

    void Compile();

    // raster passes are timed by `timers` when given, compute passes by their RHIComputePass
    void Execute(RHICommandContext &command_context, RGPassTimers *timers = nullptr);

    // the compiled graph, with the GPU time of each executed pass whose timer has a result. it names size classes
    // instead of pixel sizes, so it does not depend on the resolution.
    [[nodiscard]] nlohmann::json Dump() const;

private:
    friend class RGBuilder;
    friend class RGPassContext;

    static constexpr uint8_t NoSlot = std::numeric_limits<uint8_t>::max();
    static constexpr uint8_t DepthSlot = MaxNumColorAttachments;

    struct Access;
    struct BufferAccess;
    struct Pass;
    struct Texture;
    struct Buffer;

    template <typename Context, typename Setup>
    void AddPass(std::string name, RHIResourceRef<RHIComputePass> compute_pass, Setup &&setup)
    {
        const auto index = NewPass(std::move(name), Context::Kind, std::move(compute_pass));
        RGBuilder builder(*this, index);
        SetRecord(index, [this, index, record = std::invoke(std::forward<Setup>(setup), builder)](
                             RHICommandContext &command_context) mutable {
            Context context(*this, index, command_context);
            record(context);
        });
    }

    uint32_t NewPass(std::string name, RGPassKind kind, RHIResourceRef<RHIComputePass> compute_pass);

    void SetRecord(uint32_t pass, std::function<void(RHICommandContext &)> record);

    // the index of `buffer`, or of its first import
    uint32_t ImportBuffer(Buffer buffer);

    void Validate() const;

    void Cull();

    void ResolveTextures();

    void ResolveBuffers();

    void ResolveBindings();

    void PlanBarriers();

    // transitions the access's subresources in `states`, the planned states of its image, recording its barriers and
    // resulting states
    void PlanAccess(Access &access, bool discard, std::vector<RHIImageState> &states) const;

    void InferStoreOps();

    // the attachments of each live raster pass, with the load and store actions planned for them
    void BuildRenderingInfos();

    void CheckDeclaredStates(const Pass &pass) const;

    static void CheckBindingsApplied(const Pass &pass, const RHICommandContext &command_context);

    void CheckBoundResourcesDeclared(const Pass &pass, const RHICommandContext &command_context) const;

    void CheckBindingDeclared(const Pass &pass, const RHIShaderResourceBinding &binding) const;

    RGTexturePool &pool_;
    RenderResolution resolution_;
    bool cull_;
    bool full_barriers_;
    std::vector<Pass> passes_;
    std::vector<Texture> textures_;
    std::vector<Buffer> buffers_;
    bool compiled_ = false;
    bool executed_ = false;
};
} // namespace sparkle
