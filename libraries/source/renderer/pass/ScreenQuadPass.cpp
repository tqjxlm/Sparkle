#include "renderer/pass/ScreenQuadPass.h"

#include "application/NativeView.h"
#include "core/math/Utilities.h"
#include "renderer/graph/RenderGraph.h"
#include "renderer/pass/ColorSlot.h"
#include "rhi/RHI.h"

namespace sparkle
{
class ScreenQuadPixelShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ScreenQuadPixelShader, RHIShaderStage::Pixel, "shaders/screen/screen.ps.slang", "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(screenTexture, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(screenTextureSampler, RHIShaderResourceReflection::ResourceType::Sampler)

    END_SHADER_RESOURCE_TABLE
};

const std::array<ScreenQuadPass::ScreenVertex, 4> ScreenQuadPass::Vertices{{
    {.position = Vector3{-1, -1, 0}, .uv = Vector2{0, 0}},
    {.position = Vector3{1, -1, 0}, .uv = Vector2{1, 0}},
    {.position = Vector3{1, 1, 0}, .uv = Vector2{1, 1}},
    {.position = Vector3{-1, 1, 0}, .uv = Vector2{0, 1}},
}};

const std::array<uint32_t, 6> ScreenQuadPass::Indices{0, 2, 1, 0, 3, 2};

static constexpr RHISampler::SamplerAttribute BilinearSampler{
    .address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
    .filtering_method_min = RHISampler::FilteringMethod::Linear,
    .filtering_method_mag = RHISampler::FilteringMethod::Linear,
    .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest,
    .enable_anisotropy = false};

ScreenQuadPass::ScreenQuadPass(RHIContext *ctx, std::string name, PixelFormat output_format, InputFilter input_filter,
                               bool to_back_buffer)
    : ScreenQuadPass(ctx, std::move(name), output_format, ColorSlot::Screen, input_filter, to_back_buffer)
{
}

ScreenQuadPass::ScreenQuadPass(RHIContext *ctx, std::string name, PixelFormat output_format, uint8_t output_slot,
                               InputFilter input_filter, bool to_back_buffer)
    : PipelinePass(ctx), name_(std::move(name)), output_slot_(output_slot), input_filter_(input_filter),
      to_back_buffer_(to_back_buffer)
{
    signature_.color_formats[output_slot_] = output_format;
}

void ScreenQuadPass::InitRenderResources(const RenderConfig &)
{
    SetupVertices();
    SetupVertexShader();

    pipeline_state_ = CreatePipeline(signature_);
    SetupPixelShader();
    CompilePipeline(*pipeline_state_);

    BindPixelShaderResources();

    draw_args_.index_count = 6;
}

RHIResourceRef<RHIPipelineState> ScreenQuadPass::CreatePipeline(const RHIAttachmentSignature &signature) const
{
    auto pipeline = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Graphics, "ScreenQuadPipeline");
    pipeline->SetAttachmentSignature(signature);

    RHIPipelineState::DepthState depth_state;
    depth_state.write_depth = false;
    depth_state.test_state = RHIPipelineState::DepthTestState::Always;
    pipeline->SetDepthState(depth_state);

    pipeline->SetIndexBuffer(index_buffer_);
    pipeline->SetVertexBuffer(0, vertex_buffer_);

    auto &vertex_delcaration = pipeline->GetVertexInputDeclaration();
    vertex_delcaration.SetAttribute(0, 0, {RHIVertexFormat::R32G32B32Float, offsetof(ScreenVertex, position)});
    vertex_delcaration.SetAttribute(1, 0, {RHIVertexFormat::R32G32Float, offsetof(ScreenVertex, uv)});

    pipeline->SetShader<RHIShaderStage::Vertex>(vertex_shader_);
    return pipeline;
}

void ScreenQuadPass::CompilePipeline(RHIPipelineState &pipeline) const
{
    pipeline.Compile();
    pipeline.GetShaderResource<ScreenQuadVertexShader>()->ubo().BindResource(vs_ub_);
}

const RHIResourceRef<RHIPipelineState> &ScreenQuadPass::GetPipeline(const RGRasterContext & /*context*/,
                                                                    RGTexture /*input*/) const
{
    return pipeline_state_;
}

