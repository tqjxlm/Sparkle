#include "application/TestCase.h"

#include "core/Logger.h"
#include "io/TextureCompression.h"
#include "io/TextureCookJob.h"

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
        bool success = VerifyIdentityRules();

        for (auto profile : {TextureCompression::Profile::Color, TextureCompression::Profile::Data,
                             TextureCompression::Profile::Normal})
        {
            for (auto family : {TextureCompression::Family::Astc, TextureCompression::Family::Bc})
            {
                success &= VerifyRoundTrip(profile, family);
            }
        }
        for (auto family : {TextureCompression::Family::Astc, TextureCompression::Family::Bc})
        {
            success &= VerifyOddMipEdges(family);
        }

        return success ? Result::Pass : Result::Fail;
    }

    static bool VerifyIdentityRules()
    {
        bool success = true;
        success &= Expect(MakeTextureIdentity("scenes/city", "textures/road.png") == "scenes/city/textures/road.png",
                          "identity joins the scene parent");
        success &= Expect(MakeTextureIdentity("", "a/../b.png") == "b.png", "identity normalizes lexically");
        success &= Expect(MakeTextureIdentity("scenes", "../shared.png") == "shared.png",
                          "inner .. resolving inside the root survives");
        success &= Expect(MakeTextureIdentity("", "../escape.png").empty(), "identity escaping the root is rejected");
        success &= Expect(MakeTextureIdentity("scenes", "../../escape.png").empty(),
                          "identity escaping through the parent is rejected");
        for (const auto &absolute : {std::string("/abs/x.png"), std::string("C:/abs/x.png"),
                                     std::string(R"(C:\abs\x.png)"), std::string(R"(\abs\x.png)"),
                                     std::string(R"(\\server\share\x.png)"), std::string("//server/share/x.png")})
        {
            success &= Expect(MakeTextureIdentity("", absolute).empty(),
                              ("absolute identity is rejected: " + absolute).c_str());
        }
        success &= Expect(MakeTextureIdentity("", "").empty(), "empty authored path yields no identity");

        auto source =
            std::make_shared<Image2D>(1, 1, PixelFormat::R8G8B8A8Unorm, std::vector<uint8_t>{32, 64, 128, 255});
        const TextureCookJob color_job(source, "texture.png", TextureCompression::Profile::Color,
                                       TextureCompression::Family::Astc);
        const TextureCookJob data_job(source, "texture.png", TextureCompression::Profile::Data,
                                      TextureCompression::Family::Astc);
        const TextureCookJob normal_job(source, "texture.png", TextureCompression::Profile::Normal,
                                        TextureCompression::Family::Astc);
        success &= Expect(color_job.GetSourceHash() != data_job.GetSourceHash() &&
                              color_job.GetSourceHash() != normal_job.GetSourceHash() &&
                              data_job.GetSourceHash() != normal_job.GetSourceHash(),
                          "texture source hashes distinguish compression profiles");
        return success;
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

        auto fresh = TextureCompression::CreateImageFromPayload(payload, "texture_compression_copy");
        const Image2D copy = *fresh; // NOLINT(performance-unnecessary-copy-initialization)
        success &= Expect(&fresh->EnsureDecoded() == &copy.EnsureDecoded(),
                          (label + ": pre-decode copy shares the decode cache").c_str());

        const float psnr = ComputePsnr(source, decoded);
        Log(Info, "TextureCompressionTest: {} PSNR {:.2f} dB", label, psnr);
        success &= Expect(psnr > 30.f, (label + ": decode fidelity above 30 dB").c_str());

        return success;
    }

    static bool VerifyOddMipEdges(TextureCompression::Family family)
    {
        bool success = VerifyOddMipEdges(family, 7, 5);
        success &= VerifyOddMipEdges(family, 3, 1);
        success &= VerifyOddMipEdges(family, 1, 3);
        return success;
    }

    static bool VerifyOddMipEdges(TextureCompression::Family family, unsigned width, unsigned height)
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4, 0);
        for (unsigned y = 0; y < height; y++)
        {
            pixels[(static_cast<size_t>(y) * width + width - 1) * 4] = 255;
        }
        for (unsigned x = 0; x < width; x++)
        {
            pixels[(static_cast<size_t>(height - 1) * width + x) * 4 + 1] = 255;
        }
        for (size_t i = 3; i < pixels.size(); i += 4)
        {
            pixels[i] = 255;
        }

        const Image2D source(width, height, PixelFormat::R8G8B8A8Unorm, pixels);
        const auto payload = TextureCompression::Encode(source, TextureCompression::Profile::Data, family);
        const auto label = std::string(TextureCompression::GetFamilyName(family)) + " " + std::to_string(width) + "x" +
                           std::to_string(height) + " odd mip";
        bool success = Expect(!payload.empty(), (label + ": encode produced a payload").c_str());
        auto compressed = TextureCompression::CreateImageFromPayload(payload, "texture_compression_odd_mip");
        success &= Expect(compressed != nullptr, (label + ": payload validates").c_str());
        if (compressed == nullptr)
        {
            return false;
        }

        constexpr double CodecTolerance = 0.01;
        for (auto mip = 1u; mip < compressed->GetMipCount(); mip++)
        {
            const auto decoded = TextureCompression::Decode(*compressed, mip);
            const size_t pixel_count = static_cast<size_t>(decoded.GetWidth()) * decoded.GetHeight();
            double red = 0.0;
            double green = 0.0;
            for (size_t i = 0; i < pixel_count; i++)
            {
                red += decoded.GetRawData()[i * 4];
                green += decoded.GetRawData()[i * 4 + 1];
            }
            const double scale = 255.0 * static_cast<double>(pixel_count);
            const double red_mean = red / scale;
            const double green_mean = green / scale;
            const double expected_red = 1.0 / width;
            const double expected_green = 1.0 / height;
            Log(Info, "TextureCompressionTest: {} {} edge mean ({:.3f}, {:.3f})", label, mip, red_mean, green_mean);
            success &= Expect(std::abs(red_mean - expected_red) < CodecTolerance &&
                                  std::abs(green_mean - expected_green) < CodecTolerance,
                              (label + " " + std::to_string(mip) + ": final row and column preserve area").c_str());
        }
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
