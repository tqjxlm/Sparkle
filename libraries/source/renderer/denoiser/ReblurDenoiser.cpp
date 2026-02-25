#include "renderer/denoiser/ReblurDenoiser.h"

#include "core/Logger.h"
#include "rhi/RHI.h"

#include <cmath>

namespace sparkle
{

class ReblurClassifyTilesShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurClassifyTilesShader, RHIShaderStage::Compute,
                     "shaders/ray_trace/reblur_classify_tiles.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(inViewZ, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(outTiles, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        float denoising_range;
    };
};

class ReblurBlurShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurBlurShader, RHIShaderStage::Compute, "shaders/ray_trace/reblur_blur.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(inDiffuse, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inSpecular, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inNormalRoughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inViewZ, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(outDiffuse, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outSpecular, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(inInternalData, RHIShaderResourceReflection::ResourceType::Texture2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        float max_blur_radius;
        float min_blur_radius;
        float lobe_angle_fraction;
        float plane_dist_sensitivity;
        float min_hit_dist_weight;
        float denoising_range;
        Vector4 rotator; // cos, sin, -sin, cos
        uint32_t frame_index;
        uint32_t blur_pass_index;
        float diffuse_prepass_blur_radius;
        float specular_prepass_blur_radius;
        uint32_t has_temporal_data;
    };
};

class ReblurTemporalAccumShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurTemporalAccumShader, RHIShaderStage::Compute,
                     "shaders/ray_trace/reblur_temporal_accumulation.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(inDiffuse, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inSpecular, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inNormalRoughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inViewZ, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevDiffHistory, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevSpecHistory, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevViewZ, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevNormalRoughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevInternalData, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(outDiffuse, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outSpecular, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outInternalData, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        float max_accumulated_frame_num;
        float max_fast_accumulated_frame_num;
        float disocclusion_threshold;
        float denoising_range;
        uint32_t frame_index;
        uint32_t reset_history;
    };
};

class ReblurHistoryFixShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurHistoryFixShader, RHIShaderStage::Compute,
                     "shaders/ray_trace/reblur_history_fix.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(inDiffuse, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inSpecular, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inNormalRoughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inViewZ, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inInternalData, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(outDiffuse, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outSpecular, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        float history_fix_frame_num;
        float history_fix_stride;
        float plane_dist_sensitivity;
        float lobe_angle_fraction;
        float denoising_range;
    };
};

ReblurDenoiser::ReblurDenoiser(RHIContext *rhi, uint32_t width, uint32_t height)
    : rhi_(rhi), width_(width), height_(height)
{
    Log(Info, "ReblurDenoiser: creating ({}x{})", width, height);
    CreateTextures();
    CreatePipelines();

    // Initialize history and previous-frame textures to Read layout.
    // On the first frame, these are read by TemporalAccumulate (with reset_history=1,
    // so content is ignored) and PrePass Blur. They need a valid layout for binding.
    auto init_to_read = [](RHIImage *image) {
        image->Transition({.target_layout = RHIImageLayout::Read,
                           .after_stage = RHIPipelineStage::Top,
                           .before_stage = RHIPipelineStage::ComputeShader});
    };
    init_to_read(diff_history_.get());
    init_to_read(spec_history_.get());
    init_to_read(internal_data_.get());
    init_to_read(prev_internal_data_.get());
    init_to_read(prev_view_z_.get());
    init_to_read(prev_normal_roughness_.get());

    Log(Info, "ReblurDenoiser: ready");
}

ReblurDenoiser::~ReblurDenoiser() = default;

