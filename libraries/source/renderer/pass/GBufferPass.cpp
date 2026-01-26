#include "renderer/pass/GBufferPass.h"

#include "../shader/MeshPassVertexShader.h"
#include "renderer/RenderConfig.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/MaterialRenderProxy.h"
#include "renderer/proxy/MeshRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "rhi/RHI.h"

namespace sparkle
{
class GBufferPassPixelShader : public RHIShaderInfo
{
    REGISTGER_SHADER(GBufferPassPixelShader, RHIShaderStage::Pixel, "shaders/standard/gbuffer_pass.ps.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(mesh, RHIShaderResourceReflection::ResourceType::UniformBuffer)

    USE_SHADER_RESOURCE(material, RHIShaderResourceReflection::ResourceType::UniformBuffer)
    USE_SHADER_RESOURCE(base_color_texture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(normal_texture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(metallic_roughness_texture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(emissive_texture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(material_texture_sampler, RHIShaderResourceReflection::ResourceType::Sampler)

public:
    void BindResources(MeshRenderProxy *mesh_proxy, RHIContext *rhi)
    {
        auto *material_proxy = mesh_proxy->GetMaterialRenderProxy();

        ASSERT(material_proxy);
        ASSERT_F(material_proxy->GetUBO(), "{}", material_proxy->GetName());

        mesh().BindResource(mesh_proxy->GetUniformBuffer());
        material().BindResource(material_proxy->GetUBO());

        base_color_texture().BindResource(material_proxy->GetBaseColorTexture()->GetDefaultView(rhi));
        normal_texture().BindResource(material_proxy->GetNormalTexture()->GetDefaultView(rhi));
        metallic_roughness_texture().BindResource(material_proxy->GetMetallicRoughnessTexture()->GetDefaultView(rhi));
        emissive_texture().BindResource(material_proxy->GetEmissiveTexture()->GetDefaultView(rhi));

        material_texture_sampler().BindResource(material_proxy->GetBaseColorTexture()->GetSampler());
    }

    END_SHADER_RESOURCE_TABLE
};

GBufferPass::GBufferPass(RHIContext *ctx, SceneRenderProxy *scene_proxy,
                         RHIRenderTarget::ColorImageArray gbuffer_images, const RHIResourceRef<RHIImage> &scene_depth)
    : MeshPass(ctx, scene_proxy), scene_depth_(scene_depth), gbuffer_images_(std::move(gbuffer_images))
{
}

void GBufferPass::InitRenderResources(const RenderConfig &)
{
    vertex_shader_ = rhi_->CreateShader<StandardVertexShader>();
    pixel_shader_ = rhi_->CreateShader<GBufferPassPixelShader>();

    render_target_ = rhi_->CreateRenderTarget({}, gbuffer_images_, scene_depth_, "GBufferPassRT");

    RHIRenderPass::Attribute pass_attribute;
    pass_attribute.color_load_op = RHIRenderPass::LoadOp::Clear;
    pass_attribute.depth_load_op = RHIRenderPass::LoadOp::Clear;
    pass_attribute.depth_store_op = RHIRenderPass::StoreOp::Store;

    pass_ = rhi_->CreateRenderPass(pass_attribute, render_target_, "GBufferPass");
}

void GBufferPass::SetupVertices(const RHIResourceRef<RHIPipelineState> &pso, MeshRenderProxy *mesh_proxy)
{
    pso->SetVertexBuffer(0, mesh_proxy->GetVertexBuffer());
    pso->SetVertexBuffer(1, mesh_proxy->GetVertexAttribBuffer());
    pso->SetIndexBuffer(mesh_proxy->GetIndexBuffer());

    auto &vertex_decl = pso->GetVertexInputDeclaration();
    vertex_decl.SetAttribute(0, 0, {RHIVertexFormat::R32G32B32Float, 0});
    vertex_decl.SetAttribute(1, 1,
                             {RHIVertexFormat::R32G32B32A32Float, offsetof(MeshRenderProxy::VertexAttribute, tangent)});
    vertex_decl.SetAttribute(
        2, 1, {RHIVertexFormat::R32G32B32Float, offsetof(MeshRenderProxy::VertexAttribute, normal), sizeof(Vector4)});
    vertex_decl.SetAttribute(
        3, 1, {RHIVertexFormat::R32G32Float, offsetof(MeshRenderProxy::VertexAttribute, tex_coord), sizeof(Vector4)});
}

void GBufferPass::SetupVertexShader(const RHIResourceRef<RHIPipelineState> &pso) const
{
    pso->SetShader<RHIShaderStage::Vertex>(vertex_shader_);
}

void GBufferPass::SetupPixelShader(const RHIResourceRef<RHIPipelineState> &pso) const
{
    pso->SetShader<RHIShaderStage::Pixel>(pixel_shader_);
}

void GBufferPass::BindShaderResources(const RHIResourceRef<RHIPipelineState> &pso, MeshRenderProxy *mesh_proxy) const
{
    auto *vs_resources = pso->GetShaderResource<StandardVertexShader>();
    vs_resources->mesh().BindResource(mesh_proxy->GetUniformBuffer());
    vs_resources->view().BindResource(scene_proxy_->GetCamera()->GetViewBuffer());

    auto *ps_resources = pso->GetShaderResource<GBufferPassPixelShader>();
    ps_resources->BindResources(mesh_proxy, rhi_);
}

void GBufferPass::HandleNewPrimitive(uint32_t primitive_id)
{
    auto *primitive = scene_proxy_->GetPrimitive(primitive_id);

    auto *mesh_proxy = primitive->As<MeshRenderProxy>();

    pipeline_states_.resize(std::max(pipeline_states_.size(), static_cast<size_t>(primitive_id + 1)));

    ASSERT_F(pipeline_states_[primitive_id] == nullptr,
             "do not overwrite an existing primitive. remove or move it first!");

    pipeline_states_[primitive_id] =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Graphics, "GBufferPassMeshPipelineState");

    auto &pso = pipeline_states_[primitive_id];

    pso->SetRenderPass(pass_);

    SetupVertexShader(pso);
    SetupPixelShader(pso);
    SetupVertices(pso, mesh_proxy);

    pso->Compile();

    BindShaderResources(pso, mesh_proxy);
}

void GBufferPass::HandleUpdatedPrimitive([[maybe_unused]] uint32_t primitive_id)
{
}

void GBufferPass::Render()
{
    rhi_->BeginRenderPass(pass_);

    MeshPass::Render();

    rhi_->EndRenderPass();
}
} // namespace sparkle
