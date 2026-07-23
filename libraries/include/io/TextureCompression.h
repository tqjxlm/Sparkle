#pragma once

#include "io/Image.h"

#include <memory>
#include <string>
#include <vector>

namespace sparkle
{
// cook-time block compression for material textures, plus the CPU decode the compressed
// images fall back to. a payload is self-describing: PayloadHeader followed by tightly
// packed blocks, mip-major, full chain down to 1x1
class TextureCompression
{
public:
    enum class Profile : uint8_t
    {
        Color,  // sRGB content: base color, emissive
        Data,   // linear packed data: metallic-roughness
        Normal, // tangent-space normals: tighter blocks, renormalized mips
    };

    enum class Family : uint8_t
    {
        Astc,
        Bc,
    };

    struct PayloadHeader
    {
        uint32_t format = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mip_count = 0;
    };

    // desktop GPUs have no ASTC; Apple and Android GPUs prefer ASTC over BC. keyed on
    // the OS, not the framework: glfw on macOS samples through MoltenVK on an Apple GPU.
    // must agree per target with CookTargetFamilies in AppFramework.cpp, which decides
    // the family a target's packaged content carries
#if defined(__APPLE__) || defined(__ANDROID__)
    static constexpr Family PlatformFamily = Family::Astc;
#else
    static constexpr Family PlatformFamily = Family::Bc;
#endif

    [[nodiscard]] static const char *GetFamilyName(Family family);

    [[nodiscard]] static PixelFormat SelectFormat(Profile profile, Family family);

    // the HDR format a family carries for sky and IBL cube maps: ASTC-HDR on Apple/Android,
    // the RGB9E5 packed format on desktop (no BC HDR encoder is vendored)
    [[nodiscard]] static PixelFormat SelectHdrFormat(Family family);

    // encodes one RGBAFloat16 image into the target HDR format's tightly-packed single-mip
    // bytes. target must be R9G9B9E5Float or an HDR ASTC format. returns empty on failure
    [[nodiscard]] static std::vector<uint8_t> EncodeHdrFace(const Image2D &source, PixelFormat target_format);

    // source must be an uncompressed RGBA8 image. returns empty on failure
    [[nodiscard]] static std::vector<char> Encode(const Image2D &source, Profile profile, Family family);

    // validates the header against the payload size. returns nullptr on failure
    [[nodiscard]] static std::shared_ptr<Image2D> CreateImageFromPayload(const std::vector<char> &payload,
                                                                         const std::string &name);

    // decodes one mip of a block-compressed image into RGBA8, preserving sRGB-ness
    [[nodiscard]] static Image2D Decode(const Image2D &compressed, unsigned mip_level = 0);
};
} // namespace sparkle
