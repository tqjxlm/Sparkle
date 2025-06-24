#pragma once

#include "rhi/RHIResource.h"

#include "rhi/RHIImage.h"
#include <sys/types.h>

namespace sparkle
{
class RHIRenderTarget : public RHIResource
{
public:
    static constexpr uint8_t MaxNumColorImage = 8;
    using ColorImageArray = std::array<RHIResourceRef<RHIImage>, MaxNumColorImage>;

    struct Attribute
    {
        uint32_t width = 0;
        uint32_t height = 0;

        uint8_t msaa_samples = 1;
        uint8_t mip_level = 0;
        uint8_t array_layer = 0;

        std::array<RHIImage::Attribute, MaxNumColorImage> color_attributes_ = {};
        RHIImage::Attribute depth_attribute_ = {};

        [[nodiscard]] RHIImage::Attribute GetColorAttribute(size_t index = 0) const
        {
            return color_attributes_[index];
        }

        [[nodiscard]] RHIImage::Attribute GetDepthAttribute() const
        {
            return depth_attribute_;
        }

        void SetColorAttribute(const RHIImage::Attribute &attribute, size_t index)
        {
            if (width == 0 && height == 0)
            {
                width = attribute.width;
                height = attribute.height;
            }
            else
            {
                ASSERT_EQUAL(width, attribute.width);
                ASSERT_EQUAL(height, attribute.height);
                ASSERT_EQUAL(msaa_samples, attribute.msaa_samples);
            }

            ASSERT(RHIImage::ImageUsage::ColorAttachment & attribute.usages);

            color_attributes_[index] = attribute;
            color_attributes_[index].msaa_samples = msaa_samples;
        }

        void SetDepthAttribute(const RHIImage::Attribute &attribute)
        {
            if (width == 0 && height == 0)
            {
                width = attribute.width;
                height = attribute.height;
            }
            else
            {
                ASSERT_EQUAL(width, attribute.width);
                ASSERT_EQUAL(height, attribute.height);
                ASSERT_EQUAL(msaa_samples, attribute.msaa_samples);
            }

            ASSERT(RHIImage::ImageUsage::DepthStencilAttachment & attribute.usages);

            depth_attribute_ = attribute;
            depth_attribute_.msaa_samples = msaa_samples;
        }
    };

    RHIRenderTarget(const Attribute &attribute, const RHIResourceRef<RHIImage> &depth_image, const std::string &name);

    RHIRenderTarget(const Attribute &attribute, const ColorImageArray &color_images,
                    const RHIResourceRef<RHIImage> &depth_image, const std::string &name);

    RHIResourceRef<RHIImage> GetColorImage(size_t index)
    {
        return color_images_[index];
    }

    [[nodiscard]] const auto &GetColorImages() const
    {
        return color_images_;
    }

    RHIResourceRef<RHIImage> GetDepthImage()
    {
        return depth_image_;
    }

    void SetDepthImage(const RHIResourceRef<RHIImage> &image)
    {
        if (!image)
        {
            return;
        }

        depth_image_ = image;
        attribute_.SetDepthAttribute(image->GetAttributes());
    }

    void SetColorImage(const RHIResourceRef<RHIImage> &image, size_t index)
    {
        if (!image)
        {
            return;
        }

        color_images_[index] = image;
        attribute_.SetColorAttribute(image->GetAttributes(), index);
    }

    [[nodiscard]] const Attribute &GetAttribute() const
    {
        return attribute_;
    }

    void SetNeedClear(bool need_clear)
    {
        need_clear_ = need_clear;
    }

    [[nodiscard]] bool NeedClear() const
    {
        return need_clear_;
    }

    [[nodiscard]] bool IsBackBufferTarget() const
    {
        return is_back_buffer_;
    }

protected:
    void Cleanup();

    Attribute attribute_;

    ColorImageArray color_images_;
    ColorImageArray msaa_images_;
    RHIResourceRef<RHIImage> depth_image_;

    bool need_clear_ = true;

private:
    bool is_back_buffer_ = false;
};
} // namespace sparkle
