#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/FileManager.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"
#include "io/Image.h"
#include "scene/Scene.h"
#include "scene/SceneManager.h"
#include "scene/component/light/DirectionalLight.h"
#include "scene/component/light/SkyLight.h"
#include "scene/component/primitive/MeshPrimitive.h"
#include "scene/material/Material.h"

#include <string_view>

namespace sparkle
{
class UsdLoaderSemanticsTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        switch (stage_)
        {
        case Stage::InsertRenderBarrier:
            render_barrier_ = TaskManager::RunInRenderThread([] {});
            stage_ = Stage::WaitForRenderBarrier;
            return Result::Pending;

        case Stage::WaitForRenderBarrier:
            if (!render_barrier_->IsReady())
            {
                return Result::Pending;
            }
            if (!WriteFixtures())
            {
                CleanupFixtures();
                return Result::Fail;
            }
            load_task_ = SceneManager::LoadScene(app.GetScene(), ScenePath, false, false);
            stage_ = Stage::WaitForScene;
            return Result::Pending;

        case Stage::WaitForScene:
            if (!load_task_->IsReady() || app.GetScene()->HasPendingAsyncTasks())
            {
                return Result::Pending;
            }

            const bool success = load_task_->Get() && VerifyTextures(app.GetScene()) && VerifyLights(app.GetScene());
            CleanupFixtures();
            return success ? Result::Pass : Result::Fail;
        }

        return Result::Fail;
    }

private:
    enum class Stage : uint8_t
    {
        InsertRenderBarrier,
        WaitForRenderBarrier,
        WaitForScene,
    };

    static bool WriteFixtures()
    {
        CleanupFixtures();

        Image2D image(1, 1, PixelFormat::R8G8B8A8Unorm);
        image.SetPixel(0, 0, {0.25f, 0.5f, 0.75f});

        bool success = image.WriteToFile(SrgbTexturePath);
        success &= image.WriteToFile(LinearTexturePath);

        const std::string_view scene_data = R"USD(#usda 1.0
(
    defaultPrim = "Root"
    upAxis = "Z"
)

def Xform "Root"
{
    def DistantLight "Sun"
    {
        color3f inputs:color = (0.25, 0.5, 0.75)
        float inputs:exposure = 2
        float inputs:intensity = 3
    }

    def DomeLight "Sky"
    {
        color3f inputs:color = (0.1, 0.2, 0.3)
        float inputs:intensity = 2
    }

    def Mesh "TexturedTriangle" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        rel material:binding = </Root/Materials/TestMaterial>
        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1)] (
            interpolation = "vertex"
        )
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (0, 1)] (
            interpolation = "vertex"
        )
        float4[] primvars:tangents = [(1, 0, 0, 1), (1, 0, 0, 1), (1, 0, 0, 1)] (
            interpolation = "vertex"
        )
        uniform token subdivisionScheme = "none"
    }

    def Scope "Materials"
    {
        def Material "TestMaterial"
        {
            token outputs:surface.connect = </Root/Materials/TestMaterial/Surface.outputs:surface>

            def Shader "StReader"
            {
                uniform token info:id = "UsdPrimvarReader_float2"
                string inputs:varname = "st"
                float2 outputs:result
            }

            def Shader "SrgbTexture"
            {
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @srgb.png@ (
                    colorSpace = "srgb_texture"
                )
                float2 inputs:st.connect = </Root/Materials/TestMaterial/StReader.outputs:result>
                float3 outputs:rgb
            }

            def Shader "LinearTexture"
            {
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @linear.png@ (
                    colorSpace = "lin_srgb"
                )
                float2 inputs:st.connect = </Root/Materials/TestMaterial/StReader.outputs:result>
                float3 outputs:rgb
            }

            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor.connect = </Root/Materials/TestMaterial/SrgbTexture.outputs:rgb>
                color3f inputs:emissiveColor.connect = </Root/Materials/TestMaterial/LinearTexture.outputs:rgb>
                float inputs:metallic = 0
                float inputs:roughness = 1
                token outputs:surface
            }
        }
    }
}
)USD";

        auto *file_manager = FileManager::GetNativeFileManager();
        success &= !file_manager->Write(ScenePath, scene_data.data(), scene_data.size()).empty();
        return success;
    }

    static bool VerifyTextures(Scene *scene)
    {
        Material *material = nullptr;
        scene->GetRootNode()->Traverse([&material](SceneNode *node) {
            for (const auto &component : node->GetComponents())
            {
                if (auto *primitive = dynamic_cast<MeshPrimitive *>(component.get()))
                {
                    material = primitive->GetMaterial();
                }
            }
        });

        if (!Expect(material != nullptr, "loaded textured mesh material"))
        {
            return false;
        }

        const auto &resource = material->GetRawMaterial();
        bool success = Expect(resource.base_color_texture != nullptr, "loaded sRGB texture");
        success &= Expect(resource.emissive_texture != nullptr, "loaded linear texture");
        if (!success)
        {
            return false;
        }

        success &= Expect(resource.base_color_texture->GetFormat() == PixelFormat::R8G8B8A8Srgb,
                          "srgb_texture data uses an sRGB format");
        success &= Expect(resource.emissive_texture->GetFormat() == PixelFormat::R8G8B8A8Unorm,
                          "lin_srgb data uses a linear format");
        success &= Expect(resource.base_color_texture->GetStorageSize() == 4 &&
                              resource.emissive_texture->GetStorageSize() == 4,
                          "decoded RGBA textures have exact storage");
        return success;
    }

    static bool VerifyLights(Scene *scene)
    {
        const auto *directional_light = scene->GetDirectionalLight();
        const auto *sky_light = scene->GetSkyLight();

        bool success = Expect(directional_light != nullptr, "loaded distant light");
        success &= Expect(sky_light != nullptr, "loaded dome light");
        if (!success)
        {
            return false;
        }

        success &= Expect(directional_light->GetColor().isApprox(Vector3(3.0f, 6.0f, 9.0f)),
                          "distant light applies intensity and exposure");
        success &= Expect(sky_light->GetColor().isApprox(Vector3(0.2f, 0.4f, 0.6f)), "dome light applies intensity");
        return success;
    }

    static bool Expect(bool condition, const char *description)
    {
        if (condition)
        {
            Log(Info, "UsdLoaderSemanticsTest: OK - {}", description);
        }
        else
        {
            Log(Error, "UsdLoaderSemanticsTest: FAILED - {}", description);
        }
        return condition;
    }

    static void CleanupFixtures()
    {
        auto *file_manager = FileManager::GetNativeFileManager();
        for (const auto &path : {ScenePath, SrgbTexturePath, LinearTexturePath})
        {
            if (file_manager->Exists(path))
            {
                file_manager->Remove(path);
            }
        }
    }

    static inline const Path ScenePath = Path::Internal("tests/usd_loader_semantics/scene.usda");
    static inline const Path SrgbTexturePath = Path::Internal("tests/usd_loader_semantics/srgb.png");
    static inline const Path LinearTexturePath = Path::Internal("tests/usd_loader_semantics/linear.png");

    Stage stage_ = Stage::InsertRenderBarrier;
    std::shared_ptr<TaskFuture<>> render_barrier_;
    std::shared_ptr<TaskFuture<bool>> load_task_;
};

static TestCaseRegistrar<UsdLoaderSemanticsTest> usd_loader_semantics_test_registrar("usd_loader_semantics");
} // namespace sparkle
