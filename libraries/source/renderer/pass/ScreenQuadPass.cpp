#include "renderer/pass/ScreenQuadPass.h"

#include "application/NativeView.h"
#include "core/math/Utilities.h"
#include "rhi/RHI.h"

namespace sparkle
{
class ScreenQuadPixelShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ScreenQuadPixelShader, RHIShaderStage::Pixel, "shaders/screen/screen.ps.hlsl", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(screenTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(screenTextureSampler, RHIShaderResourceReflection::ResourceType::Sampler)

    END_SHADER_RESOURCE_TABLE
};

const std::array<ScreenQuadPass::ScreenVertex, 4> ScreenQuadPass::Vertices{{
    {.position = Vector3{-1, -1, 0}, .uv = Vector2{0, 0}},
    {.position = Vector3{1, -1, 0}, .uv = Vector2{1, 0}},
    {.position = Vector3{1, 1, 0}, .uv = Vector2{1, 1}},
    {.position = Vector3{-1, 1, 0}, .uv = Vector2{0, 1}},
}};

const std::array<uint32_t, 6> ScreenQuadPass::Indices{0, 2, 1, 0, 3, 2};

void ScreenQuadPass::InitRenderResources(const RenderConfig &)
{
    SetupRenderPass();
    SetupPipeline();
    SetupVertices();
    SetupVertexShader();
    SetupPixelShader();

    CompilePipeline();

    BindVertexShaderResources();
    BindPixelShaderResources();

    draw_args_.index_count = 6;
}

void ScreenQuadPass::CompilePipeline()
{
    pipeline_state_->Compile();
}

void ScreenQuadPass::SetupRenderPass()
{
    RHIRenderPass::Attribute pass_attribute;
    pass_ = rhi_->CreateRenderPass(pass_attribute, target_, "ScreenPass");
}

void ScreenQuadPass::SetupPipeline()
{
    pipeline_state_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Graphics, "ScreenQuadPipeline");
    pipeline_state_->SetRenderPass(pass_);

    RHIPipelineState::DepthState depth_state;
    depth_state.write_depth = false;
    depth_state.test_state = RHIPipelineState::DepthTestState::Always;
    pipeline_state_->SetDepthState(depth_state);
}

void ScreenQuadPass::SetupVertices()
{
    if (!vertex_buffer_)
    {
        vertex_buffer_ =
            rhi_->CreateBuffer({.size = ARRAY_SIZE(Vertices),
                                .usages = RHIBuffer::BufferUsage::VertexBuffer,
                                .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                .is_dynamic = false},
                               "ScreenVertexBuffer");
        vertex_buffer_->UploadImmediate(Vertices.data());
    }

    if (!index_buffer_)
    {
        index_buffer_ =
            rhi_->CreateBuffer({.size = ARRAY_SIZE(Indices),
                                .usages = RHIBuffer::BufferUsage::IndexBuffer,
                                .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                .is_dynamic = false},
                               "ScreenIndexBuffer");
        index_buffer_->UploadImmediate(Indices.data());
    }

    pipeline_state_->SetIndexBuffer(index_buffer_);
    pipeline_state_->SetVertexBuffer(0, vertex_buffer_);

    auto &vertex_delcaration = pipeline_state_->GetVertexInputDeclaration();
    vertex_delcaration.SetAttribute(0, 0, {RHIVertexFormat::R32G32B32Float, offsetof(ScreenVertex, position)});
    vertex_delcaration.SetAttribute(1, 0, {RHIVertexFormat::R32G32Float, offsetof(ScreenVertex, uv)});
}

void ScreenQuadPass::SetupVertexShader()
{
    vertex_shader_ = rhi_->CreateShader<ScreenQuadVertexShader>();
    pipeline_state_->SetShader<RHIShaderStage::Vertex>(vertex_shader_);
}

void ScreenQuadPass::SetupPixelShader()
{
    pixel_shader_ = rhi_->CreateShader<ScreenQuadPixelShader>();
    pipeline_state_->SetShader<RHIShaderStage::Pixel>(pixel_shader_);
}

void ScreenQuadPass::BindVertexShaderResources()
{
    auto *vs_resources = pipeline_state_->GetShaderResource<ScreenQuadVertexShader>();

    if (!vs_ub_)
    {
        vs_ub_ = rhi_->CreateBuffer({.size = sizeof(ScreenQuadVertexShader::UniformBufferData),
                                     .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                     .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                     .is_dynamic = false},
                                    "ScreenVSUBO");

        ScreenQuadVertexShader::UniformBufferData ubo;
        ubo.pre_rotation.setIdentity();

        if (target_->IsBackBufferTarget())
        {
            ubo.pre_rotation.topLeftCorner(2, 2) =
                NativeView::GetRotationMatrix(rhi_->GetHardwareInterface()->GetWindowOrientation());
        }

        vs_ub_->UploadImmediate(&ubo);
    }
    vs_resources->ubo().BindResource(vs_ub_);
}

void ScreenQuadPass::BindPixelShaderResources()
{
    auto *ps_resources = pipeline_state_->GetShaderResource<ScreenQuadPixelShader>();
    ps_resources->screenTexture().BindResource(source_texture_->GetDefaultView(rhi_));
    ps_resources->screenTextureSampler().BindResource(source_texture_->GetSampler());
}

void ScreenQuadPass::Render()
{
    rhi_->BeginRenderPass(pass_);

    rhi_->DrawMesh(pipeline_state_, draw_args_);

    rhi_->EndRenderPass();
}

void ScreenQuadPass::UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene)
{
    PipelinePass::UpdateFrameData(config, scene);

    // in case of swapchain being recreated, we may have to reset the back buffer
    if (target_->IsBackBufferTarget() && rhi_->IsBackBufferDirty())
    {
        ScreenQuadVertexShader::UniformBufferData ubo;
        ubo.pre_rotation.topLeftCorner(2, 2) =
            NativeView::GetRotationMatrix(rhi_->GetHardwareInterface()->GetWindowOrientation());

        vs_ub_->UploadImmediate(&ubo);
    }
}
} // namespace sparkle
