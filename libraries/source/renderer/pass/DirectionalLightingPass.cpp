#include "renderer/pass/DirectionalLightingPass.h"

#include "renderer/RenderConfig.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/DirectionalLightRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "renderer/resource/ImageBasedLighting.h"
#include "renderer/resource/PbrResource.h"
#include "renderer/resource/SSAOResource.h"
#include "rhi/RHI.h"

namespace sparkle
{
class DirectionalLightingPassPixelShader : public RHIShaderInfo
{
    REGISTGER_SHADER(DirectionalLightingPassPixelShader, RHIShaderStage::Pixel,
                     "shaders/screen/directional_lighting.ps.hlsl", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(view, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)

    USE_SHADER_RESOURCE(shadow_map, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(shadow_map_sampler, RHIShaderResourceReflection::ResourceType::Sampler)

    USE_SHADER_RESOURCE(ibl_brdf, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(ibl_brdf_sampler, RHIShaderResourceReflection::ResourceType::Sampler)

    USE_SHADER_RESOURCE(ibl_diffuse, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(ibl_diffuse_sampler, RHIShaderResourceReflection::ResourceType::Sampler)

    USE_SHADER_RESOURCE(ibl_specular, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(ibl_specular_sampler, RHIShaderResourceReflection::ResourceType::Sampler)

    USE_SHADER_RESOURCE(gbuffer_texture, RHIShaderResourceReflection::ResourceType::Texture2D)

    USE_SHADER_RESOURCE(depth_texture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(depth_sampler, RHIShaderResourceReflection::ResourceType::Sampler)

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

DirectionalLightingPass::DirectionalLightingPass(RHIContext *ctx, const RHIResourceRef<RHIRenderTarget> &target,
                                                 PassResources resources)
    : ScreenQuadPass(ctx, nullptr, target), resources_(std::move(resources))
{
}

void DirectionalLightingPass::UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene)
{
    ScreenQuadPass::UpdateFrameData(config, scene);

    bool use_ssao = config.use_ssao;
    // bool ibl_ready = (resources_.ibl != nullptr) && !resources_.ibl->NeedUpdate();
    bool use_diffuse_ibl = (resources_.ibl != nullptr) && config.use_diffuse_ibl;
    bool use_specular_ibl = (resources_.ibl != nullptr) && config.use_specular_ibl;

    if (ibl_dirty_)
    {
        ibl_dirty_ = false;
        BindPixelShaderResources();
    }

    auto *sky_light = scene->GetSkyLight();
    auto *camera = scene->GetCamera();
    auto *dir_light = scene->GetDirectionalLight();

    const PbrConfig pbr_config{.mode = static_cast<uint32_t>(config.debug_mode),
                               .use_ssao = static_cast<uint32_t>(use_ssao ? 1 : 0),
                               .use_ibl_diffuse = static_cast<uint32_t>(use_diffuse_ibl ? 1 : 0),
                               .use_ibl_specular = static_cast<uint32_t>(use_specular_ibl ? 1 : 0)};

    DirectionalLightingPassPixelShader::UniformBufferData ubo{
        .sky_light = sky_light ? sky_light->GetRenderData() : SkyRenderProxy::UniformBufferData{},
        .dir_light = dir_light ? dir_light->GetRenderData() : DirectionalLightRenderProxy::UniformBufferData{},
        .view_pos = camera->GetPosture().position,
        .render_config = pbr_config,
        .ssao_config = {}};

    ps_ub_->Upload(rhi_, &ubo);
}

void DirectionalLightingPass::SetupPixelShader()
{
    pixel_shader_ = rhi_->CreateShader<DirectionalLightingPassPixelShader>();
    pipeline_state_->SetShader<RHIShaderStage::Pixel>(pixel_shader_);
}

void DirectionalLightingPass::BindPixelShaderResources()
{
    auto *ps_resources = pipeline_state_->GetShaderResource<DirectionalLightingPassPixelShader>();

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

    if (resources_.shadow_map)
    {
        ps_resources->shadow_map().BindResource(resources_.shadow_map->GetDefaultView(rhi_));
        ps_resources->shadow_map_sampler().BindResource(resources_.shadow_map->GetSampler());
    }
    else
    {
        ps_resources->shadow_map().BindResource(dummy_texture_2d->GetDefaultView(rhi_));
        ps_resources->shadow_map_sampler().BindResource(dummy_texture_2d->GetSampler());
    }

    ps_ub_ = rhi_->CreateBuffer({.size = sizeof(DirectionalLightingPassPixelShader::UniformBufferData),
                                 .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                 .mem_properties = RHIMemoryProperty::None,
                                 .is_dynamic = true},
                                "DirectionalLightingPassUniformBuffer");

    ps_resources->view().BindResource(resources_.camera->GetViewBuffer());

    ps_resources->ubo().BindResource(ps_ub_);

    auto *ibl = resources_.ibl;

    auto ibl_brdf = ibl ? ibl->GetBRDFMap() : dummy_texture_2d;
    auto ibl_diffuse = (ibl && ibl->GetDiffuseMap()) ? ibl->GetDiffuseMap() : dummy_texture_cube;
    auto ibl_specualr = (ibl && ibl->GetSpecularMap()) ? ibl->GetSpecularMap() : dummy_texture_cube;

    ps_resources->ibl_brdf().BindResource(ibl_brdf->GetDefaultView(rhi_));
    ps_resources->ibl_brdf_sampler().BindResource(ibl_brdf->GetSampler());

    ps_resources->ibl_diffuse().BindResource(ibl_diffuse->GetDefaultView(rhi_));
    ps_resources->ibl_diffuse_sampler().BindResource(ibl_diffuse->GetSampler());

    ps_resources->ibl_specular().BindResource(ibl_specualr->GetDefaultView(rhi_));
    ps_resources->ibl_specular_sampler().BindResource(ibl_specualr->GetSampler());

    ps_resources->gbuffer_texture().BindResource(resources_.gbuffer.packed_texture->GetDefaultView(rhi_));

    ps_resources->depth_texture().BindResource(resources_.depth_texture->GetDefaultView(rhi_));
    ps_resources->depth_sampler().BindResource(resources_.depth_texture->GetSampler());
}

void DirectionalLightingPass::SetDirectionalShadow(const RHIResourceRef<RHIImage> &shadow_map)
{
    if (resources_.shadow_map == shadow_map)
    {
        return;
    }

    resources_.shadow_map = shadow_map;

    BindPixelShaderResources();
}

void DirectionalLightingPass::SetIBL(ImageBasedLighting *ibl)
{
    if (resources_.ibl == ibl)
    {
        return;
    }

    resources_.ibl = ibl;

    if (ibl->NeedUpdate())
    {
        ibl_changed_subscription_ = ibl->OnRenderResourceChange().Subscribe([this]() { ibl_dirty_ = true; });
    }
    else
    {
        BindPixelShaderResources();
    }
}

void DirectionalLightingPass::SetSkyLight(SkyRenderProxy *sky_light)
{
    if (resources_.sky_light == sky_light)
    {
        return;
    }

    resources_.sky_light = sky_light;
}

void DirectionalLightingPass::Render()
{
    rhi_->BeginRenderPass(pass_);

    rhi_->DrawMesh(pipeline_state_, draw_args_);

    rhi_->EndRenderPass();
}

void DirectionalLightingPass::SetupRenderPass()
{
    RHIRenderPass::Attribute pass_attribute;

    pass_ = rhi_->CreateRenderPass(pass_attribute, target_, "DirectionalLightingPass");
}
} // namespace sparkle
