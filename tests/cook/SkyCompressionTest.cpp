#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "io/HdrCubeTranscodeJob.h"
#include "io/Image.h"
#include "io/TextureCompression.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "rhi/RHI.h"
#include "scene/Scene.h"
#include "scene/component/light/SkyLight.h"

#include <cstring>

namespace sparkle
{
// the fp16 master sky cube a dev run resolves, plus the family transcode derived from it:
// the artifact chain packaged targets ship
class SkyCompressionTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        const auto *sky_light = app.GetScene()->GetSkyLight();
        const auto *sky_proxy = app.GetScene()->GetRenderProxy()->GetSkyLight();
        if (sky_light == nullptr || !sky_light->GetCubeMap() || sky_proxy == nullptr || !sky_proxy->GetSkyMap())
        {
            return Result::Pending;
        }

        // a dev pool resolves the fp16 master; a packaged image carries only the transcode
        const auto &cube = sky_light->GetCubeMap();
        const auto transcode_format = TextureCompression::SelectHdrFormat(TextureCompression::PlatformFamily);
        const bool is_master = cube->GetFormat() == PixelFormat::RGBAFloat16;
        bool success = Expect(is_master || cube->GetFormat() == transcode_format,
                              "sky cube is the fp16 master or the family transcode");

        const std::array<Vector3, 6> directions{Right, -Right, Up, -Up, Front, -Front};
        for (const auto &direction : directions)
        {
            const Vector3 sample = cube->Sample(direction);
            success &= Expect(sample.array().isFinite().all() && sample.minCoeff() >= 0.f,
                              "CPU sky sampling returns finite non-negative HDR");
        }

        const PixelFormat expected_upload = is_master || !app.GetRHI()->SupportsSampledFormat(transcode_format)
                                                ? PixelFormat::RGBAFloat16
                                                : transcode_format;
        success &= Expect(sky_proxy->GetSkyMap()->GetAttributes().format == expected_upload,
                          "GPU sky cube uploads natively or falls back to fp16");

        success &= VerifyTranscode(*cube);

        return success ? Result::Pass : Result::Fail;
    }

private:
    // runs the transcode job on a small payload sampled from the live cube: full-resolution
    // ASTC encoding would take minutes in a Debug astcenc
    static bool VerifyTranscode(const Image2DCube &cube)
    {
        constexpr unsigned Size = 128;
        const size_t face_size = static_cast<size_t>(Size) * Size * GetPixelSize(PixelFormat::RGBAFloat16);

        std::vector<uint8_t> fp16(6 * face_size);
        const unsigned stride = cube.GetWidth() / Size;
        for (unsigned id = 0; id < Image2DCube::FaceId::Count; id++)
        {
            const auto &face = cube.GetFace(static_cast<Image2DCube::FaceId>(id));
            auto *pixels = reinterpret_cast<Vector4h *>(fp16.data() + id * face_size);
            for (unsigned y = 0; y < Size; y++)
            {
                for (unsigned x = 0; x < Size; x++)
                {
                    // box-filter, not point-sample: aliased content would inflate the codec error
                    Vector4 sum = Vector4::Zero();
                    for (unsigned sy = 0; sy < stride; sy++)
                    {
                        for (unsigned sx = 0; sx < stride; sx++)
                        {
                            sum += face.AccessPixel((x * stride) + sx, (y * stride) + sy);
                        }
                    }
                    pixels[(static_cast<size_t>(y) * Size) + x] =
                        (sum / static_cast<float>(stride * stride)).cast<Half>();
                }
            }
        }

        auto master = TextureCompression::WrapFp16Payload(fp16.data(), fp16.size(), Size, Size, 1);
        const std::array<char, 4> tail{1, 2, 3, 4};
        master.insert(master.end(), tail.begin(), tail.end());

        HdrCubeTranscodeJob job("skylight", TextureCompression::PlatformFamily, "sky_compression_test",
                                std::move(master), 0);
        auto result = job.Execute();
        bool success = Expect(result.IsSuccess() && !result.GetPayload().empty(), "transcode job produces a payload");
        if (!success)
        {
            return false;
        }
        const auto &payload = result.GetPayload();

        TextureCompression::PayloadHeader header{};
        std::memcpy(&header, payload.data(), sizeof(header));
        const auto expected_format = TextureCompression::SelectHdrFormat(TextureCompression::PlatformFamily);
        success &= Expect(static_cast<PixelFormat>(header.format) == expected_format,
                          "transcode carries the platform family HDR format");

        success &= Expect(payload.size() > tail.size() &&
                              std::memcmp(payload.data() + payload.size() - tail.size(), tail.data(), tail.size()) == 0,
                          "bytes after the cube region carry over");

        const auto decoded = TextureCompression::DecodeHdrCube(payload);
        success &= Expect(decoded.size() == fp16.size(), "transcode decodes back to the master layout");
        if (decoded.size() != fp16.size())
        {
            return false;
        }

        const auto *reference = reinterpret_cast<const Half *>(fp16.data());
        const auto *restored = reinterpret_cast<const Half *>(decoded.data());
        double abs_error = 0.0;
        double signal = 0.0;
        for (size_t i = 0; i < fp16.size() / sizeof(Half); i++)
        {
            // alpha is not preserved by RGB formats
            if (i % 4 == 3)
            {
                continue;
            }
            abs_error += std::abs(static_cast<float>(restored[i]) - static_cast<float>(reference[i]));
            signal += std::abs(static_cast<float>(reference[i]));
        }
        const double relative = signal > 0.0 ? abs_error / signal : 0.0;
        Log(Info, "SkyCompressionTest: transcode mean relative error {:.4f}", relative);
        // a mechanism bound, not a quality gate (packaged screenshot FLIP owns quality): the
        // downsample concentrates photographic detail, measuring ~0.073 on studio_garden ASTC
        success &= Expect(relative < 0.1, "transcode round-trip within 10% mean relative error");

        return success;
    }

    static bool Expect(bool condition, const char *description)
    {
        if (condition)
        {
            Log(Info, "SkyCompressionTest: PASS: {}", description);
        }
        else
        {
            Log(Error, "SkyCompressionTest: FAIL: {}", description);
        }
        return condition;
    }
};

static TestCaseRegistrar<SkyCompressionTest> sky_compression_test_registrar("sky_compression");
} // namespace sparkle
