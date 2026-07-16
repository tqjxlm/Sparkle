#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/FileManager.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"
#include "io/Image.h"
#include "io/Mesh.h"
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
            if (!load_task_->Get())
            {
                CleanupFixtures();
                return Result::Fail;
            }
            render_barrier_ = TaskManager::RunInRenderThread([] {});
            stage_ = Stage::WaitForLoadedRenderBarrier;
            return Result::Pending;

        case Stage::WaitForLoadedRenderBarrier: {
            if (!render_barrier_->IsReady())
            {
                return Result::Pending;
            }

            const bool success =
                VerifyTextures(app.GetScene()) && VerifyLights(app.GetScene()) && VerifyMeshes(app.GetScene());
            CleanupFixtures();
            return success ? Result::Pass : Result::Fail;
        }

        default:
            return Result::Fail;
        }
    }

private:
    enum class Stage : uint8_t
    {
        InsertRenderBarrier,
        WaitForRenderBarrier,
        WaitForScene,
        WaitForLoadedRenderBarrier,
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
        uniform token subdivisionScheme = "none"
    }

    def Mesh "UntexturedTriangle" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        rel material:binding = </Root/Materials/PlainMaterial>
        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1)] (
            interpolation = "vertex"
        )
        point3f[] points = [(2, 0, 0), (3, 0, 0), (2, 1, 0)]
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

        def Material "PlainMaterial"
        {
            token outputs:surface.connect = </Root/Materials/PlainMaterial/Surface.outputs:surface>

            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0.5, 0.5, 0.5)
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

    static MeshPrimitive *FindMeshPrimitive(Scene *scene, std::string_view name)
    {
        MeshPrimitive *result = nullptr;
        scene->GetRootNode()->Traverse([name, &result](SceneNode *node) {
            for (const auto &component : node->GetComponents())
            {
                auto *primitive = dynamic_cast<MeshPrimitive *>(component.get());
                if (primitive && primitive->GetMeshResource()->name == name)
                {
                    result = primitive;
                }
            }
        });
        return result;
    }

    static bool VerifyTextures(Scene *scene)
    {
        const auto *primitive = FindMeshPrimitive(scene, "TexturedTriangle");
        Material *material = primitive ? primitive->GetMaterial() : nullptr;

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

    static bool VerifyMeshes(Scene *scene)
    {
        const auto *textured_primitive = FindMeshPrimitive(scene, "TexturedTriangle");
        const auto *untextured_primitive = FindMeshPrimitive(scene, "UntexturedTriangle");

        bool success = Expect(textured_primitive != nullptr, "loaded textured mesh");
        success &= Expect(untextured_primitive != nullptr, "loaded untextured mesh");
        if (!success)
        {
            return false;
        }

        const auto &textured_mesh = *textured_primitive->GetMeshResource();
        const auto &untextured_mesh = *untextured_primitive->GetMeshResource();
        success &= Expect(textured_mesh.Validate() && untextured_mesh.Validate(), "loaded meshes are valid");
        success &= Expect(textured_mesh.uvs.size() == textured_mesh.vertices.size(), "loaded primary UV set");
        if (textured_mesh.uvs.size() == textured_mesh.vertices.size())
        {
            bool uses_primary_uvs = true;
            for (size_t i = 0; i < textured_mesh.vertices.size(); i++)
            {
                uses_primary_uvs &= textured_mesh.uvs[i].isApprox(
                    Vector2(textured_mesh.vertices[i].x(), textured_mesh.vertices[i].y()));
            }
            success &= Expect(uses_primary_uvs, "selected texture coordinate slot zero");
        }
        success &=
            Expect(textured_mesh.tangents.size() == textured_mesh.vertices.size(), "generated fallback tangents");
        success &= Expect(untextured_mesh.uvs.empty(), "preserved missing UVs");
        success &= Expect(untextured_mesh.tangents.size() == untextured_mesh.vertices.size(),
                          "generated tangents for untextured mesh");
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
