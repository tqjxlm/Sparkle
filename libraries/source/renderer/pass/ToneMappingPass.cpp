#include "renderer/pass/ToneMappingPass.h"

#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/ColorSlot.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "rhi/RHI.h"

namespace sparkle
{
class ToneMappingPixelShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ToneMappingPixelShader, RHIShaderStage::Pixel, "shaders/screen/tone_mapping.ps.slang",
                     "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(screenTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(screenTextureSampler, RHIShaderResourceReflection::ResourceType::Sampler)

    END_SHADER_RESOURCE_TABLE

    struct UniformBufferData
    {
        float exposure;
    };
};

void ToneMappingPass::SetupPixelShader()
{
    pixel_shader_ = rhi_->CreateShader<ToneMappingPixelShader>();
    pipeline_state_->SetShader<RHIShaderStage::Pixel>(pixel_shader_);
}

void ToneMappingPass::UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene)
{
    ScreenQuadPass::UpdateFrameData(config, scene);

    ToneMappingPixelShader::UniformBufferData ubo;
    ubo.exposure = scene->GetCamera()->GetAttribute().exposure;
    ps_ub_->Upload(rhi_, &ubo);
}

void ToneMappingPass::BindPixelShaderResources()
{
    auto *ps_resources = pipeline_state_->GetShaderResource<ToneMappingPixelShader>();

    ps_ub_ = rhi_->CreateBuffer({.size = sizeof(ToneMappingPixelShader::UniformBufferData),
                                 .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                 .mem_properties = RHIMemoryProperty::None,
                                 .is_dynamic = true},
                                "ToneMappingUBO");

    ps_resources->ubo().BindResource(ps_ub_);
}

void ToneMappingPass::SampleInput(RGBuilder &builder, RGTexture input,
                                  const RHISampler::SamplerAttribute &sampler) const
{
    using Table = ToneMappingPixelShader::ResourceTable;
    builder.PixelLocalRead(input, ColorSlot::SceneColor, &Table::screenTexture, &Table::screenTextureSampler, sampler);
}

const RHIResourceRef<RHIPipelineState> &ToneMappingPass::GetPipeline(const RGRasterContext &context,
                                                                     RGTexture input) const
{
    if (!context.IsPixelLocal(input))
    {
        return pipeline_state_;
    }

    if (!pixel_local_pipeline_)
    {
        auto signature = GetSignature();
        signature.color_formats[ColorSlot::SceneColor] = context.GetImage(input)->GetAttributes().format;
        pixel_local_pipeline_ = CreatePipeline(signature);
        pixel_local_pipeline_->SetShader<RHIShaderStage::Pixel>(
            rhi_->CreateShader<ToneMappingPixelShader>("PIXEL_LOCAL"));
        CompilePipeline(*pixel_local_pipeline_);
        pixel_local_pipeline_->GetShaderResource<ToneMappingPixelShader>()->ubo().BindResource(ps_ub_);
    }
    return pixel_local_pipeline_;
}
} // namespace sparkle