void ReblurDenoiser::CreateTextures()
{
    auto make_image = [this](PixelFormat format, uint32_t w, uint32_t h, const std::string &name) {
        return rhi_->CreateImage(
            RHIImage::Attribute{
                .format = format,
                .sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
                            .filtering_method_min = RHISampler::FilteringMethod::Linear,
                            .filtering_method_mag = RHISampler::FilteringMethod::Linear,
                            .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
                .width = w,
                .height = h,
                .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV |
                          RHIImage::ImageUsage::TransferSrc | RHIImage::ImageUsage::TransferDst,
                .memory_properties = RHIMemoryProperty::DeviceLocal,
            },
            name);
    };

    denoised_diffuse_ = make_image(PixelFormat::RGBAFloat16, width_, height_, "ReblurDenoisedDiffuse");
    denoised_specular_ = make_image(PixelFormat::RGBAFloat16, width_, height_, "ReblurDenoisedSpecular");

    diff_temp1_ = make_image(PixelFormat::RGBAFloat16, width_, height_, "ReblurDiffTemp1");
    diff_temp2_ = make_image(PixelFormat::RGBAFloat16, width_, height_, "ReblurDiffTemp2");
    spec_temp1_ = make_image(PixelFormat::RGBAFloat16, width_, height_, "ReblurSpecTemp1");
    spec_temp2_ = make_image(PixelFormat::RGBAFloat16, width_, height_, "ReblurSpecTemp2");

    // History buffers (persistent across frames)
    diff_history_ = make_image(PixelFormat::RGBAFloat16, width_, height_, "ReblurDiffHistory");
    spec_history_ = make_image(PixelFormat::RGBAFloat16, width_, height_, "ReblurSpecHistory");

    // Internal data: packed accumSpeed (RG16Float)
    internal_data_ = make_image(PixelFormat::RG16Float, width_, height_, "ReblurInternalData");
    prev_internal_data_ = make_image(PixelFormat::RG16Float, width_, height_, "ReblurPrevInternalData");

    // Previous-frame buffers
    prev_view_z_ = make_image(PixelFormat::R32_FLOAT, width_, height_, "ReblurPrevViewZ");
    prev_normal_roughness_ = make_image(PixelFormat::RGBAFloat16, width_, height_, "ReblurPrevNormalRoughness");

    uint32_t tile_w = (width_ + 15) / 16;
    uint32_t tile_h = (height_ + 15) / 16;
    tiles_ = make_image(PixelFormat::R32_FLOAT, tile_w, tile_h, "ReblurTiles");
}

void ReblurDenoiser::CreatePipelines()
{
    compute_pass_ = rhi_->CreateComputePass("ReblurComputePass", false);

    // ClassifyTiles pipeline
    classify_tiles_shader_ = rhi_->CreateShader<ReblurClassifyTilesShader>();
    classify_tiles_pipeline_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurClassifyTilesPipeline");
    classify_tiles_pipeline_->SetShader<RHIShaderStage::Compute>(classify_tiles_shader_);
    classify_tiles_pipeline_->Compile();

    classify_tiles_ub_ = rhi_->CreateBuffer({.size = sizeof(ReblurClassifyTilesShader::UniformBufferData),
                                             .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                             .mem_properties = RHIMemoryProperty::None,
                                             .is_dynamic = true},
                                            "ReblurClassifyTilesUBO");

    auto *ct_resources = classify_tiles_pipeline_->GetShaderResource<ReblurClassifyTilesShader>();
    ct_resources->ubo().BindResource(classify_tiles_ub_);
    ct_resources->outTiles().BindResource(tiles_->GetDefaultView(rhi_));

    // Temporal accumulation pipeline
    temporal_accum_shader_ = rhi_->CreateShader<ReblurTemporalAccumShader>();
    temporal_accum_pipeline_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurTemporalAccumPipeline");
    temporal_accum_pipeline_->SetShader<RHIShaderStage::Compute>(temporal_accum_shader_);
    temporal_accum_pipeline_->Compile();

    temporal_accum_ub_ = rhi_->CreateBuffer({.size = sizeof(ReblurTemporalAccumShader::UniformBufferData),
                                             .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                             .mem_properties = RHIMemoryProperty::None,
                                             .is_dynamic = true},
                                            "ReblurTemporalAccumUBO");

    auto *ta_resources = temporal_accum_pipeline_->GetShaderResource<ReblurTemporalAccumShader>();
    ta_resources->ubo().BindResource(temporal_accum_ub_);

    // History fix pipeline
    history_fix_shader_ = rhi_->CreateShader<ReblurHistoryFixShader>();
    history_fix_pipeline_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurHistoryFixPipeline");
    history_fix_pipeline_->SetShader<RHIShaderStage::Compute>(history_fix_shader_);
    history_fix_pipeline_->Compile();

    history_fix_ub_ = rhi_->CreateBuffer({.size = sizeof(ReblurHistoryFixShader::UniformBufferData),
                                          .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                          .mem_properties = RHIMemoryProperty::None,
                                          .is_dynamic = true},
                                         "ReblurHistoryFixUBO");

    auto *hf_resources = history_fix_pipeline_->GetShaderResource<ReblurHistoryFixShader>();
    hf_resources->ubo().BindResource(history_fix_ub_);

    // Blur pipelines (separate per pass to avoid descriptor set conflicts within a frame)
    static constexpr const char *PassNames[] = {"PrePass", "Blur", "PostBlur"};
    blur_shader_ = rhi_->CreateShader<ReblurBlurShader>();
    for (uint32_t i = 0; i < BlurPassCount; i++)
    {
        blur_pipelines_[i] = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute,
                                                       std::string("ReblurBlur") + PassNames[i] + "Pipeline");
        blur_pipelines_[i]->SetShader<RHIShaderStage::Compute>(blur_shader_);
        blur_pipelines_[i]->Compile();

        blur_ubs_[i] = rhi_->CreateBuffer({.size = sizeof(ReblurBlurShader::UniformBufferData),
                                           .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                           .mem_properties = RHIMemoryProperty::None,
                                           .is_dynamic = true},
                                          std::string("ReblurBlur") + PassNames[i] + "UBO");

        auto *blur_resources = blur_pipelines_[i]->GetShaderResource<ReblurBlurShader>();
        blur_resources->ubo().BindResource(blur_ubs_[i]);
    }
}

