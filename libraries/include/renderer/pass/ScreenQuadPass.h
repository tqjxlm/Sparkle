#pragma once

#include "renderer/pass/PipelinePass.h"

#include "core/math/Types.h"
#include "rhi/RHIImage.h"
#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIRenderTarget.h"

namespace sparkle
{
class ScreenQuadVertexShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ScreenQuadVertexShader, RHIShaderStage::Vertex, "shaders/screen/screen.vs.hlsl", "main")

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

    ScreenQuadPass(RHIContext *ctx, const RHIResourceRef<RHIImage> &input,
                   const RHIResourceRef<RHIRenderTarget> &target)
        : PipelinePass(ctx), source_texture_(input), target_(target)
    {
        ASSERT(target_);
    }

    ScreenQuadPass(RHIContext *ctx, const RHIResourceRef<RHIImage> &input) : PipelinePass(ctx), source_texture_(input)
    {
    }

    void Render() override;

    void InitRenderResources(const RenderConfig &config) override;

    void UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene) override;

    [[nodiscard]] RHIResourceRef<RHIRenderPass> GetRenderPass() const
    {
        return pass_;
    }

protected:
    virtual void SetupRenderPass();
    virtual void SetupPipeline();
    virtual void SetupVertices();
    virtual void SetupVertexShader();
    virtual void SetupPixelShader();
    virtual void CompilePipeline();
    virtual void BindVertexShaderResources();
    virtual void BindPixelShaderResources();

    const static std::array<ScreenVertex, 4> Vertices;
    const static std::array<uint32_t, 6> Indices;

    RHIResourceRef<RHIImage> source_texture_;
    RHIResourceRef<RHIRenderTarget> target_;

    RHIResourceRef<RHIBuffer> vertex_buffer_;
    RHIResourceRef<RHIBuffer> index_buffer_;

    RHIResourceRef<RHIPipelineState> pipeline_state_;
    RHIResourceRef<RHIRenderPass> pass_;

    RHIResourceRef<RHIBuffer> vs_ub_;
    RHIResourceRef<RHIBuffer> ps_ub_;

    DrawArgs draw_args_;
};
} // namespace sparkle
