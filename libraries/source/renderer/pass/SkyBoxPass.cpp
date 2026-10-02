#include "renderer/pass/SkyBoxPass.h"

#include "core/math/Utilities.h"
#include "io/Mesh.h"
#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/ColorSlot.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "rhi/RHI.h"

namespace sparkle
{
class SkyBoxVertexShader : public RHIShaderInfo
{
    REGISTGER_SHADER(SkyBoxVertexShader, RHIShaderStage::Vertex, "shaders/standard/sky_box.vs.slang", "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(view, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)

    END_SHADER_RESOURCE_TABLE

    struct UniformBufferData
    {
        Mat4 view_matrix;
        Mat4 projection_matrix;
    };
};

class SkyBoxPixelShader : public RHIShaderInfo
{
    REGISTGER_SHADER(SkyBoxPixelShader, RHIShaderStage::Pixel, "shaders/standard/sky_box.ps.slang", "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(sky_map, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(sky_map_sampler, RHIShaderResourceReflection::ResourceType::Sampler)

    END_SHADER_RESOURCE_TABLE
};

SkyBoxPass::SkyBoxPass(RHIContext *rhi, PixelFormat color_format, PixelFormat depth_format) : PipelinePass(rhi)
{
    signature_.color_formats[ColorSlot::SceneColor] = color_format;
    signature_.depth_format = depth_format;
}

void SkyBoxPass::UpdateFrameData(const RenderConfig & /*config*/, SceneRenderProxy *scene)
{
    const auto *camera = scene->GetCamera();
    Mat4 view_matrix_without_translation = Mat4::Zero();
    view_matrix_without_translation.topLeftCorner<3, 3>() = camera->GetViewMatrix().topLeftCorner<3, 3>();
    view_matrix_without_translation(3, 3) = 1;
    SkyBoxVertexShader::UniformBufferData view_ubo{.view_matrix = view_matrix_without_translation,
                                                   .projection_matrix = camera->GetProjectionMatrix()};
    vs_ub_->Upload(rhi_, &view_ubo);
}

void SkyBoxPass::InitRenderResources(const RenderConfig & /*config*/)
{
    pipeline_state_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Graphics, "SkyBoxPipeline");
    pipeline_state_->SetAttachmentSignature(signature_);

    RHIPipelineState::DepthState depth_state;
    depth_state.test_state = RHIPipelineState::DepthTestState::LessEqual;
    depth_state.write_depth = false;
    pipeline_state_->SetDepthState(depth_state);

    RHIPipelineState::RasterizationState rasterization_state;
    rasterization_state.cull_mode = RHIPipelineState::FaceCullMode::Back;
    pipeline_state_->SetRasterizationState(rasterization_state);

    vertex_shader_ = rhi_->CreateShader<SkyBoxVertexShader>();
    pipeline_state_->SetShader<RHIShaderStage::Vertex>(vertex_shader_);

    pixel_shader_ = rhi_->CreateShader<SkyBoxPixelShader>();
    pipeline_state_->SetShader<RHIShaderStage::Pixel>(pixel_shader_);

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

    pipeline_state_->SetVertexBuffer(0, vertex_buffer_);
    pipeline_state_->SetIndexBuffer(index_buffer_);

    auto &vertex_delcaration = pipeline_state_->GetVertexInputDeclaration();
    vertex_delcaration.SetAttribute(0, 0, {RHIVertexFormat::R32G32B32Float, 0});

    draw_args_.index_count = static_cast<uint32_t>(unit_cube->indices.size());

    pipeline_state_->Compile();

    vs_ub_ = rhi_->CreateBuffer({.size = sizeof(SkyBoxVertexShader::UniformBufferData),
                                 .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                 .mem_properties = RHIMemoryProperty::None,
                                 .is_dynamic = true},
                                "SkyBoxVSUBO");

    pipeline_state_->GetShaderResource<SkyBoxVertexShader>()->view().BindResource(vs_ub_);
}

void SkyBoxPass::AddTo(RenderGraph &graph, RGTexture sky_map, const RHISampler::SamplerAttribute &sky_map_sampler,
                       RGTexture scene_color, RGTexture scene_depth) const
{
    graph.AddRasterPass("SkyBox", [this, sky_map, sky_map_sampler, scene_color, scene_depth](RGBuilder &builder) {
        using Table = SkyBoxPixelShader::ResourceTable;
        builder.Sampled(sky_map, &Table::sky_map, &Table::sky_map_sampler, sky_map_sampler);
        builder.ColorWrite(scene_color, ColorSlot::SceneColor);
        builder.DepthTest(scene_depth);
        return [this](RGRasterContext &context) { context.DrawMesh(pipeline_state_, draw_args_); };
    });
}
} // namespace sparkle