void ReblurDenoiser::CopyToOutput(RHIImage *diff, RHIImage *spec)
{
    diff->Transition({.target_layout = RHIImageLayout::TransferSrc,
                      .after_stage = RHIPipelineStage::ComputeShader,
                      .before_stage = RHIPipelineStage::Transfer});
    denoised_diffuse_->Transition({.target_layout = RHIImageLayout::TransferDst,
                                   .after_stage = RHIPipelineStage::Top,
                                   .before_stage = RHIPipelineStage::Transfer});
    diff->CopyToImage(denoised_diffuse_.get());

    spec->Transition({.target_layout = RHIImageLayout::TransferSrc,
                      .after_stage = RHIPipelineStage::ComputeShader,
                      .before_stage = RHIPipelineStage::Transfer});
    denoised_specular_->Transition({.target_layout = RHIImageLayout::TransferDst,
                                    .after_stage = RHIPipelineStage::Top,
                                    .before_stage = RHIPipelineStage::Transfer});
    spec->CopyToImage(denoised_specular_.get());

    // Transition to Read so GPURenderer can composite without stage mismatch
    denoised_diffuse_->Transition({.target_layout = RHIImageLayout::Read,
                                   .after_stage = RHIPipelineStage::Transfer,
                                   .before_stage = RHIPipelineStage::ComputeShader});
    denoised_specular_->Transition({.target_layout = RHIImageLayout::Read,
                                    .after_stage = RHIPipelineStage::Transfer,
                                    .before_stage = RHIPipelineStage::ComputeShader});
}

