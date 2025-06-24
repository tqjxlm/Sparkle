#include "renderer/pass/SkyBoxPass.h"

#include "application/NativeView.h"
#include "io/Mesh.h"
#include "renderer/pass/ScreenQuadPass.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "rhi/RHI.h"

namespace sparkle
{
class SkyBoxVertexShader : public RHIShaderInfo
{
    REGISTGER_SHADER(SkyBoxVertexShader, RHIShaderStage::Vertex, "shaders/standard/sky_box.vs.hlsl", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(view, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)

    END_SHADER_RESOURCE_TABLE

    struct UniformBufferData
    {
        Mat4 view_matrix;
        Mat4 projection_matrix;
    };
};

class SkyLightPixelShader : public RHIShaderInfo
{
    REGISTGER_SHADER(SkyLightPixelShader, RHIShaderStage::Pixel, "shaders/screen/sky_light.ps.hlsl", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::UniformBuffer)

    END_SHADER_RESOURCE_TABLE
};

class SkyBoxPixelShader : public RHIShaderInfo
{
    REGISTGER_SHADER(SkyBoxPixelShader, RHIShaderStage::Pixel, "shaders/standard/sky_box.ps.hlsl", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(sky_map, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(sky_map_sampler, RHIShaderResourceReflection::ResourceType::Sampler)

    END_SHADER_RESOURCE_TABLE
};

SkyBoxPass::SkyBoxPass(RHIContext *rhi, const SkyRenderProxy *sky_proxy, const RHIResourceRef<RHIImage> &color_buffer,
                       const RHIResourceRef<RHIImage> &depth_buffer)
    : PipelinePass(rhi), sky_proxy_(sky_proxy), color_buffer_(color_buffer), depth_buffer_(depth_buffer)
{
    sky_map_to_render_ = sky_proxy->GetSkyMap();
}

void SkyBoxPass::OverrideSkyMap(const RHIResourceRef<RHIImage> &sky_map)
{
    if (sky_map)
    {
        sky_map_to_render_ = sky_map;
    }
    else
    {
        sky_map_to_render_ = sky_proxy_->GetSkyMap();
    }
    BindShaderResources();
}

void SkyBoxPass::Render()
{
    rhi_->BeginRenderPass(render_pass_);

    rhi_->DrawMesh(pipeline_state_, draw_args_);

    rhi_->EndRenderPass();
}

void SkyBoxPass::UpdateFrameData(const RenderConfig & /*config*/, SceneRenderProxy *scene)
{
    if (sky_map_to_render_)
    {
        const auto *camera = scene->GetCamera();
        Mat4 view_matrix_without_translation = Mat4::Zero();
        view_matrix_without_translation.topLeftCorner<3, 3>() = camera->GetViewMatrix().topLeftCorner<3, 3>();
        view_matrix_without_translation(3, 3) = 1;
        SkyBoxVertexShader::UniformBufferData view_ubo{.view_matrix = view_matrix_without_translation,
                                                       .projection_matrix = camera->GetProjectionMatrix()};
        vs_ub_->Upload(rhi_, &view_ubo);
    }
    else if (rhi_->IsBackBufferDirty())
    {
        ScreenQuadVertexShader::UniformBufferData ubo;
        ubo.pre_rotation.topLeftCorner(2, 2) =
            NativeView::GetRotationMatrix(rhi_->GetHardwareInterface()->GetWindowOrientation());

        vs_ub_->UploadImmediate(&ubo);
    }
}

void SkyBoxPass::InitRenderResources(const RenderConfig & /*config*/)
{
    render_target_ = rhi_->CreateRenderTarget({}, color_buffer_, depth_buffer_, "SkyBoxRenderTarget");

    // TODO(tqjxlm): avoid the additional pass here.
    RHIRenderPass::Attribute pass_attribute;
    pass_attribute.color_load_op = RHIRenderPass::LoadOp::Load;
    pass_attribute.color_initial_layout = RHIImageLayout::ColorOutput;

    pass_attribute.depth_load_op = RHIRenderPass::LoadOp::Load;
    pass_attribute.depth_store_op = RHIRenderPass::StoreOp::None;
    pass_attribute.depth_initial_layout = RHIImageLayout::DepthStencilOutput;

    render_pass_ = rhi_->CreateRenderPass(pass_attribute, render_target_, "SkyBoxPass");

    pipeline_state_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Graphics, "SkyBoxPipeline");
    pipeline_state_->SetRenderPass(render_pass_);

    RHIPipelineState::DepthState depth_state;
    depth_state.test_state = RHIPipelineState::DepthTestState::LessEqual;
    depth_state.write_depth = false;
    pipeline_state_->SetDepthState(depth_state);

    RHIPipelineState::RasterizationState rasterization_state;
    rasterization_state.cull_mode = RHIPipelineState::FaceCullMode::Back;
    pipeline_state_->SetRasterizationState(rasterization_state);

    SetupVertexShader();
    SetupPixelShader();
    SetupVertices();

    pipeline_state_->Compile();

    BindShaderResources();
}

void SkyBoxPass::SetupVertexShader()
{
    vertex_shader_ =
        sky_map_to_render_ ? rhi_->CreateShader<SkyBoxVertexShader>() : rhi_->CreateShader<ScreenQuadVertexShader>();

    pipeline_state_->SetShader<RHIShaderStage::Vertex>(vertex_shader_);
}

void SkyBoxPass::SetupPixelShader()
{
    pixel_shader_ =
        sky_map_to_render_ ? rhi_->CreateShader<SkyBoxPixelShader>() : rhi_->CreateShader<SkyLightPixelShader>();
    pipeline_state_->SetShader<RHIShaderStage::Pixel>(pixel_shader_);
}

void SkyBoxPass::SetupVertices()
{
    if (sky_map_to_render_)
    {
        auto unit_cube = Mesh::GetUnitCube();

        vertex_buffer_ =
            rhi_->CreateBuffer({.size = ARRAY_SIZE(unit_cube->vertices),
                                .usages = RHIBuffer::BufferUsage::VertexBuffer,
                                .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                .is_dynamic = false},
                               "UnitBoxVertexBuffer");
        index_buffer_ =
            rhi_->CreateBuffer({.size = ARRAY_SIZE(unit_cube->indices),
                                .usages = RHIBuffer::BufferUsage::IndexBuffer,
                                .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                .is_dynamic = false},
                               "UnitBoxIndexBuffer");

        vertex_buffer_->UploadImmediate(unit_cube->vertices.data());
        index_buffer_->UploadImmediate(unit_cube->indices.data());
    }
    else
    {
        const static std::array<ScreenVertex, 4> Vertices{{
            {.position = Vector3{-1, -1, 0}, .uv = Vector2{0, 0}},
            {.position = Vector3{1, -1, 0}, .uv = Vector2{1, 0}},
            {.position = Vector3{1, 1, 0}, .uv = Vector2{1, 1}},
            {.position = Vector3{-1, 1, 0}, .uv = Vector2{0, 1}},
        }};
        const static std::array<uint32_t, 6> Indices{0, 2, 1, 0, 3, 2};

        vertex_buffer_ =
            rhi_->CreateBuffer({.size = ARRAY_SIZE(Vertices),
                                .usages = RHIBuffer::BufferUsage::VertexBuffer,
                                .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                .is_dynamic = false},
                               "ScreenVertexBuffer");
        index_buffer_ =
            rhi_->CreateBuffer({.size = ARRAY_SIZE(Indices),
                                .usages = RHIBuffer::BufferUsage::IndexBuffer,
                                .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                .is_dynamic = false},
                               "ScreenIndexBuffer");

        vertex_buffer_->UploadImmediate(Vertices.data());
        index_buffer_->UploadImmediate(Indices.data());
    }

    pipeline_state_->SetVertexBuffer(0, vertex_buffer_);
    pipeline_state_->SetIndexBuffer(index_buffer_);

    if (sky_map_to_render_)
    {
        auto &vertex_delcaration = pipeline_state_->GetVertexInputDeclaration();
        vertex_delcaration.SetAttribute(0, 0, {RHIVertexFormat::R32G32B32Float, 0});

        draw_args_.index_count = static_cast<uint32_t>(Mesh::GetUnitCube()->indices.size());
    }
    else
    {
        auto &vertex_delcaration = pipeline_state_->GetVertexInputDeclaration();
        vertex_delcaration.SetAttribute(0, 0, {RHIVertexFormat::R32G32B32Float, offsetof(ScreenVertex, position)});
        vertex_delcaration.SetAttribute(1, 0, {RHIVertexFormat::R32G32Float, offsetof(ScreenVertex, uv)});

        draw_args_.index_count = 6;
    }
}

void SkyBoxPass::BindShaderResources()
{
    if (sky_map_to_render_)
    {
        vs_ub_ = rhi_->CreateBuffer({.size = sizeof(SkyBoxVertexShader::UniformBufferData),
                                     .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                     .mem_properties = RHIMemoryProperty::None,
                                     .is_dynamic = true},
                                    "SkyBoxVSUBO");

        auto *vs_resources = pipeline_state_->GetShaderResource<SkyBoxVertexShader>();
        vs_resources->view().BindResource(vs_ub_);

        auto *ps_resources = pipeline_state_->GetShaderResource<SkyBoxPixelShader>();
        ps_resources->sky_map().BindResource(sky_map_to_render_->GetDefaultView(rhi_));
        ps_resources->sky_map_sampler().BindResource(sky_map_to_render_->GetSampler());
    }
    else
    {
        vs_ub_ = rhi_->CreateBuffer({.size = sizeof(ScreenQuadVertexShader::UniformBufferData),
                                     .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                     .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                     .is_dynamic = false},
                                    "SkyScreenVSUBO");

        auto *vs_resources = pipeline_state_->GetShaderResource<ScreenQuadVertexShader>();
        vs_resources->ubo().BindResource(vs_ub_);

        ps_ub_ = rhi_->CreateBuffer({.size = sizeof(SkyRenderProxy::UniformBufferData),
                                     .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                     .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                     .is_dynamic = false},
                                    "SkyLightUBO");

        auto ubo = sky_proxy_->GetRenderData();
        ps_ub_->UploadImmediate(&ubo);

        auto *ps_resources = pipeline_state_->GetShaderResource<SkyLightPixelShader>();
        ps_resources->ubo().BindResource(ps_ub_);
    }
}
} // namespace sparkle
