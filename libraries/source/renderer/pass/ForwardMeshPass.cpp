#include "renderer/pass/ForwardMeshPass.h"

#include "../shader/MeshPassVertexShader.h"
#include "renderer/RenderConfig.h"
#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/LightingInputs.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/DirectionalLightRenderProxy.h"
#include "renderer/proxy/MaterialRenderProxy.h"
#include "renderer/proxy/MeshRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "renderer/resource/PbrResource.h"
#include "rhi/RHI.h"

namespace sparkle
{
class ForwardPixelShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ForwardPixelShader, RHIShaderStage::Pixel, "shaders/standard/forward.ps.slang", "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(mesh, RHIShaderResourceReflection::ResourceType::UniformBuffer)
    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)

    USE_SHADER_RESOURCE(material, RHIShaderResourceReflection::ResourceType::UniformBuffer)
    USE_SHADER_RESOURCE(base_color_texture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(normal_texture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(metallic_roughness_texture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(emissive_texture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(material_texture_sampler, RHIShaderResourceReflection::ResourceType::Sampler)

    USE_SHADER_RESOURCE(view, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)

    USE_SHADER_RESOURCE(shadow_map, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(shadow_map_sampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(ibl_brdf, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(ibl_brdf_sampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(ibl_diffuse, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(ibl_diffuse_sampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(ibl_specular, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(ibl_specular_sampler, RHIShaderResourceReflection::ResourceType::Sampler)

public:
    void BindMeshResources(MeshRenderProxy *mesh_proxy, RHIContext *rhi)
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

    struct UniformBufferData
    {
        SkyRenderProxy::UniformBufferData sky_light;
        DirectionalLightRenderProxy::UniformBufferData dir_light;
        alignas(16) Vector3 view_pos;
        alignas(16) PbrConfig render_config;
    };
};

ForwardMeshPass::ForwardMeshPass(RHIContext *ctx, SceneRenderProxy *scene_proxy, PixelFormat color_format,
                                 PixelFormat depth_format)
    : MeshPass(ctx, scene_proxy)
{
    signature_.color_formats[0] = color_format;
    signature_.depth_format = depth_format;
}

void ForwardMeshPass::InitRenderResources(const RenderConfig &)
{
    vertex_shader_ = rhi_->CreateShader<StandardVertexShader>();

    pixel_shader_ = rhi_->CreateShader<ForwardPixelShader>();

    uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ForwardPixelShader::UniformBufferData),
                                          .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                          .mem_properties = RHIMemoryProperty::None,
                                          .is_dynamic = true},
                                         "ForwardRendererUniformBuffer");
}

void ForwardMeshPass::SetupVertices(const RHIResourceRef<RHIPipelineState> &pso, MeshRenderProxy *mesh_proxy)
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

void ForwardMeshPass::SetupVertexShader(const RHIResourceRef<RHIPipelineState> &pso) const
{
    pso->SetShader<RHIShaderStage::Vertex>(vertex_shader_);
}

void ForwardMeshPass::SetupPixelShader(const RHIResourceRef<RHIPipelineState> &pso) const
{
    pso->SetShader<RHIShaderStage::Pixel>(pixel_shader_);
}

void ForwardMeshPass::UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene)
{
    MeshPass::UpdateFrameData(config, scene);

    auto *sky_light = scene->GetSkyLight();
    auto *camera = scene->GetCamera();
    auto *directional_light = scene->GetDirectionalLight();

    const bool has_ibl = sky_light != nullptr && sky_light->GetImageBasedLighting() != nullptr;
    const bool use_diffuse_ibl = has_ibl && config.use_diffuse_ibl;
    const bool use_specular_ibl = has_ibl && config.use_specular_ibl;

    const PbrConfig pbr_config{.mode = static_cast<uint32_t>(config.debug_mode),
                               .use_ibl_diffuse = static_cast<uint32_t>(use_diffuse_ibl ? 1 : 0),
                               .use_ibl_specular = static_cast<uint32_t>(use_specular_ibl ? 1 : 0)};

    ForwardPixelShader::UniformBufferData ubo{
        .sky_light = {}, .dir_light = {}, .view_pos = camera->GetPosture().position, .render_config = pbr_config};

    if (sky_light)
    {
        ubo.sky_light = sky_light->GetRenderData();
    }

    if (directional_light)
    {
        ubo.dir_light = directional_light->GetRenderData();
    }

    uniform_buffer_->Upload(rhi_, &ubo);
}

static void BindMeshResources(const RHIResourceRef<RHIPipelineState> &pso, MeshRenderProxy *mesh_proxy, RHIContext *rhi)
{
    auto *vs_resources = pso->GetShaderResource<StandardVertexShader>();
    vs_resources->mesh().BindResource(mesh_proxy->GetUniformBuffer());

    pso->GetShaderResource<ForwardPixelShader>()->BindMeshResources(mesh_proxy, rhi);
}

void ForwardMeshPass::BindPassResources(const RHIResourceRef<RHIPipelineState> &pso) const
{
    auto view_buffer = scene_proxy_->GetCamera()->GetViewBuffer();

    auto *vs_resources = pso->GetShaderResource<StandardVertexShader>();
    vs_resources->view().BindResource(view_buffer);

    auto *ps_resources = pso->GetShaderResource<ForwardPixelShader>();

    ps_resources->view().BindResource(view_buffer);
    ps_resources->ubo().BindResource(uniform_buffer_);
}

void ForwardMeshPass::HandleNewPrimitive(uint32_t primitive_id)
{
    auto *primitive = scene_proxy_->GetPrimitive(primitive_id);

    auto *mesh_proxy = primitive->As<MeshRenderProxy>();

    pipeline_states_.resize(std::max(pipeline_states_.size(), static_cast<size_t>(primitive_id + 1)));

    ASSERT_F(pipeline_states_[primitive_id] == nullptr,
             "do not overwrite an existing primitive. remove or move it first!");

    pipeline_states_[primitive_id] =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Graphics, "ForwardMeshPipelineState");

    auto &pso = pipeline_states_[primitive_id];

    pso->SetAttachmentSignature(signature_);

    SetupVertexShader(pso);
    SetupPixelShader(pso);
    SetupVertices(pso, mesh_proxy);

    pso->Compile();

    BindMeshResources(pso, mesh_proxy, rhi_);

    BindPassResources(pso);
}

void ForwardMeshPass::HandleUpdatedPrimitive([[maybe_unused]] uint32_t primitive_id)
{
}

void ForwardMeshPass::AddTo(RenderGraph &graph, const LightingInputs &lighting, RGTexture scene_color,
                            RGTexture scene_depth) const
{
    graph.AddRasterPass("BasePass", [this, lighting, scene_color, scene_depth](RGBuilder &builder) {
        lighting.Sample<ForwardPixelShader::ResourceTable>(builder, rhi_);
        builder.ColorWrite(scene_color, 0, Vector4(0.f, 0.f, 0.f, 1.f));
        builder.DepthWrite(scene_depth, 1.f);
        return [this](RGRasterContext &context) { DrawPrimitives(context); };
    });
}
} // namespace sparkle
