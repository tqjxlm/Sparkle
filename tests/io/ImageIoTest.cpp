#include "application/TestCase.h"

#include "core/FileManager.h"
#include "core/Logger.h"
#include "io/Image.h"

#include <cstring>

namespace sparkle
{
class ImageIoTest : public TestCase
{
public:
    Result OnTick(AppFramework & /*app*/) override
    {
        auto *file_manager = FileManager::GetNativeFileManager();
        RemoveIfPresent(file_manager, LdrPath);
        RemoveIfPresent(file_manager, HdrPath);

        bool success = VerifyDefaultImage();
        success &= VerifyLdrContentDetection(file_manager);
        success &= VerifyHdrContentDetection();
        success &= VerifyRGB9E5Conformance();

        RemoveIfPresent(file_manager, LdrPath);
        RemoveIfPresent(file_manager, HdrPath);

        return success ? Result::Pass : Result::Fail;
    }

private:
    static bool VerifyDefaultImage()
    {
        const Image2D image;
        bool success = true;
        success &= Expect(image.GetWidth() == 0 && image.GetHeight() == 0, "default dimensions are zero");
        success &= Expect(image.GetFormat() == PixelFormat::Count, "default pixel format is invalid");
        success &= Expect(image.GetStorageSize() == 0 && !image.IsValid(), "default image has no storage");
        return success;
    }

    static bool VerifyLdrContentDetection(FileManager *file_manager)
    {
        const std::vector<char> ppm{'P', '6', '\n', '1', ' ', '1', '\n', '2', '5', '5', '\n', 17, 34, 51};
        bool success = Expect(!file_manager->Write(LdrPath, ppm).empty(), "wrote LDR fixture");

        Image2D image;
        success &= Expect(image.LoadFromFile(LdrPath.path.string()), "loaded LDR data with an HDR extension");
        success &= Expect(image.GetFormat() == PixelFormat::R8G8B8A8Unorm, "LDR data uses an UNORM format");
        success &= Expect(image.GetWidth() == 1 && image.GetHeight() == 1, "LDR dimensions are preserved");
        success &= Expect(image.GetStorageSize() == 4, "LDR storage contains one RGBA8 pixel");

        if (image.GetStorageSize() == 4)
        {
            const auto *pixel = image.GetRawData();
            success &= Expect(pixel[0] == 17 && pixel[1] == 34 && pixel[2] == 51 && pixel[3] == 255,
                              "LDR RGB is preserved and alpha is padded");
        }

        return success;
    }

    static bool VerifyHdrContentDetection()
    {
        Image2D source(1, 1, PixelFormat::RGBAFloat);
        source.SetPixel(0, 0, ExpectedHdrColor);

        bool success = Expect(source.WriteToFile(HdrPath), "synchronously wrote HDR fixture");

        Image2D image;
        success &= Expect(image.LoadFromFile(HdrPath.path.string()), "loaded HDR data with a PNG extension");
        success &= Expect(image.GetFormat() == PixelFormat::RGBAFloat16, "HDR data uses a half-float format");
        success &= Expect(image.GetWidth() == 1 && image.GetHeight() == 1, "HDR dimensions are preserved");
        success &= Expect(image.GetStorageSize() == GetPixelSize(PixelFormat::RGBAFloat16),
                          "HDR storage contains one RGBA16F pixel");

        if (image.IsValid())
        {
            const Vector3 actual = image.AccessPixel(0, 0).head<3>();
            success &= Expect((actual - ExpectedHdrColor).cwiseAbs().maxCoeff() < 0.02f,
                              "HDR values survive encode and half-float conversion");
        }

        return success;
    }

    static bool VerifyRGB9E5Conformance()
    {
        Image2D image(1, 1, PixelFormat::R9G9B9E5Float);
        bool success = Expect(image.GetStorageSize() == GetPixelSize(PixelFormat::R9G9B9E5Float),
                              "RGB9E5 storage is one packed word per texel");

        struct TestVector
        {
            Vector3 color;
            uint32_t packed;
        };

        for (const auto &[color, expected] :
             {TestVector{Vector3::Zero(), 0x00000000u}, TestVector{Vector3::Ones(), 0x84020100u},
              TestVector{Vector3{0.5f, 1.f, 2.f}, 0x8c010040u}})
        {
            image.SetPixel(0, 0, color);
            uint32_t packed = 0;
            memcpy(&packed, image.GetRawData(), sizeof(packed));
            success &= Expect(packed == expected, "RGB9E5 matches the packed wire format");

            std::vector<uint8_t> bytes(sizeof(expected));
            memcpy(bytes.data(), &expected, sizeof(expected));
            const Image2D encoded(1, 1, PixelFormat::R9G9B9E5Float, bytes);
            const Vector3 decoded = encoded.AccessPixel(0, 0).head<3>();
            success &= Expect(decoded == color, "RGB9E5 decodes a packed wire-format value");
        }

        return success;
    }

    static bool Expect(bool condition, const char *description)
    {
        if (condition)
        {
            Log(Info, "ImageIoTest: OK - {}", description);
        }
        else
        {
            Log(Error, "ImageIoTest: FAILED - {}", description);
        }
        return condition;
    }

    static void RemoveIfPresent(FileManager *file_manager, const Path &path)
    {
        if (file_manager->Exists(path))
        {
            file_manager->Remove(path);
        }
    }

    static inline const Path LdrPath = Path::Internal("tests/image_io_ldr.hdr");
    static inline const Path HdrPath = Path::Internal("tests/image_io_hdr.png");
    static inline const Vector3 ExpectedHdrColor{0.25f, 0.5f, 0.75f};
};

static TestCaseRegistrar<ImageIoTest> image_io_test_registrar("image_io");
} // namespace sparkle
