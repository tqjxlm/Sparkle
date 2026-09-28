#pragma once

#include "renderer/pass/PipelinePass.h"

#include "core/math/Types.h"
#include "rhi/RHIImage.h"
#include "rhi/RHIPIpelineState.h"

namespace sparkle
{
class RenderGraph;
class RGBuilder;
struct RGTexture;

class ScreenQuadVertexShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ScreenQuadVertexShader, RHIShaderStage::Vertex, "shaders/screen/screen.vs.slang", "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::UniformBuffer)

    END_SHADER_RESOURCE_TABLE

    struct UniformBufferData
    {
        // we only use 2x2 but need to conform to std140 layout
        Mat4x2 pre_rotation;
    };
};

class ScreenQuadPass : public PipelinePass
{
public:
    struct ScreenVertex
    {
        Vector3 position;
        Vector2 uv;
    };

    // how AddTo samples its input, always with edge clamping
    enum class InputFilter : uint8_t
    {
        Nearest,
        // bilinear when the input's size differs from the output's and the device filters the input's format
        // linearly, otherwise nearest
        Bilinear,
        // as Bilinear, but nearest also when the output's size is an integer multiple of the input's in both axes
        NearestAtIntegerScale,
    };

    // draws through AddTo, as the graph pass `name`, into a color attachment of `output_format` at slot 0.
    // `to_back_buffer` applies the window's pre-rotation, and the filter compares the output's size along the rotated
    // axes.
    ScreenQuadPass(RHIContext *ctx, std::string name, PixelFormat output_format, InputFilter input_filter,
                   bool to_back_buffer = false);

    // adds a Raster pass drawing `input`, sampled as the pass's InputFilter chooses, over all of `output`
    void AddTo(RenderGraph &graph, RGTexture input, RGTexture output) const;

    void InitRenderResources(const RenderConfig &config) override;

    void UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene) override;

protected:
    static constexpr RHISampler::SamplerAttribute NearestSampler{
        .address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest};

    virtual void SetupPixelShader();

    // binds what the pixel shader reads beyond the graph's bindings
    virtual void BindPixelShaderResources()
    {
    }

    // declares the pass's input, bound to the pixel shader's texture, and binds `sampler` to its sampler
    virtual void SampleInput(RGBuilder &builder, RGTexture input, const RHISampler::SamplerAttribute &sampler) const;

    const static std::array<ScreenVertex, 4> Vertices;
    const static std::array<uint32_t, 6> Indices;

    RHIResourceRef<RHIBuffer> vertex_buffer_;
    RHIResourceRef<RHIBuffer> index_buffer_;

    RHIResourceRef<RHIPipelineState> pipeline_state_;

    RHIResourceRef<RHIBuffer> vs_ub_;
    RHIResourceRef<RHIBuffer> ps_ub_;

    DrawArgs draw_args_;

    std::string name_;

private:
    void SetupPipeline();
    void SetupVertices();
    void SetupVertexShader();
    void BindVertexShaderResources();

    RHIAttachmentSignature signature_;
    InputFilter input_filter_;
    bool to_back_buffer_ = false;
};
} // namespace sparkle