void ReblurDenoiser::Denoise(const ReblurInputBuffers &inputs, const ReblurSettings &settings,
                             const ReblurMatrices & /*matrices*/, uint32_t /*frame_index*/, uint32_t debug_pass)
{
    ClassifyTiles(inputs, settings);

    // PrePass: input signals → temp1 (spatial pre-filter with larger radius)
    // Uses previous frame's accumSpeed for radius adaptation
    Blur(inputs, settings, 0, inputs.diffuse_radiance_hit_dist, inputs.specular_radiance_hit_dist, diff_temp1_.get(),
         spec_temp1_.get(), prev_internal_data_.get(), history_valid_);

    if (debug_pass == 0)
    {
        CopyToOutput(diff_temp1_.get(), spec_temp1_.get());
        internal_frame_index_++;
        return;
    }

    // Temporal Accumulation: temp1 + history → temp2, writes internal_data
    TemporalAccumulate(inputs, settings);

    // History Fix: temp2 → temp1 (wide-stride bilateral for disoccluded regions)
    HistoryFix(inputs, settings);

    // Blur: temp1 → temp2 (primary spatial filter, uses new accumSpeed)
    Blur(inputs, settings, 1, diff_temp1_.get(), spec_temp1_.get(), diff_temp2_.get(), spec_temp2_.get(),
         internal_data_.get(), true);

    if (debug_pass == 1)
    {
        CopyToOutput(diff_temp2_.get(), spec_temp2_.get());
        CopyPreviousFrameData(inputs);
        internal_frame_index_++;
        history_valid_ = true;
        return;
    }

    // PostBlur: temp2 → denoised output (final spatial refinement, uses new accumSpeed)
    Blur(inputs, settings, 2, diff_temp2_.get(), spec_temp2_.get(), denoised_diffuse_.get(), denoised_specular_.get(),
         internal_data_.get(), true);

    // Save history and frame data for next frame
    CopyHistoryData(denoised_diffuse_.get(), denoised_specular_.get());
    CopyPreviousFrameData(inputs);

    internal_frame_index_++;
    history_valid_ = true;
}

