#include "application/TestCase.h"

#include "core/Logger.h"
#include "io/TextureCompression.h"

#include <algorithm>
#include <cmath>

namespace sparkle
{
// encode/decode invariants of every profile x family combination on a synthetic image:
// payload layout, full mip chain, and a decode fidelity floor. runs anywhere: no store,
// no cooker, no RHI
class TextureCompressionTest : public TestCase
{
    // odd, non-power-of-two dimensions exercise partial blocks and odd mip halving
    static constexpr unsigned Width = 67;
    static constexpr unsigned Height = 41;

    Result OnTick(AppFramework & /*app*/) override
    {
        bool success = true;

        for (auto profile : {TextureCompression::Profile::Color, TextureCompression::Profile::Data,
                             TextureCompression::Profile::Normal})
        {
            for (auto family : {TextureCompression::Family::Astc, TextureCompression::Family::Bc})
            {
                success &= VerifyRoundTrip(profile, family);
            }
        }

        return success ? Result::Pass : Result::Fail;
    }

    static Image2D MakeSource(TextureCompression::Profile profile)
    {
        const bool srgb = profile == TextureCompression::Profile::Color;

        std::vector<uint8_t> pixels(static_cast<size_t>(Width) * Height * 4);
        for (unsigned y = 0; y < Height; y++)
        {
            for (unsigned x = 0; x < Width; x++)
            {
                const size_t i = (static_cast<size_t>(y) * Width + x) * 4;
                if (profile == TextureCompression::Profile::Normal)
                {
                    // a smooth unit vector field packed into [0, 255]
                    const float nx = std::sin(static_cast<float>(x) * 0.2f) * 0.5f;
                    const float ny = std::cos(static_cast<float>(y) * 0.15f) * 0.5f;
                    const float nz = std::sqrt(std::max(1.f - nx * nx - ny * ny, 0.f));
                    pixels[i + 0] = static_cast<uint8_t>((nx * 0.5f + 0.5f) * 255.f);
                    pixels[i + 1] = static_cast<uint8_t>((ny * 0.5f + 0.5f) * 255.f);
                    pixels[i + 2] = static_cast<uint8_t>((nz * 0.5f + 0.5f) * 255.f);
                }
                else
                {
                    // gradients plus block-scale hash structure and a mild dither: busy
                    // enough to be non-trivial, but not white noise, which no 3.56 bpp
                    // format can hold above 30 dB
                    const auto cell_hash =
                        ((x >> 2) * 374761393u + (y >> 2) * 668265263u) ^ (((x >> 2) * (y >> 2)) * 2654435761u);
                    const auto dither = (x * 374761393u + y * 668265263u) % 32u;
                    pixels[i + 0] = static_cast<uint8_t>((x * 255u) / Width);
                    pixels[i + 1] = static_cast<uint8_t>((y * 255u) / Height);
                    pixels[i + 2] = static_cast<uint8_t>(std::min(cell_hash % 224u + dither, 255u));
                }
                pixels[i + 3] = 255;
            }
        }

        return {Width, Height, srgb ? PixelFormat::R8G8B8A8Srgb : PixelFormat::R8G8B8A8Unorm, pixels};
    }

    static bool VerifyRoundTrip(TextureCompression::Profile profile, TextureCompression::Family family)
    {
        const auto label = std::string(TextureCompression::GetFamilyName(family)) + " profile " +
                           std::to_string(static_cast<int>(profile));

        const auto source = MakeSource(profile);
        const auto payload = TextureCompression::Encode(source, profile, family);
        bool success = Expect(!payload.empty(), (label + ": encode produced a payload").c_str());
        if (!success)
        {
            return false;
        }

        auto compressed = TextureCompression::CreateImageFromPayload(payload, "texture_compression_test");
        success &= Expect(compressed != nullptr, (label + ": payload validates").c_str());
        if (compressed == nullptr)
        {
            return false;
        }

        success &= Expect(compressed->IsValid(), (label + ": compressed image is valid").c_str());
        success &= Expect(compressed->GetWidth() == Width && compressed->GetHeight() == Height,
                          (label + ": dimensions survive").c_str());
        success &= Expect(compressed->GetMipCount() == 7, (label + ": full mip chain down to 1x1").c_str());
        success &= Expect(compressed->GetFormat() == TextureCompression::SelectFormat(profile, family),
                          (label + ": format matches the profile").c_str());
        success &= Expect(IsSRGBFormat(compressed->GetFormat()) == IsSRGBFormat(source.GetFormat()),
                          (label + ": sRGB-ness survives").c_str());

        const auto &decoded = compressed->EnsureDecoded();
        success &= Expect(decoded.IsValid() && decoded.GetFormat() == source.GetFormat(),
                          (label + ": decode restores the source format").c_str());

        const float psnr = ComputePsnr(source, decoded);
        Log(Info, "TextureCompressionTest: {} PSNR {:.2f} dB", label, psnr);
        success &= Expect(psnr > 30.f, (label + ": decode fidelity above 30 dB").c_str());

        return success;
    }

    static float ComputePsnr(const Image2D &reference, const Image2D &decoded)
    {
        const auto *a = reference.GetRawData();
        const auto *b = decoded.GetRawData();
        double squared_error = 0.0;
        const size_t count = static_cast<size_t>(Width) * Height;
        for (size_t i = 0; i < count; i++)
        {
            for (unsigned channel = 0; channel < 3; channel++)
            {
                const double diff = static_cast<double>(a[i * 4 + channel]) - static_cast<double>(b[i * 4 + channel]);
                squared_error += diff * diff;
            }
        }
        const double mse = squared_error / (static_cast<double>(count) * 3.0);
        if (mse <= 0.0)
        {
            return 99.f;
        }
        return static_cast<float>(10.0 * std::log10(255.0 * 255.0 / mse));
    }

    static bool Expect(bool condition, const char *description)
    {
        if (condition)
        {
            Log(Info, "TextureCompressionTest: OK - {}", description);
        }
        else
        {
            Log(Error, "TextureCompressionTest: FAILED - {}", description);
        }
        return condition;
    }
};

static TestCaseRegistrar<TextureCompressionTest> texture_compression_test_registrar("texture_compression");
} // namespace sparkle
