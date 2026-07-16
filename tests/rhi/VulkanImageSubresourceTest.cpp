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
            return failed_.load(std::memory_order_acquire) ? Result::Fail : Result::Pass;
        }

        started_ = true;
        task_pending_.store(true, std::memory_order_release);

        auto *rhi = app.GetRHI();
        TaskManager::RunInRenderThread([this, rhi] {
            auto image = rhi->CreateImage(MakeImageAttribute(), "VulkanImageSubresourceTestImage");
            auto target = rhi->CreateRenderTarget({.mip_level = TargetMip, .array_layer = TargetLayer}, image, nullptr,
                                                  "VulkanImageSubresourceTestTarget");

            RHIRenderPass::Attribute pass_attribute;
            pass_attribute.color_load_op = RHIRenderPass::LoadOp::Clear;
            pass_attribute.clear_color = Vector4(1.0f, 0.0f, 1.0f, 1.0f);
            auto pass = rhi->CreateRenderPass(pass_attribute, target, "VulkanImageSubresourceTestPass");

            rhi->BeginCommandBuffer();
            rhi->BeginRenderPass(pass);
            rhi->EndRenderPass();

            VerifyRenderPassLayout(image.get());

            image->Transition({.target_layout = RHIImageLayout::TransferSrc,
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
        attribute.sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
                             .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                             .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                             .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest,
                             .max_lod = 1,
                             .enable_anisotropy = false};
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

    void Expect(bool condition, const std::string &what)
    {
        if (condition)
        {
            Log(Info, "{}: OK - {}", GetName(), what);
        }
        else
        {
            Log(Error, "{}: FAILED - {}", GetName(), what);
            failed_.store(true, std::memory_order_release);
        }
    }

    bool started_ = false;
    std::atomic<bool> task_pending_{false};
    std::atomic<bool> failed_{false};
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
