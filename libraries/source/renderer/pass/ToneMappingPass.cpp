#include "renderer/pass/ToneMappingPass.h"

#include "renderer/graph/RenderGraph.h"
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

void ToneMappingPass::SampleInput(RGBuilder &builder, RGTexture input) const
{
    using Table = ToneMappingPixelShader::ResourceTable;
    builder.Sampled(input, &Table::screenTexture, &Table::screenTextureSampler);
}
} // namespace sparkle