void ScreenQuadPass::SetupVertices()
{
    if (!vertex_buffer_)
    {
        vertex_buffer_ =
            rhi_->CreateBuffer({.size = ARRAY_SIZE(Vertices),
                                .usages = RHIBuffer::BufferUsage::VertexBuffer,
                                .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                .is_dynamic = false},
                               "ScreenVertexBuffer");
        vertex_buffer_->UploadImmediate(Vertices.data());
    }

    if (!index_buffer_)
    {
        index_buffer_ =
            rhi_->CreateBuffer({.size = ARRAY_SIZE(Indices),
                                .usages = RHIBuffer::BufferUsage::IndexBuffer,
                                .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                .is_dynamic = false},
                               "ScreenIndexBuffer");
        index_buffer_->UploadImmediate(Indices.data());
    }
}

void ScreenQuadPass::SetupVertexShader()
{
    vertex_shader_ = rhi_->CreateShader<ScreenQuadVertexShader>();

    if (!vs_ub_)
    {
        vs_ub_ = rhi_->CreateBuffer({.size = sizeof(ScreenQuadVertexShader::UniformBufferData),
                                     .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                     .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                     .is_dynamic = false},
                                    "ScreenVSUBO");

        ScreenQuadVertexShader::UniformBufferData ubo;
        ubo.pre_rotation.setIdentity();

        if (to_back_buffer_)
        {
            ubo.pre_rotation.topLeftCorner(2, 2) =
                NativeView::GetRotationMatrix(rhi_->GetHardwareInterface()->GetWindowOrientation());
        }

        vs_ub_->UploadImmediate(&ubo);
    }
}

void ScreenQuadPass::SetupPixelShader()
{
    pixel_shader_ = rhi_->CreateShader<ScreenQuadPixelShader>();
    pipeline_state_->SetShader<RHIShaderStage::Pixel>(pixel_shader_);
}

// a pre-rotation by a quarter turn lays the input's x axis along the output's y axis
static bool IsQuarterTurn(NativeView::WindowRotation rotation)
{
    return rotation == NativeView::WindowRotation::Landscape ||
           rotation == NativeView::WindowRotation::ReverseLandscape;
}

static bool Resamples(ScreenQuadPass::InputFilter filter, const Vector2UInt &input_size, const Vector2UInt &output_size)
{
    switch (filter)
    {
    case ScreenQuadPass::InputFilter::Nearest:
        return false;
    case ScreenQuadPass::InputFilter::Bilinear:
        return input_size != output_size;
    case ScreenQuadPass::InputFilter::NearestAtIntegerScale:
        return output_size.x() % input_size.x() != 0 || output_size.y() % input_size.y() != 0;
    default:
        UnImplemented(filter);
        return false;
    }
}

void ScreenQuadPass::AddTo(RenderGraph &graph, RGTexture input, RGTexture output) const
{
    auto output_size = graph.GetSize(output);
    if (to_back_buffer_ && IsQuarterTurn(rhi_->GetHardwareInterface()->GetWindowOrientation()))
    {
        output_size = Vector2UInt(output_size.y(), output_size.x());
    }
    const bool bilinear = Resamples(input_filter_, graph.GetSize(input), output_size) &&
                          rhi_->SupportsLinearFiltering(graph.GetFormat(input));
    graph.AddRasterPass(name_, [this, input, output, bilinear](RGBuilder &builder) {
        SampleInput(builder, input, bilinear ? BilinearSampler : NearestSampler);
        builder.ColorWrite(output, output_slot_);
        builder.FullyOverwrites();
        return [this, input](RGRasterContext &context) { context.DrawMesh(GetPipeline(context, input), draw_args_); };
    });
}

void ScreenQuadPass::SampleInput(RGBuilder &builder, RGTexture input, const RHISampler::SamplerAttribute &sampler) const
{
    using Table = ScreenQuadPixelShader::ResourceTable;
    builder.Sampled(input, &Table::screenTexture, &Table::screenTextureSampler, sampler);
}

void ScreenQuadPass::UpdateFrameData(const RenderConfig &config, SceneRenderProxy *scene)
{
    PipelinePass::UpdateFrameData(config, scene);

    // in case of swapchain being recreated, we may have to reset the back buffer
    if (to_back_buffer_ && rhi_->IsBackBufferDirty())
    {
        ScreenQuadVertexShader::UniformBufferData ubo;
        ubo.pre_rotation.topLeftCorner(2, 2) =
            NativeView::GetRotationMatrix(rhi_->GetHardwareInterface()->GetWindowOrientation());

        vs_ub_->UploadImmediate(&ubo);
    }
}
} // namespace sparkle
