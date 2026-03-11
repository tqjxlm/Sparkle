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
        Vector4 rotator; // cos, sin, -sin, cos
        Vector4 hit_dist_params;
        Vector4 view_row0;
        Vector4 view_row1;
        Vector4 view_row2;
        Vector2UInt resolution;
        float max_blur_radius;
        float min_blur_radius;
        float lobe_angle_fraction;
        float plane_dist_sensitivity;
        float min_hit_dist_weight;
        float denoising_range;
        float diffuse_prepass_blur_radius;
        float specular_prepass_blur_radius;
        float min_rect_dim_mul_unproject;
        float roughness_fraction;
        float unproject_x;
        float unproject_y;
        uint32_t frame_index;
        uint32_t blur_pass_index;
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
    USE_SHADER_RESOURCE(inMotionVectors, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(linearSampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(outDiffuse, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outSpecular, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outInternalData, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        float max_accumulated_frame_num;
        float disocclusion_threshold;
        float denoising_range;
        uint32_t frame_index;
        uint32_t reset_history;
        uint32_t enable_firefly_suppression;
        uint32_t debug_output;
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
        Vector4 view_row0;
        Vector4 view_row1;
        Vector4 view_row2;
        Vector2UInt resolution;
        float history_fix_frame_num;
        float history_fix_stride;
        float plane_dist_sensitivity;
        float lobe_angle_fraction;
        float denoising_range;
        float min_rect_dim_mul_unproject;
        float roughness_fraction;
        float unproject_x;
        float unproject_y;
        uint32_t enable_anti_firefly;
    };
};

class ReblurTemporalStabShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurTemporalStabShader, RHIShaderStage::Compute,
                     "shaders/ray_trace/reblur_temporal_stabilization.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(inDiffuse, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inSpecular, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inNormalRoughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inViewZ, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inInternalData, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevStabilizedDiff, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevStabilizedSpec, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevViewZ, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevNormalRoughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(outDiffuse, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outSpecular, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outInternalData, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(inMotionVectors, RHIShaderResourceReflection::ResourceType::Texture2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        float stabilization_strength;
        float fast_history_sigma_scale;
        float antilag_sigma_scale;
        float antilag_sensitivity;
        float disocclusion_threshold;
        float denoising_range;
        uint32_t frame_index;
        uint32_t max_stabilized_frame_num;
        float framerate_scale;
        uint32_t debug_output;
    };
};

class ReblurCopyStabilizedShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurCopyStabilizedShader, RHIShaderStage::Compute,
                     "shaders/ray_trace/reblur_copy_stabilized.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(inDiffuse, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inSpecular, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(outDiffuse, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    USE_SHADER_RESOURCE(outSpecular, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
    };
};

class ReblurCompositeAlbedoShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurCompositeAlbedoShader, RHIShaderStage::Compute,
                     "shaders/ray_trace/reblur_stabilize_albedo.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(inAlbedoMetallic, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inNormalRoughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inViewZ, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inMotionVectors, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(inInternalData, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevAlbedoMetallic, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevViewZ, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(prevNormalRoughness, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(linearSampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(outAlbedoMetallic, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        Vector2UInt resolution;
        float disocclusion_threshold;
        float denoising_range;
        uint32_t reset_history;
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
    init_to_read(stabilized_albedo_metallic_.get());
    init_to_read(prev_stabilized_albedo_metallic_.get());
    for (int i = 0; i < 2; i++)
    {
        init_to_read(diff_stabilized_[i].get());
        init_to_read(spec_stabilized_[i].get());
    }

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
    stabilized_albedo_metallic_ =
        make_image(PixelFormat::RGBAFloat16, width_, height_, "ReblurStabilizedAlbedoMetallic");
    prev_stabilized_albedo_metallic_ =
        make_image(PixelFormat::RGBAFloat16, width_, height_, "ReblurPrevStabilizedAlbedoMetallic");

    // Stabilized history (ping-pong)
    for (int i = 0; i < 2; i++)
    {
        std::string suffix = std::to_string(i);
        diff_stabilized_[i] = make_image(PixelFormat::RGBAFloat, width_, height_, "ReblurDiffStabilized" + suffix);
        spec_stabilized_[i] = make_image(PixelFormat::RGBAFloat, width_, height_, "ReblurSpecStabilized" + suffix);
    }

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

    // Second TA pipeline for diagnostic two-pass mode (separate UBO to avoid overwrite)
    temporal_accum_pipeline_alt_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurTemporalAccumPipelineAlt");
    temporal_accum_pipeline_alt_->SetShader<RHIShaderStage::Compute>(temporal_accum_shader_);
    temporal_accum_pipeline_alt_->Compile();

    temporal_accum_ub_alt_ = rhi_->CreateBuffer({.size = sizeof(ReblurTemporalAccumShader::UniformBufferData),
                                                  .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                                  .mem_properties = RHIMemoryProperty::None,
                                                  .is_dynamic = true},
                                                 "ReblurTemporalAccumUBOAlt");

    auto *ta_alt_resources = temporal_accum_pipeline_alt_->GetShaderResource<ReblurTemporalAccumShader>();
    ta_alt_resources->ubo().BindResource(temporal_accum_ub_alt_);

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

    // Temporal stabilization pipeline
    temporal_stab_shader_ = rhi_->CreateShader<ReblurTemporalStabShader>();
    temporal_stab_pipeline_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurTemporalStabPipeline");
    temporal_stab_pipeline_->SetShader<RHIShaderStage::Compute>(temporal_stab_shader_);
    temporal_stab_pipeline_->Compile();

    temporal_stab_ub_ = rhi_->CreateBuffer({.size = sizeof(ReblurTemporalStabShader::UniformBufferData),
                                            .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                            .mem_properties = RHIMemoryProperty::None,
                                            .is_dynamic = true},
                                           "ReblurTemporalStabUBO");

    auto *ts_resources = temporal_stab_pipeline_->GetShaderResource<ReblurTemporalStabShader>();
    ts_resources->ubo().BindResource(temporal_stab_ub_);

    copy_stabilized_shader_ = rhi_->CreateShader<ReblurCopyStabilizedShader>();
    copy_stabilized_pipeline_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurCopyStabilizedPipeline");
    copy_stabilized_pipeline_->SetShader<RHIShaderStage::Compute>(copy_stabilized_shader_);
    copy_stabilized_pipeline_->Compile();

    copy_stabilized_ub_ = rhi_->CreateBuffer({.size = sizeof(ReblurCopyStabilizedShader::UniformBufferData),
                                              .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                              .mem_properties = RHIMemoryProperty::None,
                                              .is_dynamic = true},
                                             "ReblurCopyStabilizedUBO");

    auto *copy_resources = copy_stabilized_pipeline_->GetShaderResource<ReblurCopyStabilizedShader>();
    copy_resources->ubo().BindResource(copy_stabilized_ub_);

    composite_albedo_shader_ = rhi_->CreateShader<ReblurCompositeAlbedoShader>();
    composite_albedo_pipeline_ =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "ReblurCompositeAlbedoPipeline");
    composite_albedo_pipeline_->SetShader<RHIShaderStage::Compute>(composite_albedo_shader_);
    composite_albedo_pipeline_->Compile();

    composite_albedo_ub_ = rhi_->CreateBuffer({.size = sizeof(ReblurCompositeAlbedoShader::UniformBufferData),
                                               .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                               .mem_properties = RHIMemoryProperty::None,
                                               .is_dynamic = true},
                                              "ReblurCompositeAlbedoUBO");

    auto *albedo_resources = composite_albedo_pipeline_->GetShaderResource<ReblurCompositeAlbedoShader>();
    albedo_resources->ubo().BindResource(composite_albedo_ub_);

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
                             const ReblurMatrices &matrices, uint32_t /*frame_index*/,
                             RenderConfig::ReblurDebugPass debug_pass)
{
    using DP = RenderConfig::ReblurDebugPass;

    if (debug_pass == DP::Passthrough)
    {
        CopyToOutput(const_cast<RHIImage *>(inputs.diffuse_radiance_hit_dist),
                     const_cast<RHIImage *>(inputs.specular_radiance_hit_dist));
        history_valid_ = true;
        internal_frame_index_++;
        return;
    }

    if (debug_pass == DP::InputComposite)
    {
        CopyToOutput(const_cast<RHIImage *>(inputs.diffuse_radiance_hit_dist),
                     const_cast<RHIImage *>(inputs.specular_radiance_hit_dist));
        internal_frame_index_++;
        return;
    }

    ClassifyTiles(inputs, settings);

    // PrePass: input signals → temp1 (spatial pre-filter with larger radius)
    // Uses previous frame's accumSpeed for radius adaptation
    Blur(inputs, settings, matrices, 0, inputs.diffuse_radiance_hit_dist, inputs.specular_radiance_hit_dist,
         diff_temp1_.get(), spec_temp1_.get(), prev_internal_data_.get(), history_valid_);

    if (debug_pass == DP::PrePass)
    {
        CopyToOutput(diff_temp1_.get(), spec_temp1_.get());
        internal_frame_index_++;
        return;
    }

    // Temporal Accumulation: temp1 + history → temp2, writes internal_data
    bool is_ta_diagnostic = debug_pass == DP::TADisocclusion || debug_pass == DP::TAMotionVector ||
                             debug_pass == DP::TADepth || debug_pass == DP::TAHistory ||
                             debug_pass == DP::TASpecHistory ||
                             debug_pass == DP::TAMaterialId || debug_pass == DP::TAAccumSpeed ||
                             debug_pass == DP::TASpecAccumSpeed ||
                             debug_pass == DP::TASpecMotionInputs ||
                             debug_pass == DP::TASpecQualityDelta ||
                             debug_pass == DP::TASpecSurfaceInputs ||
                             debug_pass == DP::TAMotionVectorFine;
    uint32_t ta_debug =
        is_ta_diagnostic ? (static_cast<uint32_t>(debug_pass) - static_cast<uint32_t>(DP::TADisocclusion) + 1) : 0;

    if (is_ta_diagnostic)
    {
        // Two-pass approach: diagnostic modes write non-accumulated data to temp2,
        // which would corrupt history if fed back via CopyHistoryData.
        //
        // Pass 1 (primary pipeline): run with diagnostic output, capture visualization.
        // Pass 2 (alt pipeline): run normally for proper temporal accumulation.
        //
        // Uses separate pipelines with separate UBOs because the RHI's dynamic UBO
        // Upload is a CPU memcpy — a second Upload would overwrite the first pass's
        // UBO data before the GPU reads it.
        TemporalAccumulate(inputs, settings, ta_debug);
        CopyToOutput(diff_temp2_.get(), spec_temp2_.get());
        // Pass 2: use alt pipeline (separate UBO) to avoid dynamic buffer overwrite.
        TemporalAccumulate(inputs, settings, 0, /*use_alt_pipeline=*/true);
        CopyHistoryData(diff_temp2_.get(), spec_temp2_.get());
        internal_data_->Transition({.target_layout = RHIImageLayout::Read,
                                    .after_stage = RHIPipelineStage::Transfer,
                                    .before_stage = RHIPipelineStage::ComputeShader});
        CopyPreviousFrameData(inputs);
        internal_frame_index_++;
        history_valid_ = true;
        return;
    }

    TemporalAccumulate(inputs, settings, ta_debug);

    if (debug_pass == DP::TemporalAccum || debug_pass == DP::TemporalAccumSpecular)
    {
        CopyToOutput(diff_temp2_.get(), spec_temp2_.get());
        CopyHistoryData(diff_temp2_.get(), spec_temp2_.get());
        // CopyHistoryData leaves internal_data_ in TransferSrc; transition to Read for composite
        internal_data_->Transition({.target_layout = RHIImageLayout::Read,
                                    .after_stage = RHIPipelineStage::Transfer,
                                    .before_stage = RHIPipelineStage::ComputeShader});
        CopyPreviousFrameData(inputs);
        internal_frame_index_++;
        history_valid_ = true;
        return;
    }

    // History Fix: temp2 → temp1 (wide-stride bilateral for disoccluded regions)
    HistoryFix(inputs, settings, matrices);

    // Store TemporalAccum output (temp2) as history BEFORE Blur overwrites it.
    // Critical: use TA output, NOT HistoryFix output (temp1). HistoryFix applies
    // anti-firefly clamping which, when fed back into history, compounds through
    // the temporal accumulation loop causing systematic energy loss (measured: 32%
    // at 2048 spp). The TA output in temp2 is still intact here because HistoryFix
    // reads temp2 but writes to temp1.
    CopyHistoryData(diff_temp2_.get(), spec_temp2_.get());

    // CopyHistoryData leaves internal_data_ in TransferSrc; transition to Read
    // so Blur/PostBlur can use it as a compute shader resource.
    internal_data_->Transition({.target_layout = RHIImageLayout::Read,
                                .after_stage = RHIPipelineStage::Transfer,
                                .before_stage = RHIPipelineStage::ComputeShader});

    if (debug_pass == DP::HistoryFix || debug_pass == DP::HistoryFixSpecular)
    {
        CopyToOutput(diff_temp1_.get(), spec_temp1_.get());
        CopyPreviousFrameData(inputs);
        internal_frame_index_++;
        history_valid_ = true;
        return;
    }

    // Blur: temp1 → temp2 (primary spatial filter, uses new accumSpeed)
    Blur(inputs, settings, matrices, 1, diff_temp1_.get(), spec_temp1_.get(), diff_temp2_.get(), spec_temp2_.get(),
         internal_data_.get(), true);

    if (debug_pass == DP::Blur || debug_pass == DP::BlurSpecular)
    {
        CopyToOutput(diff_temp2_.get(), spec_temp2_.get());
        CopyPreviousFrameData(inputs);
        internal_frame_index_++;
        history_valid_ = true;
        return;
    }

    // PostBlur: temp2 → denoised output (final spatial refinement, uses new accumSpeed)
    Blur(inputs, settings, matrices, 2, diff_temp2_.get(), spec_temp2_.get(), denoised_diffuse_.get(),
         denoised_specular_.get(), internal_data_.get(), true);

    // TemporalStabilization: denoised output → stabilized output (variance clamping)
    // Also writes antilag-modified accumulation speeds to prev_internal_data_
    // Skipped for PostBlur debug (isolate PostBlur output)
    if (debug_pass != DP::PostBlur && debug_pass != DP::PostBlurSpecular && settings.max_stabilized_frame_num > 0)
    {
        uint32_t ts_debug = 0u;
        if (debug_pass == DP::TSStabCount)
            ts_debug = 1u;
        else if (debug_pass == DP::TSSpecBlend)
            ts_debug = 2u;
        else if (debug_pass == DP::TSSpecAntilagInputs)
            ts_debug = 3u;
        else if (debug_pass == DP::TSSpecClampInputs)
            ts_debug = 4u;
        TemporalStabilize(inputs, settings, matrices, ts_debug);

        // Copy stabilized result into denoised_diffuse_/denoised_specular_ for composite
        uint32_t cur_idx = stab_ping_pong_;
        CopyStabilizedHistory(diff_stabilized_[cur_idx].get(), spec_stabilized_[cur_idx].get());
    }

    StabilizeCompositeAlbedo(inputs, settings);
    CopyPreviousFrameData(inputs, stabilized_albedo_metallic_.get());

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

void ReblurDenoiser::Blur(const ReblurInputBuffers &inputs, const ReblurSettings &settings,
                          const ReblurMatrices &matrices, uint32_t pass_index, RHIImage *in_diff, RHIImage *in_spec,
                          RHIImage *out_diff, RHIImage *out_spec, RHIImage *internal_data, bool has_temporal_data)
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

    // Compute derived values from matrices
    float unproject_x = 1.0f / matrices.view_to_clip(0, 0);
    float unproject_y = 1.0f / matrices.view_to_clip(1, 1);
    float min_dim = std::min(static_cast<float>(width_), static_cast<float>(height_));
    float min_rect_dim_mul_unproject = min_dim * 2.0f / (static_cast<float>(height_) * matrices.view_to_clip(1, 1));

    // View matrix rows for world→view normal transform (columns of view_to_world = rows of world_to_view)
    Vector4 view_row0 = matrices.view_to_world.col(0);
    Vector4 view_row1 = matrices.view_to_world.col(1);
    Vector4 view_row2 = matrices.view_to_world.col(2);
    Vector4 hit_dist_params(settings.hit_dist_params[0], settings.hit_dist_params[1], settings.hit_dist_params[2],
                            settings.hit_dist_params[3]);

    ReblurBlurShader::UniformBufferData ubo{
        .rotator = {cos_a, sin_a, -sin_a, cos_a},
        .hit_dist_params = hit_dist_params,
        .view_row0 = view_row0,
        .view_row1 = view_row1,
        .view_row2 = view_row2,
        .resolution = {width_, height_},
        .max_blur_radius = settings.max_blur_radius,
        .min_blur_radius = settings.min_blur_radius,
        .lobe_angle_fraction = settings.lobe_angle_fraction,
        .plane_dist_sensitivity = settings.plane_dist_sensitivity,
        .min_hit_dist_weight = settings.min_hit_dist_weight,
        .denoising_range = 1000.f,
        .diffuse_prepass_blur_radius = settings.diffuse_prepass_blur_radius,
        .specular_prepass_blur_radius = settings.specular_prepass_blur_radius,
        .min_rect_dim_mul_unproject = min_rect_dim_mul_unproject,
        .roughness_fraction = settings.roughness_fraction,
        .unproject_x = unproject_x,
        .unproject_y = unproject_y,
        .frame_index = internal_frame_index_,
        .blur_pass_index = pass_index,
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

void ReblurDenoiser::TemporalAccumulate(const ReblurInputBuffers &inputs, const ReblurSettings &settings,
                                        uint32_t debug_output, bool use_alt_pipeline)
{
    auto &pipeline = use_alt_pipeline ? temporal_accum_pipeline_alt_ : temporal_accum_pipeline_;
    auto &ub = use_alt_pipeline ? temporal_accum_ub_alt_ : temporal_accum_ub_;
    auto *resources = pipeline->GetShaderResource<ReblurTemporalAccumShader>();

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

    // Bind motion vectors and sampler for bilinear history sampling
    resources->inMotionVectors().BindResource(inputs.motion_vectors->GetDefaultView(rhi_));
    resources->linearSampler().BindResource(diff_history_->GetSampler());

    // Bind outputs
    resources->outDiffuse().BindResource(diff_temp2_->GetDefaultView(rhi_));
    resources->outSpecular().BindResource(spec_temp2_->GetDefaultView(rhi_));
    resources->outInternalData().BindResource(internal_data_->GetDefaultView(rhi_));

    ReblurTemporalAccumShader::UniformBufferData ubo{
        .resolution = {width_, height_},
        .max_accumulated_frame_num = static_cast<float>(settings.max_accumulated_frame_num),
        .disocclusion_threshold = settings.disocclusion_threshold,
        .denoising_range = 1000.f,
        .frame_index = internal_frame_index_,
        .reset_history = history_valid_ ? 0u : 1u,
        .enable_firefly_suppression = settings.enable_anti_firefly ? 1u : 0u,
        .debug_output = debug_output,
    };
    ub->Upload(rhi_, &ubo);

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
    rhi_->DispatchCompute(pipeline, {width_, height_, 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(compute_pass_);
}

void ReblurDenoiser::HistoryFix(const ReblurInputBuffers &inputs, const ReblurSettings &settings,
                                const ReblurMatrices &matrices)
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

    // Compute derived values from matrices
    float unproject_x = 1.0f / matrices.view_to_clip(0, 0);
    float unproject_y = 1.0f / matrices.view_to_clip(1, 1);
    float min_dim = std::min(static_cast<float>(width_), static_cast<float>(height_));
    float min_rect_dim_mul_unproject = min_dim * 2.0f / (static_cast<float>(height_) * matrices.view_to_clip(1, 1));
    Vector4 view_row0 = matrices.view_to_world.col(0);
    Vector4 view_row1 = matrices.view_to_world.col(1);
    Vector4 view_row2 = matrices.view_to_world.col(2);

    ReblurHistoryFixShader::UniformBufferData ubo{
        .view_row0 = view_row0,
        .view_row1 = view_row1,
        .view_row2 = view_row2,
        .resolution = {width_, height_},
        .history_fix_frame_num = static_cast<float>(settings.history_fix_frame_num),
        .history_fix_stride = settings.history_fix_stride,
        .plane_dist_sensitivity = settings.plane_dist_sensitivity,
        .lobe_angle_fraction = settings.lobe_angle_fraction,
        .denoising_range = 1000.f,
        .min_rect_dim_mul_unproject = min_rect_dim_mul_unproject,
        .roughness_fraction = settings.roughness_fraction,
        .unproject_x = unproject_x,
        .unproject_y = unproject_y,
        .enable_anti_firefly = settings.enable_anti_firefly ? 1u : 0u,
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

void ReblurDenoiser::TemporalStabilize(const ReblurInputBuffers &inputs, const ReblurSettings &settings,
                                       const ReblurMatrices &matrices, uint32_t debug_output)
{
    auto *resources = temporal_stab_pipeline_->GetShaderResource<ReblurTemporalStabShader>();

    uint32_t prev_idx = stab_ping_pong_;
    uint32_t cur_idx = 1 - stab_ping_pong_;

    // Bind inputs (denoised output from PostBlur, currently in denoised_diffuse_/denoised_specular_)
    resources->inDiffuse().BindResource(denoised_diffuse_->GetDefaultView(rhi_));
    resources->inSpecular().BindResource(denoised_specular_->GetDefaultView(rhi_));
    resources->inNormalRoughness().BindResource(inputs.normal_roughness->GetDefaultView(rhi_));
    resources->inViewZ().BindResource(inputs.view_z->GetDefaultView(rhi_));
    resources->inInternalData().BindResource(internal_data_->GetDefaultView(rhi_));

    // Bind stabilized history (previous frame)
    resources->prevStabilizedDiff().BindResource(diff_stabilized_[prev_idx]->GetDefaultView(rhi_));
    resources->prevStabilizedSpec().BindResource(spec_stabilized_[prev_idx]->GetDefaultView(rhi_));
    resources->prevViewZ().BindResource(prev_view_z_->GetDefaultView(rhi_));
    resources->prevNormalRoughness().BindResource(prev_normal_roughness_->GetDefaultView(rhi_));

    // Bind outputs (current stabilized + antilag-modified accum speeds)
    resources->outDiffuse().BindResource(diff_stabilized_[cur_idx]->GetDefaultView(rhi_));
    resources->outSpecular().BindResource(spec_stabilized_[cur_idx]->GetDefaultView(rhi_));
    resources->outInternalData().BindResource(prev_internal_data_->GetDefaultView(rhi_));

    // Bind motion vectors for reprojection
    resources->inMotionVectors().BindResource(inputs.motion_vectors->GetDefaultView(rhi_));

    ReblurTemporalStabShader::UniformBufferData ubo{
        .resolution = {width_, height_},
        .stabilization_strength = settings.stabilization_strength,
        .fast_history_sigma_scale = settings.fast_history_sigma_scale,
        .antilag_sigma_scale = settings.antilag_sigma_scale,
        .antilag_sensitivity = settings.antilag_sensitivity,
        .disocclusion_threshold = settings.disocclusion_threshold,
        .denoising_range = 1000.f,
        .frame_index = internal_frame_index_,
        .max_stabilized_frame_num = settings.max_stabilized_frame_num,
        .framerate_scale = matrices.framerate_scale,
        .debug_output = debug_output,
    };
    temporal_stab_ub_->Upload(rhi_, &ubo);

    // Transition inputs to Read
    denoised_diffuse_->Transition({.target_layout = RHIImageLayout::Read,
                                   .after_stage = RHIPipelineStage::ComputeShader,
                                   .before_stage = RHIPipelineStage::ComputeShader});
    denoised_specular_->Transition({.target_layout = RHIImageLayout::Read,
                                    .after_stage = RHIPipelineStage::ComputeShader,
                                    .before_stage = RHIPipelineStage::ComputeShader});
    inputs.normal_roughness->Transition({.target_layout = RHIImageLayout::Read,
                                         .after_stage = RHIPipelineStage::ComputeShader,
                                         .before_stage = RHIPipelineStage::ComputeShader});
    internal_data_->Transition({.target_layout = RHIImageLayout::Read,
                                .after_stage = RHIPipelineStage::ComputeShader,
                                .before_stage = RHIPipelineStage::ComputeShader});
    prev_view_z_->Transition({.target_layout = RHIImageLayout::Read,
                              .after_stage = RHIPipelineStage::Transfer,
                              .before_stage = RHIPipelineStage::ComputeShader});
    prev_normal_roughness_->Transition({.target_layout = RHIImageLayout::Read,
                                        .after_stage = RHIPipelineStage::Transfer,
                                        .before_stage = RHIPipelineStage::ComputeShader});

    // Transition outputs to StorageWrite
    diff_stabilized_[cur_idx]->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                           .after_stage = RHIPipelineStage::Top,
                                           .before_stage = RHIPipelineStage::ComputeShader});
    spec_stabilized_[cur_idx]->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                           .after_stage = RHIPipelineStage::Top,
                                           .before_stage = RHIPipelineStage::ComputeShader});
    // prev_internal_data_ receives antilag-modified accumulation speeds
    // CopyHistoryData leaves prev_internal_data_ in Read (available to ComputeShader)
    prev_internal_data_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                     .after_stage = RHIPipelineStage::ComputeShader,
                                     .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(compute_pass_);
    rhi_->DispatchCompute(temporal_stab_pipeline_, {width_, height_, 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(compute_pass_);

    // Transition prev_internal_data_ to Read for next frame's PrePass and TemporalAccum
    prev_internal_data_->Transition({.target_layout = RHIImageLayout::Read,
                                     .after_stage = RHIPipelineStage::ComputeShader,
                                     .before_stage = RHIPipelineStage::ComputeShader});

    // Flip ping-pong
    stab_ping_pong_ = cur_idx;
}

void ReblurDenoiser::CopyStabilizedHistory(RHIImage *diff, RHIImage *spec)
{
    auto *resources = copy_stabilized_pipeline_->GetShaderResource<ReblurCopyStabilizedShader>();
    resources->inDiffuse().BindResource(diff->GetDefaultView(rhi_));
    resources->inSpecular().BindResource(spec->GetDefaultView(rhi_));
    resources->outDiffuse().BindResource(denoised_diffuse_->GetDefaultView(rhi_));
    resources->outSpecular().BindResource(denoised_specular_->GetDefaultView(rhi_));

    ReblurCopyStabilizedShader::UniformBufferData ubo{
        .resolution = {width_, height_},
    };
    copy_stabilized_ub_->Upload(rhi_, &ubo);

    diff->Transition({.target_layout = RHIImageLayout::Read,
                      .after_stage = RHIPipelineStage::ComputeShader,
                      .before_stage = RHIPipelineStage::ComputeShader});
    spec->Transition({.target_layout = RHIImageLayout::Read,
                      .after_stage = RHIPipelineStage::ComputeShader,
                      .before_stage = RHIPipelineStage::ComputeShader});
    denoised_diffuse_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                   .after_stage = RHIPipelineStage::ComputeShader,
                                   .before_stage = RHIPipelineStage::ComputeShader});
    denoised_specular_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                    .after_stage = RHIPipelineStage::ComputeShader,
                                    .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(compute_pass_);
    rhi_->DispatchCompute(copy_stabilized_pipeline_, {width_, height_, 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(compute_pass_);

    denoised_diffuse_->Transition({.target_layout = RHIImageLayout::Read,
                                   .after_stage = RHIPipelineStage::ComputeShader,
                                   .before_stage = RHIPipelineStage::ComputeShader});
    denoised_specular_->Transition({.target_layout = RHIImageLayout::Read,
                                    .after_stage = RHIPipelineStage::ComputeShader,
                                    .before_stage = RHIPipelineStage::ComputeShader});
}

void ReblurDenoiser::StabilizeCompositeAlbedo(const ReblurInputBuffers &inputs, const ReblurSettings &settings)
{
    auto *resources = composite_albedo_pipeline_->GetShaderResource<ReblurCompositeAlbedoShader>();
    resources->inAlbedoMetallic().BindResource(inputs.albedo_metallic->GetDefaultView(rhi_));
    resources->inNormalRoughness().BindResource(inputs.normal_roughness->GetDefaultView(rhi_));
    resources->inViewZ().BindResource(inputs.view_z->GetDefaultView(rhi_));
    resources->inMotionVectors().BindResource(inputs.motion_vectors->GetDefaultView(rhi_));
    resources->inInternalData().BindResource(internal_data_->GetDefaultView(rhi_));
    resources->prevAlbedoMetallic().BindResource(prev_stabilized_albedo_metallic_->GetDefaultView(rhi_));
    resources->prevViewZ().BindResource(prev_view_z_->GetDefaultView(rhi_));
    resources->prevNormalRoughness().BindResource(prev_normal_roughness_->GetDefaultView(rhi_));
    resources->linearSampler().BindResource(prev_stabilized_albedo_metallic_->GetSampler());
    resources->outAlbedoMetallic().BindResource(stabilized_albedo_metallic_->GetDefaultView(rhi_));

    ReblurCompositeAlbedoShader::UniformBufferData ubo{
        .resolution = {width_, height_},
        .disocclusion_threshold = settings.disocclusion_threshold,
        .denoising_range = 1000.f,
        .reset_history = history_valid_ ? 0u : 1u,
    };
    composite_albedo_ub_->Upload(rhi_, &ubo);

    inputs.albedo_metallic->Transition({.target_layout = RHIImageLayout::Read,
                                        .after_stage = RHIPipelineStage::ComputeShader,
                                        .before_stage = RHIPipelineStage::ComputeShader});
    inputs.normal_roughness->Transition({.target_layout = RHIImageLayout::Read,
                                         .after_stage = RHIPipelineStage::ComputeShader,
                                         .before_stage = RHIPipelineStage::ComputeShader});
    inputs.view_z->Transition({.target_layout = RHIImageLayout::Read,
                               .after_stage = RHIPipelineStage::ComputeShader,
                               .before_stage = RHIPipelineStage::ComputeShader});
    inputs.motion_vectors->Transition({.target_layout = RHIImageLayout::Read,
                                       .after_stage = RHIPipelineStage::ComputeShader,
                                       .before_stage = RHIPipelineStage::ComputeShader});
    internal_data_->Transition({.target_layout = RHIImageLayout::Read,
                                .after_stage = RHIPipelineStage::ComputeShader,
                                .before_stage = RHIPipelineStage::ComputeShader});
    prev_stabilized_albedo_metallic_->Transition({.target_layout = RHIImageLayout::Read,
                                                  .after_stage = RHIPipelineStage::Transfer,
                                                  .before_stage = RHIPipelineStage::ComputeShader});
    prev_view_z_->Transition({.target_layout = RHIImageLayout::Read,
                              .after_stage = RHIPipelineStage::Transfer,
                              .before_stage = RHIPipelineStage::ComputeShader});
    prev_normal_roughness_->Transition({.target_layout = RHIImageLayout::Read,
                                        .after_stage = RHIPipelineStage::Transfer,
                                        .before_stage = RHIPipelineStage::ComputeShader});

    stabilized_albedo_metallic_->Transition({.target_layout = RHIImageLayout::StorageWrite,
                                             .after_stage = RHIPipelineStage::Top,
                                             .before_stage = RHIPipelineStage::ComputeShader});

    rhi_->BeginComputePass(compute_pass_);
    rhi_->DispatchCompute(composite_albedo_pipeline_, {width_, height_, 1u}, {16u, 16u, 1u});
    rhi_->EndComputePass(compute_pass_);

    stabilized_albedo_metallic_->Transition({.target_layout = RHIImageLayout::Read,
                                             .after_stage = RHIPipelineStage::ComputeShader,
                                             .before_stage = RHIPipelineStage::ComputeShader});
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

void ReblurDenoiser::CopyPreviousFrameData(const ReblurInputBuffers &inputs, RHIImage *albedo_history_source)
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

    RHIImage *albedo_src = albedo_history_source != nullptr ? albedo_history_source : inputs.albedo_metallic;
    albedo_src->Transition({.target_layout = RHIImageLayout::TransferSrc,
                            .after_stage = RHIPipelineStage::ComputeShader,
                            .before_stage = RHIPipelineStage::Transfer});
    prev_stabilized_albedo_metallic_->Transition({.target_layout = RHIImageLayout::TransferDst,
                                                  .after_stage = RHIPipelineStage::Top,
                                                  .before_stage = RHIPipelineStage::Transfer});
    albedo_src->CopyToImage(prev_stabilized_albedo_metallic_.get());

    // Transition prev buffers to Read for next frame's temporal accumulation
    prev_view_z_->Transition({.target_layout = RHIImageLayout::Read,
                              .after_stage = RHIPipelineStage::Transfer,
                              .before_stage = RHIPipelineStage::ComputeShader});
    prev_normal_roughness_->Transition({.target_layout = RHIImageLayout::Read,
                                        .after_stage = RHIPipelineStage::Transfer,
                                        .before_stage = RHIPipelineStage::ComputeShader});
    prev_stabilized_albedo_metallic_->Transition({.target_layout = RHIImageLayout::Read,
                                                  .after_stage = RHIPipelineStage::Transfer,
                                                  .before_stage = RHIPipelineStage::ComputeShader});

    // Transition source inputs back to Read (they are borrowed from the caller)
    inputs.view_z->Transition({.target_layout = RHIImageLayout::Read,
                               .after_stage = RHIPipelineStage::Transfer,
                               .before_stage = RHIPipelineStage::ComputeShader});
    inputs.normal_roughness->Transition({.target_layout = RHIImageLayout::Read,
                                         .after_stage = RHIPipelineStage::Transfer,
                                         .before_stage = RHIPipelineStage::ComputeShader});
    albedo_src->Transition({.target_layout = RHIImageLayout::Read,
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

RHIImage *ReblurDenoiser::GetInternalData() const
{
    return internal_data_.get();
}

RHIImage *ReblurDenoiser::GetCompositeAlbedoMetallic() const
{
    return stabilized_albedo_metallic_.get();
}

void ReblurDenoiser::Reset()
{
    internal_frame_index_ = 0;
    history_valid_ = false;
    stab_ping_pong_ = 0;
}
} // namespace sparkle
