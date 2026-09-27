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

    // draws through AddTo, as the graph pass `name`, into a color attachment of `output_format` at slot 0.
    // `to_back_buffer` applies the window's pre-rotation.
    ScreenQuadPass(RHIContext *ctx, std::string name, PixelFormat output_format, bool to_back_buffer = false);

    // adds a Raster pass drawing `input`, sampled with the sampler its image carries, over all of `output`
    void AddTo(RenderGraph &graph, RGTexture input, RGTexture output) const;

    void InitRenderResources(const RenderConfig &config) override;

    void UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene) override;

protected:
    virtual void SetupPixelShader();

    // binds what the pixel shader reads beyond the graph's bindings
    virtual void BindPixelShaderResources()
    {
    }

    // declares the pass's input, bound to the pixel shader's texture and sampler
    virtual void SampleInput(RGBuilder &builder, RGTexture input) const;

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
    bool to_back_buffer_ = false;
};
} // namespace sparkle
