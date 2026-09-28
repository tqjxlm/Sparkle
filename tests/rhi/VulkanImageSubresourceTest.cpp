#include "application/TestCase.h"

#if ENABLE_VULKAN

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"
#include "rhi/RHI.h"

#include <atomic>

namespace sparkle
{
class VulkanImageSubresourceTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        if (task_pending_.load(std::memory_order_acquire))
        {
            return Result::Pending;
        }

        if (started_)
        {
            return HasFailed() ? Result::Fail : Result::Pass;
        }

        started_ = true;
        task_pending_.store(true, std::memory_order_release);

        auto *rhi = app.GetRHI();
        TaskManager::RunInRenderThread([this, rhi] {
            auto image = rhi->CreateImage(MakeImageAttribute(), "VulkanImageSubresourceTestImage");

            RHIRenderingInfo info;
            info.color_attachments[0] = {.image = image.get(),
                                         .mip_level = TargetMip,
                                         .array_layer = TargetLayer,
                                         .load_op = RHILoadOp::Clear,
                                         .clear_color = Vector4(1.0f, 0.0f, 1.0f, 1.0f)};
            info.width = image->GetWidth(TargetMip);
            info.height = image->GetHeight(TargetMip);

            auto &command_context = rhi->BeginCommandBuffer();
            const auto barriers = image->TrackTransition({.target_layout = RHIImageLayout::ColorOutput,
                                                          .after_stage = RHIPipelineStage::ColorOutput,
                                                          .before_stage = RHIPipelineStage::Bottom,
                                                          .base_mip = TargetMip,
                                                          .mip_count = 1,
                                                          .base_array_layer = TargetLayer,
                                                          .array_layer_count = 1});
            command_context.BeginRendering(info, "VulkanImageSubresourceTestPass", nullptr, barriers);
            command_context.EndRendering();

            VerifyRenderPassLayout(image.get());

            image->Transition(command_context, {.target_layout = RHIImageLayout::TransferSrc,
                                                .after_stage = RHIPipelineStage::ColorOutput,
                                                .before_stage = RHIPipelineStage::Transfer});
            VerifyUniformLayout(image.get(), RHIImageLayout::TransferSrc);
            rhi->SubmitCommandBuffer();

            VerifyReadback(image.get(), image->ReadToMemory(rhi));

            task_pending_.store(false, std::memory_order_release);
        });

        return Result::Pending;
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return 1000;
    }

private:
    static constexpr unsigned TargetMip = 1;
    static constexpr unsigned TargetLayer = 3;

    static RHIImage::Attribute MakeImageAttribute()
    {
        RHIImage::Attribute attribute;
        attribute.format = PixelFormat::R8G8B8A8Unorm;
        attribute.width = 4;
        attribute.height = 4;
        attribute.usages =
            RHIImage::ImageUsage::ColorAttachment | RHIImage::ImageUsage::TransferSrc | RHIImage::ImageUsage::Texture;
        attribute.mip_levels = 2;
        attribute.type = RHIImage::ImageType::Image2DCube;
        return attribute;
    }

    void VerifyRenderPassLayout(const RHIImage *image)
    {
        bool layouts_match = true;
        for (auto mip = 0u; mip < image->GetAttributes().mip_levels; mip++)
        {
            for (auto layer = 0u; layer < image->GetArrayLayerCount(); layer++)
            {
                const auto expected =
                    mip == TargetMip && layer == TargetLayer ? RHIImageLayout::ColorOutput : RHIImageLayout::Undefined;
                layouts_match &= image->GetCurrentLayout(mip, layer) == expected;
            }
        }
        Expect(layouts_match, "render pass updates only its attached mip and cube face");
    }

    void VerifyUniformLayout(const RHIImage *image, RHIImageLayout expected)
    {
        bool layouts_match = true;
        for (auto mip = 0u; mip < image->GetAttributes().mip_levels; mip++)
        {
            for (auto layer = 0u; layer < image->GetArrayLayerCount(); layer++)
            {
                layouts_match &= image->GetCurrentLayout(mip, layer) == expected;
            }
        }
        Expect(layouts_match, "whole-image transition updates every mip and cube face");
    }

    void VerifyReadback(const RHIImage *image, const std::vector<char> &payload)
    {
        const auto mip_offset = image->GetStorageSize(0) * image->GetArrayLayerCount();
        const auto layer_offset = image->GetStorageSize(TargetMip) * TargetLayer;
        const auto offset = mip_offset + layer_offset;
        const auto byte_count = image->GetStorageSize(TargetMip);

        bool pixels_match = offset + byte_count <= payload.size();
        if (pixels_match)
        {
            const auto *pixels = reinterpret_cast<const uint8_t *>(payload.data() + offset);
            for (auto i = 0u; i < byte_count; i += 4)
            {
                pixels_match &= pixels[i] == 255 && pixels[i + 1] == 0 && pixels[i + 2] == 255 && pixels[i + 3] == 255;
            }
        }
        Expect(pixels_match, "readback preserves the selected mip and cube-face clear");
    }

    bool started_ = false;
    std::atomic<bool> task_pending_{false};
};

static TestCaseRegistrar<VulkanImageSubresourceTest> image_subresource_test_registrar("vulkan_image_subresources");
} // namespace sparkle

#else

namespace sparkle
{
class VulkanImageSubresourceTest : public TestCase
{
public:
    Result OnTick(AppFramework &) override
    {
        return Result::Fail;
    }
};

static TestCaseRegistrar<VulkanImageSubresourceTest> image_subresource_test_registrar("vulkan_image_subresources");
} // namespace sparkle

#endif
