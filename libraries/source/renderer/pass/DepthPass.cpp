#include "renderer/pass/DepthPass.h"

#include "../shader/MeshPassVertexShader.h"
#include "renderer/proxy/MeshRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "rhi/RHI.h"

namespace sparkle
{
class DepthOnlyPixelShader : public RHIShaderInfo
{
    REGISTGER_SHADER(DepthOnlyPixelShader, RHIShaderStage::Pixel, "shaders/standard/depth_only.ps.slang", "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    // no resource

    END_SHADER_RESOURCE_TABLE
};

DepthPass::DepthPass(RHIContext *ctx, SceneRenderProxy *scene_proxy, unsigned width, unsigned height)
    : MeshPass(ctx, scene_proxy),
      shadow_map_desc_{
          .format = PixelFormat::D32, .size_class = RGSizeClass::Absolute, .width = width, .height = height}
{
}

void DepthPass::InitRenderResources(const RenderConfig &)
{
    vertex_shader_ = rhi_->CreateShader<DepthOnlyVertexShader>();
    pixel_shader_ = rhi_->CreateShader<DepthOnlyPixelShader>();

    view_buffer_ = rhi_->CreateBuffer({.size = sizeof(ViewUBO),
                                       .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                       .mem_properties = RHIMemoryProperty::None,
                                       .is_dynamic = true},
                                      "ForwardRendererViewUniformBuffer");
}

void DepthPass::HandleNewPrimitive(uint32_t primitive_id)
{
    auto *primitive = scene_proxy_->GetPrimitive(primitive_id);

    auto *mesh_proxy = primitive->As<MeshRenderProxy>();
    const RHIResourceRef<RHIPipelineState> pso =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Graphics, "DepthDrawPipelineState");

    RHIAttachmentSignature signature;
    signature.depth_format = shadow_map_desc_.format;
    pso->SetAttachmentSignature(signature);

    pso->SetShader<RHIShaderStage::Vertex>(vertex_shader_);

    pso->SetShader<RHIShaderStage::Pixel>(pixel_shader_);

    pso->SetVertexBuffer(0, mesh_proxy->GetVertexBuffer());
    pso->SetIndexBuffer(mesh_proxy->GetIndexBuffer());

    auto &vertex_decl = pso->GetVertexInputDeclaration();
    vertex_decl.SetAttribute(0, 0, {RHIVertexFormat::R32G32B32Float, 0});

    pso->Compile();

    // bind resources for this pso
    auto *vs_resources = pso->GetShaderResource<DepthOnlyVertexShader>();
    vs_resources->view().BindResource(view_buffer_);
    vs_resources->mesh().BindResource(mesh_proxy->GetUniformBuffer());

    pipeline_states_.resize(std::max(pipeline_states_.size(), static_cast<size_t>(primitive_id + 1)));
    pipeline_states_[primitive_id] = pso;
}

void DepthPass::SetProjectionMatrix(const Mat4 &matrix)
{
    ViewUBO view_ubo{.view_projection_matrix = matrix};
    view_buffer_->Upload(rhi_, &view_ubo);
}

RGTexture DepthPass::AddTo(RenderGraph &graph) const
{
    const auto shadow_map = graph.CreateTexture("ShadowMap", shadow_map_desc_);
    graph.AddRasterPass("DirectionalShadow", [this, shadow_map](RGBuilder &builder) {
        builder.DepthWrite(shadow_map, 1.f);
        return [this](RGRasterContext &context) { DrawPrimitives(context); };
    });
    return shadow_map;
}
} // namespace sparkle
