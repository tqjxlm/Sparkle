#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "io/Image.h"
#include "io/TextureCompression.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "renderer/proxy/SkyRenderProxy.h"
#include "rhi/RHI.h"
#include "scene/Scene.h"
#include "scene/component/light/SkyLight.h"

namespace sparkle
{
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

        const auto &cube = sky_light->GetCubeMap();
        const PixelFormat expected_format = TextureCompression::SelectHdrFormat(TextureCompression::PlatformFamily);
        bool success = Expect(cube->GetFormat() == expected_format, "sky cube uses the platform HDR format");

        const size_t expected_face_size = GetImageMipByteSize(expected_format, cube->GetWidth(), cube->GetHeight());
        for (unsigned id = 0; id < Image2DCube::FaceId::Count; id++)
        {
            const auto &face = cube->GetFace(static_cast<Image2DCube::FaceId>(id));
            success &= Expect(face.IsValid() && face.GetStorageSize() == expected_face_size,
                              "sky cube face has valid encoded storage");
        }

        const std::array<Vector3, 6> directions{Right, -Right, Up, -Up, Front, -Front};
        for (const auto &direction : directions)
        {
            const Vector3 sample = cube->Sample(direction);
            success &= Expect(sample.array().isFinite().all() && sample.minCoeff() >= 0.f,
                              "CPU sky sampling returns finite non-negative HDR");
        }

        const PixelFormat expected_upload =
            app.GetRHI()->SupportsSampledFormat(expected_format) ? expected_format : PixelFormat::RGBAFloat16;
        success &= Expect(sky_proxy->GetSkyMap()->GetAttributes().format == expected_upload,
                          "GPU sky cube uses the native format or fp16 fallback");

        return success ? Result::Pass : Result::Fail;
    }

private:
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