void ReblurDenoiser::ClassifyTiles(const ReblurInputBuffers &inputs, const ReblurSettings & /*settings*/)
{
    auto *resources = classify_tiles_pipeline_->GetShaderResource<ReblurClassifyTilesShader>();
    resources->inViewZ().BindResource(inputs.view_z->GetDefaultView(rhi_));

    ReblurClassifyTilesShader::UniformBufferData ubo{
        .resolution = {width_, height_},
        .denoising_range = 1000.f,
    };
    classify_tiles_ub_->Upload(rhi_, &ubo);

    tiles_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                        .after_stage = RHIPipelineStage::Top,
                        .before_stage = RHIPipelineStage::ComputeShader});

    uint32_t tile_w = (width_ + 15) / 16;
    uint32_t tile_h = (height_ + 15) / 16;

    rhi_->BeginComputePass(compute_pass_);
    rhi_->DispatchCompute(classify_tiles_pipeline_, {tile_w * 16, tile_h * 16, 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(compute_pass_);

    tiles_->Transition({.target_layout = RHIImageLayout::Read,
                        .after_stage = RHIPipelineStage::ComputeShader,
                        .before_stage = RHIPipelineStage::ComputeShader});
}

void ReblurDenoiser::Blur(const ReblurInputBuffers &inputs, const ReblurSettings &settings, uint32_t pass_index,
                          RHIImage *in_diff, RHIImage *in_spec, RHIImage *out_diff, RHIImage *out_spec,
                          RHIImage *internal_data, bool has_temporal_data)
{
    auto &pipeline = blur_pipelines_[pass_index];
    auto &ub = blur_ubs_[pass_index];

    // Bind inputs/outputs
    auto *resources = pipeline->GetShaderResource<ReblurBlurShader>();
    resources->inDiffuse().BindResource(in_diff->GetDefaultView(rhi_));
    resources->inSpecular().BindResource(in_spec->GetDefaultView(rhi_));
    resources->inNormalRoughness().BindResource(inputs.normal_roughness->GetDefaultView(rhi_));
    resources->inViewZ().BindResource(inputs.view_z->GetDefaultView(rhi_));
    resources->outDiffuse().BindResource(out_diff->GetDefaultView(rhi_));
    resources->outSpecular().BindResource(out_spec->GetDefaultView(rhi_));
    resources->inInternalData().BindResource(internal_data->GetDefaultView(rhi_));

    // Per-frame rotator based on frame index (golden angle rotation)
    float angle = static_cast<float>(internal_frame_index_) * 2.399963f; // golden angle
    float cos_a = std::cos(angle);
    float sin_a = std::sin(angle);

    ReblurBlurShader::UniformBufferData ubo{
        .resolution = {width_, height_},
        .max_blur_radius = settings.max_blur_radius,
        .min_blur_radius = settings.min_blur_radius,
        .lobe_angle_fraction = settings.lobe_angle_fraction,
        .plane_dist_sensitivity = settings.plane_dist_sensitivity,
        .min_hit_dist_weight = settings.min_hit_dist_weight,
        .denoising_range = 1000.f,
        .rotator = {cos_a, sin_a, -sin_a, cos_a},
        .frame_index = internal_frame_index_,
        .blur_pass_index = pass_index,
        .diffuse_prepass_blur_radius = settings.diffuse_prepass_blur_radius,
        .specular_prepass_blur_radius = settings.specular_prepass_blur_radius,
        .has_temporal_data = has_temporal_data ? 1u : 0u,
    };
    ub->Upload(rhi_, &ubo);

    // Transition inputs to Read, outputs to StorageWrite
    in_diff->Transition({.target_layout = RHIImageLayout::Read,
                         .after_stage = RHIPipelineStage::ComputeShader,
                         .before_stage = RHIPipelineStage::ComputeShader});
    in_spec->Transition({.target_layout = RHIImageLayout::Read,
                         .after_stage = RHIPipelineStage::ComputeShader,
                         .before_stage = RHIPipelineStage::ComputeShader});
    internal_data->Transition({.target_layout = RHIImageLayout::Read,
                               .after_stage = RHIPipelineStage::ComputeShader,
                               .before_stage = RHIPipelineStage::ComputeShader});
    out_diff->Transition({.target_layout = RHIImageLayout::StorageWrite,
                          .after_stage = RHIPipelineStage::Top,
                          .before_stage = RHIPipelineStage::ComputeShader});
    out_spec->Transition({.target_layout = RHIImageLayout::StorageWrite,
                          .after_stage = RHIPipelineStage::Top,
                          .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(compute_pass_);
    rhi_->DispatchCompute(pipeline, {width_, height_, 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(compute_pass_);
}

void ReblurDenoiser::TemporalAccumulate(const ReblurInputBuffers &inputs, const ReblurSettings &settings)
{
    auto *resources = temporal_accum_pipeline_->GetShaderResource<ReblurTemporalAccumShader>();

    // Bind current frame inputs
    resources->inDiffuse().BindResource(diff_temp1_->GetDefaultView(rhi_));
    resources->inSpecular().BindResource(spec_temp1_->GetDefaultView(rhi_));
    resources->inNormalRoughness().BindResource(inputs.normal_roughness->GetDefaultView(rhi_));
    resources->inViewZ().BindResource(inputs.view_z->GetDefaultView(rhi_));

    // Bind previous frame history
    resources->prevDiffHistory().BindResource(diff_history_->GetDefaultView(rhi_));
    resources->prevSpecHistory().BindResource(spec_history_->GetDefaultView(rhi_));
    resources->prevViewZ().BindResource(prev_view_z_->GetDefaultView(rhi_));
    resources->prevNormalRoughness().BindResource(prev_normal_roughness_->GetDefaultView(rhi_));
    resources->prevInternalData().BindResource(prev_internal_data_->GetDefaultView(rhi_));

    // Bind outputs
    resources->outDiffuse().BindResource(diff_temp2_->GetDefaultView(rhi_));
    resources->outSpecular().BindResource(spec_temp2_->GetDefaultView(rhi_));
    resources->outInternalData().BindResource(internal_data_->GetDefaultView(rhi_));

    ReblurTemporalAccumShader::UniformBufferData ubo{
        .resolution = {width_, height_},
        .max_accumulated_frame_num = static_cast<float>(settings.max_accumulated_frame_num),
        .max_fast_accumulated_frame_num = static_cast<float>(settings.max_fast_accumulated_frame_num),
        .disocclusion_threshold = settings.disocclusion_threshold,
        .denoising_range = 1000.f,
        .frame_index = internal_frame_index_,
        .reset_history = history_valid_ ? 0u : 1u,
    };
    temporal_accum_ub_->Upload(rhi_, &ubo);

    // Transition inputs to Read.
    // diff_temp1_/spec_temp1_ were just written by Blur (PrePass).
    // History and prev_* textures are already in Read from CopyHistoryData/init.
    diff_temp1_->Transition({.target_layout = RHIImageLayout::Read,
                             .after_stage = RHIPipelineStage::ComputeShader,
                             .before_stage = RHIPipelineStage::ComputeShader});
    spec_temp1_->Transition({.target_layout = RHIImageLayout::Read,
                             .after_stage = RHIPipelineStage::ComputeShader,
                             .before_stage = RHIPipelineStage::ComputeShader});

    // Transition outputs to StorageWrite
    diff_temp2_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                             .after_stage = RHIPipelineStage::Top,
                             .before_stage = RHIPipelineStage::ComputeShader});
    spec_temp2_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                             .after_stage = RHIPipelineStage::Top,
                             .before_stage = RHIPipelineStage::ComputeShader});
    internal_data_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                .after_stage = RHIPipelineStage::Top,
                                .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(compute_pass_);
    rhi_->DispatchCompute(temporal_accum_pipeline_, {width_, height_, 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(compute_pass_);
}

void ReblurDenoiser::HistoryFix(const ReblurInputBuffers &inputs, const ReblurSettings &settings)
{
    auto *resources = history_fix_pipeline_->GetShaderResource<ReblurHistoryFixShader>();

    // Bind inputs (from TemporalAccum output)
    resources->inDiffuse().BindResource(diff_temp2_->GetDefaultView(rhi_));
    resources->inSpecular().BindResource(spec_temp2_->GetDefaultView(rhi_));
    resources->inNormalRoughness().BindResource(inputs.normal_roughness->GetDefaultView(rhi_));
    resources->inViewZ().BindResource(inputs.view_z->GetDefaultView(rhi_));
    resources->inInternalData().BindResource(internal_data_->GetDefaultView(rhi_));

    // Bind outputs (to temp1 for subsequent Blur pass)
    resources->outDiffuse().BindResource(diff_temp1_->GetDefaultView(rhi_));
    resources->outSpecular().BindResource(spec_temp1_->GetDefaultView(rhi_));

    ReblurHistoryFixShader::UniformBufferData ubo{
        .resolution = {width_, height_},
        .history_fix_frame_num = static_cast<float>(settings.history_fix_frame_num),
        .history_fix_stride = settings.history_fix_stride,
        .plane_dist_sensitivity = settings.plane_dist_sensitivity,
        .lobe_angle_fraction = settings.lobe_angle_fraction,
        .denoising_range = 1000.f,
    };
    history_fix_ub_->Upload(rhi_, &ubo);

    // Transition inputs to Read
    diff_temp2_->Transition({.target_layout = RHIImageLayout::Read,
                             .after_stage = RHIPipelineStage::ComputeShader,
                             .before_stage = RHIPipelineStage::ComputeShader});
    spec_temp2_->Transition({.target_layout = RHIImageLayout::Read,
                             .after_stage = RHIPipelineStage::ComputeShader,
                             .before_stage = RHIPipelineStage::ComputeShader});
    internal_data_->Transition({.target_layout = RHIImageLayout::Read,
                                .after_stage = RHIPipelineStage::ComputeShader,
                                .before_stage = RHIPipelineStage::ComputeShader});

    // Transition outputs to StorageWrite
    diff_temp1_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                             .after_stage = RHIPipelineStage::ComputeShader,
                             .before_stage = RHIPipelineStage::ComputeShader});
    spec_temp1_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                             .after_stage = RHIPipelineStage::ComputeShader,
                             .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(compute_pass_);
    rhi_->DispatchCompute(history_fix_pipeline_, {width_, height_, 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(compute_pass_);
}

void ReblurDenoiser::CopyHistoryData(RHIImage *diff, RHIImage *spec)
{
    // Copy denoised output to history for next frame's temporal accumulation
    diff->Transition({.target_layout = RHIImageLayout::TransferSrc,
                      .after_stage = RHIPipelineStage::ComputeShader,
                      .before_stage = RHIPipelineStage::Transfer});
    diff_history_->Transition({.target_layout = RHIImageLayout::TransferDst,
                               .after_stage = RHIPipelineStage::ComputeShader,
                               .before_stage = RHIPipelineStage::Transfer});
    diff->CopyToImage(diff_history_.get());

    spec->Transition({.target_layout = RHIImageLayout::TransferSrc,
                      .after_stage = RHIPipelineStage::ComputeShader,
                      .before_stage = RHIPipelineStage::Transfer});
    spec_history_->Transition({.target_layout = RHIImageLayout::TransferDst,
                               .after_stage = RHIPipelineStage::ComputeShader,
                               .before_stage = RHIPipelineStage::Transfer});
    spec->CopyToImage(spec_history_.get());

    // Copy internal data to prev for next frame
    internal_data_->Transition({.target_layout = RHIImageLayout::TransferSrc,
                                .after_stage = RHIPipelineStage::ComputeShader,
                                .before_stage = RHIPipelineStage::Transfer});
    prev_internal_data_->Transition({.target_layout = RHIImageLayout::TransferDst,
                                     .after_stage = RHIPipelineStage::ComputeShader,
                                     .before_stage = RHIPipelineStage::Transfer});
    internal_data_->CopyToImage(prev_internal_data_.get());

    // Transition everything to Read for next frame
    diff->Transition({.target_layout = RHIImageLayout::Read,
                      .after_stage = RHIPipelineStage::Transfer,
                      .before_stage = RHIPipelineStage::ComputeShader});
    spec->Transition({.target_layout = RHIImageLayout::Read,
                      .after_stage = RHIPipelineStage::Transfer,
                      .before_stage = RHIPipelineStage::ComputeShader});
    diff_history_->Transition({.target_layout = RHIImageLayout::Read,
                               .after_stage = RHIPipelineStage::Transfer,
                               .before_stage = RHIPipelineStage::ComputeShader});
    spec_history_->Transition({.target_layout = RHIImageLayout::Read,
                               .after_stage = RHIPipelineStage::Transfer,
                               .before_stage = RHIPipelineStage::ComputeShader});
    prev_internal_data_->Transition({.target_layout = RHIImageLayout::Read,
                                     .after_stage = RHIPipelineStage::Transfer,
                                     .before_stage = RHIPipelineStage::ComputeShader});
}

void ReblurDenoiser::CopyPreviousFrameData(const ReblurInputBuffers &inputs)
{
    // Transition sources to TransferSrc, destinations to TransferDst
    inputs.view_z->Transition({.target_layout = RHIImageLayout::TransferSrc,
                               .after_stage = RHIPipelineStage::ComputeShader,
                               .before_stage = RHIPipelineStage::Transfer});
    prev_view_z_->Transition({.target_layout = RHIImageLayout::TransferDst,
                              .after_stage = RHIPipelineStage::Top,
                              .before_stage = RHIPipelineStage::Transfer});
    inputs.view_z->CopyToImage(prev_view_z_.get());

    inputs.normal_roughness->Transition({.target_layout = RHIImageLayout::TransferSrc,
                                         .after_stage = RHIPipelineStage::ComputeShader,
                                         .before_stage = RHIPipelineStage::Transfer});
    prev_normal_roughness_->Transition({.target_layout = RHIImageLayout::TransferDst,
                                        .after_stage = RHIPipelineStage::Top,
                                        .before_stage = RHIPipelineStage::Transfer});
    inputs.normal_roughness->CopyToImage(prev_normal_roughness_.get());

    // Transition prev buffers to Read for next frame's temporal accumulation
    prev_view_z_->Transition({.target_layout = RHIImageLayout::Read,
                              .after_stage = RHIPipelineStage::Transfer,
                              .before_stage = RHIPipelineStage::ComputeShader});
    prev_normal_roughness_->Transition({.target_layout = RHIImageLayout::Read,
                                        .after_stage = RHIPipelineStage::Transfer,
                                        .before_stage = RHIPipelineStage::ComputeShader});
}

RHIImage *ReblurDenoiser::GetDenoisedDiffuse() const
{
    return denoised_diffuse_.get();
}

RHIImage *ReblurDenoiser::GetDenoisedSpecular() const
{
    return denoised_specular_.get();
}

void ReblurDenoiser::Reset()
{
    internal_frame_index_ = 0;
    history_valid_ = false;
}
} // namespace sparkle
