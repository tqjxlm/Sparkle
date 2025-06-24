#include "renderer/pass/BlurPass.h"

#include "renderer/pass/ScreenQuadPass.h"
#include "rhi/RHI.h"

namespace sparkle
{
class BlurPixelShader : public RHIShaderInfo
{
    REGISTGER_SHADER(BlurPixelShader, RHIShaderStage::Pixel, "shaders/screen/blur.ps.hlsl", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::UniformBuffer)
    USE_SHADER_RESOURCE(inputTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inputTextureSampler, RHIShaderResourceReflection::ResourceType::Sampler)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2 pixel_size;
    };
};

BlurPass::BlurPass(RHIContext *ctx, const RHIResourceRef<RHIImage> &input, int count)
    : ScreenQuadPass(ctx, nullptr, nullptr), input_(input), count_(count)
{
    ASSERT(count_ > 0);

    InitImageAndTarget(PingPongIndex::PING);
    InitImageAndTarget(PingPongIndex::PONG);

    images_[PingPongIndex::INPUT] = input;
    // input will not be written to, so it has no corresponding target
}

void BlurPass::InitImageAndTarget(PingPongIndex index)
{
    RHIImage::Attribute image_attribute;
    image_attribute.format = input_->GetAttributes().format;
    image_attribute.width = input_->GetAttributes().width;
    image_attribute.height = input_->GetAttributes().height;
    image_attribute.usages =
        RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::ColorAttachment | RHIImage::ImageUsage::TransferSrc;
    image_attribute.sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                               .filtering_method_min = RHISampler::FilteringMethod::Linear,
                               .filtering_method_mag = RHISampler::FilteringMethod::Linear,
                               .filtering_method_mipmap = RHISampler::FilteringMethod::Linear};

    images_[index] = rhi_->CreateImage(image_attribute, "PingPongImage" + std::to_string(index));

    targets_[index] = rhi_->CreateRenderTarget({}, images_[index], nullptr, "PingPongTarget" + std::to_string(index));

    images_[index]->Transition({.target_layout = RHIImageLayout::Read,
                                .after_stage = RHIPipelineStage::Top,
                                .before_stage = RHIPipelineStage::PixelShader});
}

void BlurPass::SetupPipeline()
{
    for (auto i = 0u; i < PingPongIndex::COUNT; i++)
    {
        pass_ = passes_[i];
        ScreenQuadPass::SetupPipeline();
        pipeline_states_[i] = pipeline_state_;
    }

    pass_ = nullptr;
    pipeline_state_ = nullptr;
}

void BlurPass::CompilePipeline()
{
    for (auto i = 0u; i < PingPongIndex::COUNT; i++)
    {
        pipeline_states_[i]->Compile();
    }
}

void BlurPass::SetupRenderPass()
{
    for (auto i = 0u; i < PingPongIndex::COUNT; i++)
    {
        // ping pass: write to pong
        // pong pass: write to ping

        RHIRenderPass::Attribute pass_attribute;
        pass_attribute.color_final_layout = RHIImageLayout::Read;
        passes_[i] = rhi_->CreateRenderPass(pass_attribute, targets_[GetOutputIndex(static_cast<PingPongIndex>(i))],
                                            std::format("BlurPass{}", i));
    }
}

void BlurPass::SetupVertices()
{
    for (auto i = 0u; i < PingPongIndex::COUNT; i++)
    {
        pipeline_state_ = pipeline_states_[i];
        ScreenQuadPass::SetupVertices();
    }
    pipeline_state_ = nullptr;
}

void BlurPass::SetupVertexShader()
{
    for (auto i = 0u; i < PingPongIndex::COUNT; i++)
    {
        pipeline_state_ = pipeline_states_[i];
        ScreenQuadPass::SetupVertexShader();
    }
    pipeline_state_ = nullptr;
}

void BlurPass::SetupPixelShader()
{
    pixel_shader_ = rhi_->CreateShader<BlurPixelShader>();
    for (auto i = 0u; i < PingPongIndex::COUNT; i++)
    {
        pipeline_states_[i]->SetShader<RHIShaderStage::Pixel>(pixel_shader_);
    }
}

void BlurPass::Render()
{
    RenderPass(PingPongIndex::INPUT);

    for (int i = 1; i < count_; i++)
    {
        if (i % 2 == 0)
        {
            RenderPass(PingPongIndex::PING);
        }
        else
        {
            RenderPass(PingPongIndex::PONG);
        }
    }
}

void BlurPass::RenderPass(PingPongIndex index)
{
    auto output_index = GetOutputIndex(index);

    // the first transition happens on INPUT image. it may be a null op if INPUT image is already in Read layout.
    images_[index]->Transition({.target_layout = RHIImageLayout::Read,
                                .after_stage = RHIPipelineStage::ColorOutput,
                                .before_stage = RHIPipelineStage::PixelShader});
    images_[output_index]->Transition({.target_layout = RHIImageLayout::ColorOutput,
                                       .after_stage = RHIPipelineStage::Top,
                                       .before_stage = RHIPipelineStage::ColorOutput});

    rhi_->BeginRenderPass(passes_[index]);
    rhi_->DrawMesh(pipeline_states_[index], draw_args_);
    rhi_->EndRenderPass();
}

void BlurPass::BindVertexShaderResources()
{
    for (auto i = 0u; i < PingPongIndex::COUNT; i++)
    {
        pipeline_state_ = pipeline_states_[i];
        ScreenQuadPass::BindVertexShaderResources();
    }
    pipeline_state_ = nullptr;
}

void BlurPass::BindPixelShaderResources()
{
    ps_ub_ = rhi_->CreateBuffer({.size = sizeof(BlurPixelShader::UniformBufferData),
                                 .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                 .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                 .is_dynamic = false},
                                "BlurPSUBO");

    BlurPixelShader::UniformBufferData ubo;
    ubo.pixel_size = Vector2(1.f / static_cast<Scalar>(targets_[0]->GetAttribute().width),
                             1.f / static_cast<Scalar>(targets_[0]->GetAttribute().height));
    ps_ub_->UploadImmediate(&ubo);

    for (auto i = 0u; i < PingPongIndex::COUNT; i++)
    {
        // ping pass: read from ping
        // pong pass: read from pong
        // launch pass: read from input

        auto *ps_resource = pipeline_states_[i]->GetShaderResource<BlurPixelShader>();

        ps_resource->inputTexture().BindResource(images_[i]->GetDefaultView(rhi_));
        ASSERT(images_[i]->GetSampler());
        ps_resource->inputTextureSampler().BindResource(images_[i]->GetSampler());
        ps_resource->ubo().BindResource(ps_ub_);
    }
}
} // namespace sparkle
