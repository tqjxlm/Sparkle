#include "renderer/pass/ToneMappingPass.h"

#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "rhi/RHI.h"

namespace sparkle
{
class ToneMappingPixelShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ToneMappingPixelShader, RHIShaderStage::Pixel, "shaders/screen/tone_mapping.ps.slang", "main")

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

void ToneMappingPass::SetupRenderPass()
{
    RHIRenderPass::Attribute pass_attribute;
    pass_ = rhi_->CreateRenderPass(pass_attribute, target_, "ToneMappingPass");
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
    ps_resources->screenTexture().BindResource(source_texture_->GetDefaultView(rhi_));
    ps_resources->screenTextureSampler().BindResource(source_texture_->GetSampler());

    ps_ub_ = rhi_->CreateBuffer({.size = sizeof(ToneMappingPixelShader::UniformBufferData),
                                 .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                 .mem_properties = RHIMemoryProperty::None,
                                 .is_dynamic = true},
                                "ToneMappingUBO");

    ps_resources->ubo().BindResource(ps_ub_);
}
} // namespace sparkle
