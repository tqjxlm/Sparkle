#include "renderer/pass/ForwardMeshPass.h"

#include "../shader/MeshPassVertexShader.h"
#include "renderer/BindlessManager.h"
#include "renderer/RenderConfig.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/DirectionalLightRenderProxy.h"
#include "renderer/proxy/MaterialRenderProxy.h"
#include "renderer/proxy/MeshRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "renderer/resource/ImageBasedLighting.h"
#include "renderer/resource/PbrResource.h"
#include "renderer/resource/SSAOResource.h"
#include "rhi/RHI.h"
#include "rhi/RHIRayTracing.h"

namespace sparkle
{
class ForwardPixelShaderBase : public RHIShaderInfo
{
    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(mesh, RHIShaderResourceReflection::ResourceType::UniformBuffer)
    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)

    USE_SHADER_RESOURCE(material, RHIShaderResourceReflection::ResourceType::UniformBuffer)
    USE_SHADER_RESOURCE(base_color_texture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(normal_texture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(metallic_roughness_texture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(emissive_texture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(material_texture_sampler, RHIShaderResourceReflection::ResourceType::Sampler)

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
        alignas(16) SSAOConfig ssao_config;
    };
};

class ForwardPixelShader : public ForwardPixelShaderBase
{
    REGISTGER_SHADER(ForwardPixelShader, RHIShaderStage::Pixel, "shaders/standard/forward.ps.hlsl", "main")

    BEGIN_SHADER_RESOURCE_TABLE(ForwardPixelShaderBase::ResourceTable)

    USE_SHADER_RESOURCE(view, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)

    USE_SHADER_RESOURCE(shadow_map, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(shadow_map_sampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(prepass_depth_map, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prepass_depth_map_sampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(ibl_brdf, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(ibl_brdf_sampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(ibl_diffuse, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(ibl_diffuse_sampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(ibl_specular, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(ibl_specular_sampler, RHIShaderResourceReflection::ResourceType::Sampler)

    END_SHADER_RESOURCE_TABLE
};

class ForwardRayTracingPixelShader : public ForwardPixelShaderBase
{
    REGISTGER_SHADER(ForwardRayTracingPixelShader, RHIShaderStage::Pixel, "shaders/ray_trace/standard_ray_trace.frag",
                     "main")

    BEGIN_SHADER_RESOURCE_TABLE(ForwardPixelShaderBase::ResourceTable)

    USE_SHADER_RESOURCE(topLevelAS, RHIShaderResourceReflection::ResourceType::AccelerationStructure)
    USE_SHADER_RESOURCE(materialIdBuffer, RHIShaderResourceReflection::ResourceType::StorageBuffer)
    USE_SHADER_RESOURCE(materialBuffer, RHIShaderResourceReflection::ResourceType::StorageBuffer)

    USE_SHADER_RESOURCE_BINDLESS(textures, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE_BINDLESS(indexBuffers, RHIShaderResourceReflection::ResourceType::StorageBuffer)
    USE_SHADER_RESOURCE_BINDLESS(vertexBuffers, RHIShaderResourceReflection::ResourceType::StorageBuffer)
    USE_SHADER_RESOURCE_BINDLESS(vertexAttributeBuffers, RHIShaderResourceReflection::ResourceType::StorageBuffer)

    END_SHADER_RESOURCE_TABLE
};

ForwardMeshPass::ForwardMeshPass(RHIContext *ctx, SceneRenderProxy *scene_proxy, PassResources resources)
    : MeshPass(ctx, scene_proxy), resources_(std::move(resources))
{
    ASSERT(resources_.scene_color);
    ASSERT(resources_.scene_depth);
}

void ForwardMeshPass::InitRenderResources(const RenderConfig &config)
{
    use_ray_tracing_ = config.IsRayTracingMode();
    use_prepass_ = config.use_prepass;
    bool resolve_prepass_depth = config.use_ssao;

    vertex_shader_ = rhi_->CreateShader<StandardVertexShader>();

    pixel_shader_ = use_ray_tracing_ ? rhi_->CreateShader<ForwardRayTracingPixelShader>()
                                     : rhi_->CreateShader<ForwardPixelShader>();

    uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ForwardPixelShaderBase::UniformBufferData),
                                          .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                          .mem_properties = RHIMemoryProperty::None,
                                          .is_dynamic = true},
                                         "ForwardRendererUniformBuffer");

    render_target_ =
        rhi_->CreateRenderTarget({}, resources_.scene_color, resources_.scene_depth, "BasePassRenderTarget");

    RHIRenderPass::Attribute pass_attribute;
    pass_attribute.color_load_op = RHIRenderPass::LoadOp::Clear;
    pass_attribute.depth_store_op = RHIRenderPass::StoreOp::Store;

    if (use_prepass_)
    {
        pass_attribute.depth_load_op = RHIRenderPass::LoadOp::Load;
        if (resolve_prepass_depth)
        {
            // if it is resolved, it should have just been transferred
            pass_attribute.depth_initial_layout = RHIImageLayout::TransferSrc;
        }
        else
        {
            pass_attribute.depth_initial_layout = RHIImageLayout::ColorOutput;
        }
    }
    else
    {
        pass_attribute.depth_load_op = RHIRenderPass::LoadOp::Clear;
    }

    base_pass_ = rhi_->CreateRenderPass(pass_attribute, render_target_, "BasePass");
}

void ForwardMeshPass::SetupVertices(const RHIResourceRef<RHIPipelineState> &pso, MeshRenderProxy *mesh_proxy,
                                    bool use_prepass)
{
    pso->SetVertexBuffer(0, mesh_proxy->GetVertexBuffer());
    pso->SetVertexBuffer(1, mesh_proxy->GetVertexAttribBuffer());
    pso->SetIndexBuffer(mesh_proxy->GetIndexBuffer());

    if (use_prepass)
    {
        pso->SetDepthState({.test_state = RHIPipelineState::DepthTestState::Equal, .write_depth = false});
    }

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

    bool use_ssao = config.use_ssao;
    // bool ibl_ready = (ibl_ != nullptr) && !ibl_->NeedUpdate();
    bool use_diffuse_ibl = (ibl_ != nullptr) && config.use_diffuse_ibl;
    bool use_specular_ibl = (ibl_ != nullptr) && config.use_specular_ibl;

    if (ibl_dirty_)
    {
        ibl_dirty_ = false;
        RebindAllShaderResources();
    }

    auto *sky_light = scene->GetSkyLight();
    auto *camera = scene->GetCamera();
    auto *directional_light = scene->GetDirectionalLight();

    const PbrConfig pbr_config{.mode = static_cast<uint32_t>(config.debug_mode),
                               .use_ssao = static_cast<uint32_t>(use_ssao ? 1 : 0),
                               .use_ibl_diffuse = static_cast<uint32_t>(use_diffuse_ibl ? 1 : 0),
                               .use_ibl_specular = static_cast<uint32_t>(use_specular_ibl ? 1 : 0)};

    ForwardPixelShaderBase::UniformBufferData ubo{.sky_light = {},
                                                  .dir_light = {},
                                                  .view_pos = camera->GetTranslation(),
                                                  .render_config = pbr_config,
                                                  .ssao_config = {}};

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

static void BindMeshResources(const RHIResourceRef<RHIPipelineState> &pso, MeshRenderProxy *mesh_proxy,
                              bool use_ray_tracing, RHIContext *rhi)
{
    auto *vs_resources = pso->GetShaderResource<StandardVertexShader>();
    vs_resources->mesh().BindResource(mesh_proxy->GetUniformBuffer());

    ForwardPixelShaderBase::ResourceTable *ps_resources;
    if (use_ray_tracing)
    {
        ps_resources = pso->GetShaderResource<ForwardRayTracingPixelShader>();
    }
    else
    {
        ps_resources = pso->GetShaderResource<ForwardPixelShader>();
    }

    ps_resources->BindMeshResources(mesh_proxy, rhi);
}

void ForwardMeshPass::BindPassResources(const RHIResourceRef<RHIPipelineState> &pso) const
{
    auto view_buffer = scene_proxy_->GetCamera()->GetViewBuffer();

    auto *vs_resources = pso->GetShaderResource<StandardVertexShader>();
    vs_resources->view().BindResource(view_buffer);

    if (use_ray_tracing_)
    {
        auto *ps_resources = pso->GetShaderResource<ForwardRayTracingPixelShader>();

        ps_resources->topLevelAS().BindResource(resources_.tlas);
        ps_resources->ubo().BindResource(uniform_buffer_);

        const auto *bindless_manager = scene_proxy_->GetBindlessManager();

        ps_resources->textures().BindResource(bindless_manager->GetBindlessBuffer(BindlessResourceType::Texture));
        ps_resources->indexBuffers().BindResource(
            bindless_manager->GetBindlessBuffer(BindlessResourceType::IndexBuffer));
        ps_resources->vertexBuffers().BindResource(
            bindless_manager->GetBindlessBuffer(BindlessResourceType::VertexBuffer));
        ps_resources->vertexAttributeBuffers().BindResource(
            bindless_manager->GetBindlessBuffer(BindlessResourceType::VertexAttributeBuffer));
        ps_resources->materialIdBuffer().BindResource(bindless_manager->GetMaterialIdBuffer());
        ps_resources->materialBuffer().BindResource(bindless_manager->GetMaterialParameterBuffer());
    }
    else
    {
        auto *ps_resources = pso->GetShaderResource<ForwardPixelShader>();

        ps_resources->view().BindResource(view_buffer);
        ps_resources->ubo().BindResource(uniform_buffer_);

        auto dummy_texture_2d = rhi_->GetOrCreateDummyTexture(RHIImage::Attribute{
            .format = PixelFormat::RGBAFloat16,
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
            .usages = RHIImage::ImageUsage::Texture,
        });

        auto dummy_texture_cube = rhi_->GetOrCreateDummyTexture(RHIImage::Attribute{
            .format = PixelFormat::RGBAFloat16,
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
            .usages = RHIImage::ImageUsage::Texture,
            .type = RHIImage::ImageType::Image2DCube,
        });

        if (shadow_map_)
        {
            ps_resources->shadow_map().BindResource(shadow_map_->GetDefaultView(rhi_));
            ps_resources->shadow_map_sampler().BindResource(shadow_map_->GetSampler());
        }
        else
        {
            ps_resources->shadow_map().BindResource(dummy_texture_2d->GetDefaultView(rhi_));
            ps_resources->shadow_map_sampler().BindResource(dummy_texture_2d->GetSampler());
        }

        if (resources_.prepass_depth_map)
        {
            ps_resources->prepass_depth_map().BindResource(resources_.prepass_depth_map->GetDefaultView(rhi_));
            ps_resources->prepass_depth_map_sampler().BindResource(resources_.prepass_depth_map->GetSampler());
        }
        else
        {
            ps_resources->prepass_depth_map().BindResource(dummy_texture_2d->GetDefaultView(rhi_));
            ps_resources->prepass_depth_map_sampler().BindResource(dummy_texture_2d->GetSampler());
        }

        auto ibl_brdf = (ibl_ && ibl_->GetBRDFMap()) ? ibl_->GetBRDFMap() : dummy_texture_2d;
        ps_resources->ibl_brdf().BindResource(ibl_brdf->GetDefaultView(rhi_));
        ps_resources->ibl_brdf_sampler().BindResource(ibl_brdf->GetSampler());

        auto ibl_diffuse = (ibl_ && ibl_->GetDiffuseMap()) ? ibl_->GetDiffuseMap() : dummy_texture_cube;
        ps_resources->ibl_diffuse().BindResource(ibl_diffuse->GetDefaultView(rhi_));
        ps_resources->ibl_diffuse_sampler().BindResource(ibl_diffuse->GetSampler());

        auto ibl_specualr = (ibl_ && ibl_->GetSpecularMap()) ? ibl_->GetSpecularMap() : dummy_texture_cube;
        ps_resources->ibl_specular().BindResource(ibl_specualr->GetDefaultView(rhi_));
        ps_resources->ibl_specular_sampler().BindResource(ibl_specualr->GetSampler());
    }
}

void ForwardMeshPass::HandleNewPrimitive(uint32_t primitive_id)
{
    auto *primitive = scene_proxy_->GetPrimitives()[primitive_id];

    auto *mesh_proxy = primitive->As<MeshRenderProxy>();

    pipeline_states_.resize(std::max(pipeline_states_.size(), static_cast<size_t>(primitive_id + 1)));

    ASSERT_F(pipeline_states_[primitive_id] == nullptr,
             "do not overwrite an existing primitive. remove or move it first!");

    pipeline_states_[primitive_id] =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Graphics, "ForwardMeshPipelineState");

    auto &pso = pipeline_states_[primitive_id];

    pso->SetRenderPass(base_pass_);

    SetupVertexShader(pso);
    SetupPixelShader(pso);
    SetupVertices(pso, mesh_proxy, use_prepass_);

    pso->Compile();

    BindMeshResources(pso, mesh_proxy, use_ray_tracing_, rhi_);

    BindPassResources(pso);
}

void ForwardMeshPass::HandleUpdatedPrimitive([[maybe_unused]] uint32_t primitive_id)
{
}

void ForwardMeshPass::SetDirectionalShadow(const RHIResourceRef<RHIImage> &shadow_map)
{
    if (shadow_map_ == shadow_map)
    {
        return;
    }

    shadow_map_ = shadow_map;

    RebindAllShaderResources();
}

void ForwardMeshPass::SetIBL(ImageBasedLighting *ibl)
{
    if (ibl_ == ibl)
    {
        return;
    }

    ibl_ = ibl;

    if (ibl_->NeedUpdate())
    {
        ibl_changed_subscription_ = ibl_->OnRenderResourceChange().Subscribe([this]() { ibl_dirty_ = true; });
    }
    else
    {
        RebindAllShaderResources();
    }
}

void ForwardMeshPass::RebindAllShaderResources()
{
    for (auto &pso : pipeline_states_)
    {
        if (pso != nullptr)
        {
            BindPassResources(pso);
        }
    }
}

void ForwardMeshPass::Render()
{
    rhi_->BeginRenderPass(base_pass_);

    MeshPass::Render();

    rhi_->EndRenderPass();
}
} // namespace sparkle
