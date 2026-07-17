#include "renderer/pass/ToneMappingPass.h"

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

// when the pass upsamples (sub-resolution rendering makes the source smaller than the target), sampling
// needs bilinear footprints and edge clamping. 32-bit float sources keep their own sampler instead:
// linear-filtering them is an optional device feature (and nearest never reaches the wrapped edge).
RHIResourceRef<RHISampler> ToneMappingPass::GetInputSampler() const
{
    const auto &source = source_texture_->GetAttributes();
    const auto &target = target_->GetAttribute();

    if ((source.width == target.width && source.height == target.height) || source.format == PixelFormat::RGBAFloat)
    {
        return source_texture_->GetSampler();
    }

    auto sampler_attribute = source.sampler;
    sampler_attribute.address_mode = RHISampler::SamplerAddressMode::ClampToEdge;
    sampler_attribute.filtering_method_min = RHISampler::FilteringMethod::Linear;
    sampler_attribute.filtering_method_mag = RHISampler::FilteringMethod::Linear;
    return rhi_->GetSampler(sampler_attribute);
}

void ToneMappingPass::BindPixelShaderResources()
{
    auto *ps_resources = pipeline_state_->GetShaderResource<ToneMappingPixelShader>();
    ps_resources->screenTexture().BindResource(source_texture_->GetDefaultView(rhi_));
    ps_resources->screenTextureSampler().BindResource(GetInputSampler());

    ps_ub_ = rhi_->CreateBuffer({.size = sizeof(ToneMappingPixelShader::UniformBufferData),
                                 .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                 .mem_properties = RHIMemoryProperty::None,
                                 .is_dynamic = true},
                                "ToneMappingUBO");

    ps_resources->ubo().BindResource(ps_ub_);
}
} // namespace sparkle
