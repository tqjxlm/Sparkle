#include "rhi/RHIImage.h"

#include "rhi/RHI.h"

namespace sparkle
{
static RHIResourceAccess GetLegacySourceAccess(RHIPipelineStage stage)
{
    switch (stage)
    {
    case RHIPipelineStage::Top:
    case RHIPipelineStage::Bottom:
        return {};
    case RHIPipelineStage::DrawIndirect:
        return {.access = RHIAccess::IndirectArgs};
    case RHIPipelineStage::VertexInput:
        return {.access = RHIAccess::VertexInput};
    case RHIPipelineStage::VertexShader:
        return {.access = RHIAccess::StorageWrite, .stages = RHIShaderStageMask::Vertex};
    case RHIPipelineStage::PixelShader:
        return {.access = RHIAccess::StorageWrite, .stages = RHIShaderStageMask::Pixel};
    case RHIPipelineStage::EarlyZ:
    case RHIPipelineStage::LateZ:
        return {.access = RHIAccess::DepthWrite};
    case RHIPipelineStage::ColorOutput:
        return {.access = RHIAccess::ColorWrite};
    case RHIPipelineStage::ComputeShader:
        return {.access = RHIAccess::StorageWrite, .stages = RHIShaderStageMask::Compute};
    case RHIPipelineStage::Transfer:
        return {.access = RHIAccess::CopyDst};
    default:
        UnImplemented(stage);
        return {};
    }
}

static RHIShaderStageMask GetLegacyShaderStages(RHIPipelineStage stage)
{
    switch (stage)
    {
    case RHIPipelineStage::Top:
    case RHIPipelineStage::Bottom:
        return RHIShaderStageMask::All;
    case RHIPipelineStage::VertexShader:
        return RHIShaderStageMask::Vertex;
    case RHIPipelineStage::PixelShader:
        return RHIShaderStageMask::Pixel;
    case RHIPipelineStage::ComputeShader:
        return RHIShaderStageMask::Compute;
    default:
        UnImplemented(stage);
        return RHIShaderStageMask::None;
    }
}

static RHIResourceAccess GetLegacyTargetAccess(RHIImageLayout layout, RHIPipelineStage stage)
{
    switch (layout)
    {
    case RHIImageLayout::Read:
        return {.access = RHIAccess::Sampled, .stages = GetLegacyShaderStages(stage)};
    case RHIImageLayout::General:
    case RHIImageLayout::StorageWrite:
        return {.access = RHIAccess::StorageRead | RHIAccess::StorageWrite, .stages = GetLegacyShaderStages(stage)};
    case RHIImageLayout::ColorOutput:
        return {.access = RHIAccess::ColorWrite};
    case RHIImageLayout::DepthStencilOutput:
        return {.access = RHIAccess::DepthWrite};
    case RHIImageLayout::TransferSrc:
        return {.access = RHIAccess::CopySrc};
    case RHIImageLayout::TransferDst:
        return {.access = RHIAccess::CopyDst};
    case RHIImageLayout::Present:
        return {.access = RHIAccess::Present};
    default:
        UnImplemented(layout);
        return {};
    }
}

std::vector<RHIImageBarrier> RHIImage::TrackTransition(const TransitionRequest &request)
{
    ASSERT(request.base_mip < attributes_.mip_levels);
    ASSERT(request.base_array_layer < GetArrayLayerCount());

    const auto mip_count = request.mip_count == 0 ? attributes_.mip_levels - request.base_mip : request.mip_count;
    const auto array_layer_count =
        request.array_layer_count == 0 ? GetArrayLayerCount() - request.base_array_layer : request.array_layer_count;

    ASSERT(request.base_mip + mip_count <= attributes_.mip_levels);
    ASSERT(request.base_array_layer + array_layer_count <= GetArrayLayerCount());

    const auto target = GetLegacyTargetAccess(request.target_layout, request.before_stage);
    const auto legacy_source = GetLegacySourceAccess(request.after_stage);

    std::vector<RHIImageBarrier> barriers;

    const auto mip_end = request.base_mip + mip_count;
    const auto array_layer_end = request.base_array_layer + array_layer_count;
    for (auto array_layer = request.base_array_layer; array_layer < array_layer_end; array_layer++)
    {
        // contiguous mips in the same state share one barrier
        auto range_start = request.base_mip;
        while (range_start < mip_end)
        {
            const auto state = GetSubresourceState(range_start, array_layer);
            auto range_end = range_start + 1;
            while (range_end < mip_end && GetSubresourceState(range_end, array_layer) == state)
            {
                range_end++;
            }

            const bool read_after_read =
                state.layout == request.target_layout && !state.access.HasWrite() && !target.HasWrite();
            if (!read_after_read || !state.access.Contains(target))
            {
                barriers.push_back({.image = this,
                                    .base_mip = range_start,
                                    .mip_count = range_end - range_start,
                                    .base_array_layer = array_layer,
                                    .array_layer_count = 1,
                                    .from = state.access | legacy_source,
                                    .to = target,
                                    .from_layout = request.discard ? RHIImageLayout::Undefined : state.layout,
                                    .to_layout = request.target_layout});

                // later writes must also wait for the reads this barrier did not order
                const auto access = read_after_read ? state.access | target : target;
                SetCurrentState(request.target_layout, access, range_start, range_end - range_start, array_layer, 1);
            }

            range_start = range_end;
        }
    }

    return barriers;
}

std::vector<char> RHIImage::ReadToMemory(RHIContext *rhi)
{
    auto image_size = GetStorageSize();

    auto staging_buffer =
        rhi->CreateBuffer({.size = image_size,
                           .usages = RHIBuffer::BufferUsage::TransferDst,
                           .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                           .is_dynamic = false},
                          "ImageReadBackStagingBuffer");

    rhi->BeginCommandBuffer();

    Transition({.target_layout = RHIImageLayout::TransferSrc,
                .after_stage = RHIPipelineStage::Bottom,
                .before_stage = RHIPipelineStage::Transfer});
    rhi->GetCommandContext()->CopyImageToBuffer(this, staging_buffer.get());
    Transition({.target_layout = RHIImageLayout::Read,
                .after_stage = RHIPipelineStage::Transfer,
                .before_stage = RHIPipelineStage::PixelShader});

    rhi->SubmitCommandBuffer();

    rhi->EnqueueEndOfRenderTasks([]() {});

    // TODO(tqjxlm): use a completion callback or execution graph
    rhi->WaitForDeviceIdle();

    const char *buffer_data = reinterpret_cast<const char *>(staging_buffer->Lock());

    std::vector<char> data(buffer_data, buffer_data + image_size);

    staging_buffer->UnLock();

    return data;
}

RHIImage::RHIImage(const Attribute &attributes, const std::string &name) : RHIResource(name), attributes_(attributes)
{
    if (attributes_.usages & ImageUsage::Texture)
    {
        ASSERT(attributes_.sampler.address_mode != RHISampler::SamplerAddressMode::Count);
    }

    subresource_states_.assign(attributes_.mip_levels * GetArrayLayerCount(),
                               {.layout = attributes_.initial_layout, .access = {}});
}

RHIImageView::RHIImageView(Attribute attribute, RHIImage *image)
    : RHIResource(image->GetName()), attribute_(std::move(attribute)), image_(image)
{
}

RHIResourceRef<RHIImageView> RHIImage::GetView(RHIContext *rhi, const RHIImageView::Attribute &attribute)
{
    auto found = image_views_.find(attribute);
    if (found != image_views_.end())
    {
        return found->second;
    }

    auto view = rhi->CreateImageView(this, attribute);

    image_views_.emplace(attribute, view);

    return view;
}

RHIResourceRef<RHIImageView> RHIImage::GetDefaultView(RHIContext *rhi)
{
    switch (attributes_.type)
    {
    case ImageType::Image2D:
        return GetView(rhi, {
                                .type = RHIImageView::ImageViewType::Image2D,
                                .mip_level_count = attributes_.mip_levels,
                            });
    case ImageType::Image2DCube:
        return GetView(rhi, {
                                .type = RHIImageView::ImageViewType::Image2DCube,
                                .mip_level_count = attributes_.mip_levels,
                                .array_layer_count = 6,
                            });
    default:
        UnImplemented(attributes_.type);
        return nullptr;
    }
}
} // namespace sparkle
