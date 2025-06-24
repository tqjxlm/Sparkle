#pragma once

#include "rhi/RHIResource.h"

namespace sparkle
{
class RHIImage;
class RHIContext;

// image view defines how an image is read or written, regardless of the image's underlying format.
// for example, a cubemap can be viewed as a texture array for use in a compute shader.
// the life cycle of an image view is managed by the underlying image.
class RHIImageView : public RHIResource
{
public:
    enum class ImageViewType : uint8_t
    {
        Image2D,
        Image2DCube,
        Image2DArray,
    };

    struct Attribute
    {
        ImageViewType type = ImageViewType::Image2D;
        unsigned base_mip_level = 0;
        unsigned mip_level_count = 1;
        unsigned base_array_layer = 0;
        unsigned array_layer_count = 1;

        bool operator==(const Attribute &) const = default;
    };

    RHIImageView(Attribute attribute, RHIImage *image);

    [[nodiscard]] RHIImage *GetImage() const
    {
        return image_;
    }

    [[nodiscard]] const Attribute &GetAttribute() const
    {
        return attribute_;
    }

protected:
    Attribute attribute_;
    RHIImage *image_;
};
} // namespace sparkle

namespace std
{
template <> struct hash<sparkle::RHIImageView::Attribute>
{
    size_t operator()(const sparkle::RHIImageView::Attribute &k) const
    {
        size_t res = 17;
        res = res * 31 + hash<uint8_t>()(static_cast<uint8_t>(k.type));
        res = res * 31 + hash<unsigned>()(k.base_mip_level);
        res = res * 31 + hash<unsigned>()(k.mip_level_count);
        res = res * 31 + hash<unsigned>()(k.base_array_layer);
        res = res * 31 + hash<unsigned>()(k.array_layer_count);
        return res;
    }
};
} // namespace std
