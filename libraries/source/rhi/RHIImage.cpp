#include "rhi/RHIImage.h"

#include "core/FileManager.h"
#include "rhi/RHI.h"

namespace sparkle
{
std::string RHIImage::SaveToFile(const std::string &file_path, RHIContext *rhi)
{
    auto image_size = GetStorageSize();

    auto staging_buffer =
        rhi->CreateBuffer({.size = image_size,
                           .usages = RHIBuffer::BufferUsage::TransferDst,
                           .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                           .is_dynamic = false},
                          "ImageSaveStagingBuffer");

    rhi->BeginCommandBuffer();

    Transition({.target_layout = RHIImageLayout::TransferSrc,
                .after_stage = RHIPipelineStage::Bottom,
                .before_stage = RHIPipelineStage::Transfer});
    CopyToBuffer(staging_buffer.get());
    Transition({.target_layout = RHIImageLayout::Read,
                .after_stage = RHIPipelineStage::Transfer,
                .before_stage = RHIPipelineStage::PixelShader});

    rhi->SubmitCommandBuffer();

    rhi->EnqueueEndOfRenderTasks([]() {});

    // TODO(tqjxlm): use a completion callback or execution graph
    rhi->WaitForDeviceIdle();

    const char *buffer_data = reinterpret_cast<const char *>(staging_buffer->Lock());

    auto written_path =
        FileManager::GetNativeFileManager()->Write(FileEntry::Internal(file_path), buffer_data, image_size);

    staging_buffer->UnLock();

    return written_path;
}

bool RHIImage::LoadFromFile(const std::string &file_path)
{
    auto file_data = FileManager::GetNativeFileManager()->Read(FileEntry::Internal(file_path));
    if (file_data.empty())
    {
        return false;
    }

    auto image_size = GetStorageSize();
    if (file_data.size() != image_size)
    {
        Log(Warn, "image loading failed. size mismatch. loaded {}. expected {}.", file_data.size(), image_size);

        return false;
    }

    Upload(reinterpret_cast<uint8_t *>(file_data.data()));

    return true;
}

RHIImage::RHIImage(const Attribute &attributes, const std::string &name) : RHIResource(name), attributes_(attributes)
{
    if (attributes_.usages & ImageUsage::Texture)
    {
        ASSERT(attributes_.sampler.address_mode != RHISampler::SamplerAddressMode::Count);
    }

    for (auto i = 0u; i < attributes.mip_levels; i++)
    {
        current_layout_[i] = RHIImageLayout::Undefined;
    }
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

    auto view = rhi->CreateImageView(this, std::move(attribute));

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
