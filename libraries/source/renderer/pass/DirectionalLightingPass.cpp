#include "renderer/pass/DirectionalLightingPass.h"

#include "renderer/RenderConfig.h"
#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/GBufferPass.h"
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
    view_buffer_ = camera->GetViewBuffer();
    BindView(*pipeline_state_);
    if (pixel_local_pipeline_)
    {
        BindView(*pixel_local_pipeline_);
    }

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

void DirectionalLightingPass::BindView(RHIPipelineState &pipeline) const
{
    pipeline.GetShaderResource<DirectionalLightingPassPixelShader>()->view().BindResource(view_buffer_);
}

const RHIResourceRef<RHIPipelineState> &DirectionalLightingPass::GetPipeline(const RGRasterContext &context,
                                                                             RGTexture gbuffer) const
{
    if (!context.IsPixelLocal(gbuffer))
    {
        return pipeline_state_;
    }

    if (!pixel_local_pipeline_)
    {
        auto signature = GetSignature();
        signature.color_formats[ColorSlot::GBufferPacked] = GBufferPass::PackedDesc.format;
        signature.color_formats[ColorSlot::DepthCopy] = GBufferPass::DepthCopyDesc.format;
        pixel_local_pipeline_ = CreatePipeline(signature);
        pixel_local_pipeline_->SetShader<RHIShaderStage::Pixel>(
            rhi_->CreateShader<DirectionalLightingPassPixelShader>("PIXEL_LOCAL"));
        CompilePipeline(*pixel_local_pipeline_);
        pixel_local_pipeline_->GetShaderResource<DirectionalLightingPassPixelShader>()->ubo().BindResource(ps_ub_);
        BindView(*pixel_local_pipeline_);
    }
    return pixel_local_pipeline_;
}

void DirectionalLightingPass::AddTo(RenderGraph &graph, const LightingInputs &lighting, RGTexture gbuffer,
                                    RGTexture depth_copy, RGTexture scene_color) const
{
    graph.AddRasterPass(name_, [this, lighting, gbuffer, depth_copy, scene_color](RGBuilder &builder) {
        using Table = DirectionalLightingPassPixelShader::ResourceTable;
        // GBuffer writes both, so either both stay pixel-local or neither does
        builder.PixelLocalRead(gbuffer, ColorSlot::GBufferPacked, &Table::gbuffer_texture);
        builder.PixelLocalRead(depth_copy, ColorSlot::DepthCopy, &Table::depth_texture);
        lighting.Sample<Table>(builder, rhi_);
        builder.ColorWrite(scene_color, ColorSlot::SceneColor);
        builder.FullyOverwrites();
        return
            [this, gbuffer](RGRasterContext &context) { context.DrawMesh(GetPipeline(context, gbuffer), draw_args_); };
    });
}
} // namespace sparkle
