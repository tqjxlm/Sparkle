#pragma once

#if FRAMEWORK_APPLE

#include "MetalRHIInternal.h"

#include "MetalShader.h"

namespace sparkle
{

class MetalPipelineState : public RHIPipelineState
{
public:
    // analogue to MTLBinding
    struct ArgumentReflection
    {
        unsigned long binding_point;
        MTLBindingType type;
    };

    MetalPipelineState(PipelineType type, const std::string &name) : RHIPipelineState(type, name)
    {
    }

protected:
    void LoadShaders();

#if DESCRIPTOR_SET_AS_ARGUMENT_BUFFER
    void SetupShaderResources(RHIShaderStage stage);
#else
    void SetupShaderResources(MTLAutoreleasedRenderPipelineReflection render_reflection,
                              MTLAutoreleasedComputePipelineReflection compute_reflection, RHIShaderStage stage);
#endif

    void BindResources(id<MTLCommandEncoder> encoder, RHIShaderStage stage);
};

class MetalGraphicsPipeline final : public MetalPipelineState
{
public:
    using MetalPipelineState::MetalPipelineState;

    void CompileInternal() override;

    void Bind(id<MTLRenderCommandEncoder> encoder);

private:
    void CreatePipelineState();

    MTLVertexDescriptor *CreateVertexDescriptor(uint64_t buffer_index_offset);

    void CreateDepthStencilState();

    id<MTLRenderPipelineState> pipeline_state_;
    id<MTLDepthStencilState> depth_stencil_state_;

    // two facts:
    // 1. buffer resources used by vertex shader take binding slot starting from 0
    // 2. vertex buffer and index buffer share binding slots with buffer resources
    // which means vertex buffer and index buffer should use slots starting from the last buffer resource slot
    unsigned num_vertex_shader_buffers_ = 0;
};

class MetalComputePipeline final : public MetalPipelineState
{
public:
    using MetalPipelineState::MetalPipelineState;

    void CompileInternal() override;

    void Bind(id<MTLComputeCommandEncoder> encoder);

    void CreatePipelineState();

private:
    id<MTLComputePipelineState> pipeline_state_;
};

} // namespace sparkle

#endif
