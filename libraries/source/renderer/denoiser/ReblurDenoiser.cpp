#include "renderer/denoiser/ReblurDenoiser.h"

#include "rhi/RHI.h"

namespace sparkle
{
namespace
{
class ReblurPassthroughShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurPassthroughShader, RHIShaderStage::Compute, "shaders/utilities/reblur_passthrough.cs.slang",
                     "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(noisy_input, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(denoised_output, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
    };
};
} // namespace

ReblurDenoiser::ReblurDenoiser(RHIContext *rhi_context) : rhi_(rhi_context)
{
    ASSERT(rhi_);
}

void ReblurDenoiser::Initialize(const Vector2UInt &image_size)
{
    ASSERT(image_size.x() > 0 && image_size.y() > 0);

    image_size_ = image_size;

    passthrough_shader_ = rhi_->CreateShader<ReblurPassthroughShader>();

    pipeline_state_ = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurPipeline");
    pipeline_state_->SetShader<RHIShaderStage::Compute>(passthrough_shader_);
    pipeline_state_->Compile();

    uniform_buffer_ = rhi_->CreateBuffer({.size = sizeof(ReblurPassthroughShader::UniformBufferData),
                                          .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                          .mem_properties = RHIMemoryProperty::None,
                                          .is_dynamic = true},
                                         "ReblurUniformBuffer");

    auto *cs_resources = pipeline_state_->GetShaderResource<ReblurPassthroughShader>();
    cs_resources->ubo().BindResource(uniform_buffer_);

    compute_pass_ = rhi_->CreateComputePass("ReblurComputePass", false);
}

void ReblurDenoiser::Resize(const Vector2UInt &image_size)
{
    ASSERT(image_size.x() > 0 && image_size.y() > 0);
    image_size_ = image_size;
}

void ReblurDenoiser::Dispatch(const FrontEndInputs &inputs, const RHIResourceRef<RHIImage> &denoised_output)
{
    ASSERT(inputs.noisy_input);
    ASSERT(inputs.normal_roughness);
    ASSERT(inputs.view_z);
    ASSERT(inputs.motion_vectors);
    ASSERT(inputs.diff_radiance_hitdist);
    ASSERT(inputs.spec_radiance_hitdist);
    ASSERT(denoised_output);
    ASSERT(pipeline_state_);
    ASSERT(compute_pass_);
    ASSERT(uniform_buffer_);

    Vector2UInt output_size(denoised_output->GetWidth(), denoised_output->GetHeight());
    if (output_size.x() != image_size_.x() || output_size.y() != image_size_.y())
    {
        Resize(output_size);
    }

    ReblurPassthroughShader::UniformBufferData ubo{.resolution = image_size_};
    uniform_buffer_->Upload(rhi_, &ubo);

    auto *cs_resources = pipeline_state_->GetShaderResource<ReblurPassthroughShader>();
    cs_resources->noisy_input().BindResource(inputs.noisy_input->GetDefaultView(rhi_));
    cs_resources->denoised_output().BindResource(denoised_output->GetDefaultView(rhi_));

    denoised_output->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                 .after_stage = RHIPipelineStage::Top,
                                 .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(compute_pass_);
    rhi_->DispatchCompute(pipeline_state_, {image_size_.x(), image_size_.y(), 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(compute_pass_);

    denoised_output->Transition({.target_layout = RHIImageLayout::Read,
                                 .after_stage = RHIPipelineStage::ComputeShader,
                                 .before_stage = RHIPipelineStage::PixelShader});
}
} // namespace sparkle
