#include "renderer/pass/DirectionalLightingPass.h"

#include "renderer/RenderConfig.h"
#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/LightingInputs.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/DirectionalLightRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "renderer/resource/PbrResource.h"
#include "rhi/RHI.h"

namespace sparkle
{
class DirectionalLightingPassPixelShader : public RHIShaderInfo
{
    REGISTGER_SHADER(DirectionalLightingPassPixelShader, RHIShaderStage::Pixel,
                     "shaders/screen/directional_lighting.ps.slang", "shader_main")

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
    };
};

void DirectionalLightingPass::UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene)
{
    ScreenQuadPass::UpdateFrameData(config, scene);

    auto *sky_light = scene->GetSkyLight();
    auto *camera = scene->GetCamera();
    auto *dir_light = scene->GetDirectionalLight();

    const bool has_ibl = sky_light != nullptr && sky_light->GetImageBasedLighting() != nullptr;
    const bool use_diffuse_ibl = has_ibl && config.use_diffuse_ibl;
    const bool use_specular_ibl = has_ibl && config.use_specular_ibl;

    // scene proxy recreation (e.g. on scene load) replaces the camera and its view buffer
    auto *ps_resources = pipeline_state_->GetShaderResource<DirectionalLightingPassPixelShader>();
    ps_resources->view().BindResource(camera->GetViewBuffer());

    const PbrConfig pbr_config{.mode = static_cast<uint32_t>(config.debug_mode),
                               .use_ibl_diffuse = static_cast<uint32_t>(use_diffuse_ibl ? 1 : 0),
                               .use_ibl_specular = static_cast<uint32_t>(use_specular_ibl ? 1 : 0)};

    DirectionalLightingPassPixelShader::UniformBufferData ubo{
        .sky_light = sky_light ? sky_light->GetRenderData() : SkyRenderProxy::UniformBufferData{},
        .dir_light = dir_light ? dir_light->GetRenderData() : DirectionalLightRenderProxy::UniformBufferData{},
        .view_pos = camera->GetPosture().position,
        .render_config = pbr_config};

    ps_ub_->Upload(rhi_, &ubo);
}

void DirectionalLightingPass::SetupPixelShader()
{
    pixel_shader_ = rhi_->CreateShader<DirectionalLightingPassPixelShader>();
    pipeline_state_->SetShader<RHIShaderStage::Pixel>(pixel_shader_);
}

void DirectionalLightingPass::BindPixelShaderResources()
{
    ps_ub_ = rhi_->CreateBuffer({.size = sizeof(DirectionalLightingPassPixelShader::UniformBufferData),
                                 .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                 .mem_properties = RHIMemoryProperty::None,
                                 .is_dynamic = true},
                                "DirectionalLightingPassUniformBuffer");

    pipeline_state_->GetShaderResource<DirectionalLightingPassPixelShader>()->ubo().BindResource(ps_ub_);
}

void DirectionalLightingPass::AddTo(RenderGraph &graph, const LightingInputs &lighting, RGTexture gbuffer,
                                    RGTexture scene_depth, RGTexture scene_color) const
{
    graph.AddRasterPass(name_, [this, lighting, gbuffer, scene_depth, scene_color](RGBuilder &builder) {
        using Table = DirectionalLightingPassPixelShader::ResourceTable;
        builder.Sampled(gbuffer, &Table::gbuffer_texture);
        builder.Sampled(scene_depth, &Table::depth_texture, &Table::depth_sampler);
        lighting.Sample<Table>(builder, rhi_);
        builder.ColorWrite(scene_color, 0);
        builder.FullyOverwrites();
        return [this](RGRasterContext &context) { context.DrawMesh(pipeline_state_, draw_args_); };
    });
}
} // namespace sparkle
