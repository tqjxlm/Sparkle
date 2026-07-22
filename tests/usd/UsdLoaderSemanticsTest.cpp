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
            if (!success)
            {
                return Result::Fail;
            }

            load_task_ = SceneManager::LoadScene(&nested_scene_a_, NestedSceneAPath, false, false);
            stage_ = Stage::WaitForNestedSceneA;
            return Result::Pending;
        }

        case Stage::WaitForNestedSceneA:
            if (!load_task_->IsReady() || nested_scene_a_.HasPendingAsyncTasks())
            {
                return Result::Pending;
            }
            if (!load_task_->Get() || !nested_scene_a_.DidAsyncTasksSucceed())
            {
                return BeginFinalization(false, false);
            }
            load_task_ = SceneManager::LoadScene(&nested_scene_b_, NestedSceneBPath, false, false);
            stage_ = Stage::WaitForNestedSceneB;
            return Result::Pending;

        case Stage::WaitForNestedSceneB:
            if (!load_task_->IsReady() || nested_scene_b_.HasPendingAsyncTasks())
            {
                return Result::Pending;
            }
            if (!load_task_->Get() || !nested_scene_b_.DidAsyncTasksSucceed())
            {
                return BeginFinalization(false, false);
            }
            if (!VerifyNestedTextureIdentities())
            {
                return BeginFinalization(false, false);
            }
            load_task_ = SceneManager::LoadScene(&gltf_scene_, GltfScenePath, false, false);
            stage_ = Stage::WaitForGltfScene;
            return Result::Pending;

        case Stage::WaitForGltfScene: {
            if (!load_task_->IsReady() || gltf_scene_.HasPendingAsyncTasks())
            {
                return Result::Pending;
            }
            if (!load_task_->Get() || !gltf_scene_.DidAsyncTasksSucceed())
            {
                return BeginFinalization(false, false);
            }
            return BeginFinalization(true, true);
        }

        case Stage::WaitForFinalRenderBarrier:
            if (!render_barrier_->IsReady())
            {
                return Result::Pending;
            }
            if (verify_gltf_)
            {
                final_success_ = VerifyGltfTextureIdentities();
            }
            CleanupLoadedScenes();
            render_barrier_ = TaskManager::RunInRenderThread([] {});
            stage_ = Stage::WaitForCleanupRenderBarrier;
            return Result::Pending;

        case Stage::WaitForCleanupRenderBarrier:
            if (!render_barrier_->IsReady())
            {
                return Result::Pending;
            }
            return final_success_ ? Result::Pass : Result::Fail;

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
        WaitForNestedSceneA,
        WaitForNestedSceneB,
        WaitForGltfScene,
        WaitForFinalRenderBarrier,
        WaitForCleanupRenderBarrier,
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

    bool VerifyNestedTextureIdentities()
    {
        const auto *primitive_a = FindMeshPrimitive(&nested_scene_a_, "Triangle");
        const auto *primitive_b = FindMeshPrimitive(&nested_scene_b_, "Triangle");
        const auto *material_a = primitive_a ? primitive_a->GetMaterial() : nullptr;
        const auto *material_b = primitive_b ? primitive_b->GetMaterial() : nullptr;

        bool success = Expect(material_a != nullptr && material_b != nullptr, "loaded both nested scene materials");
        if (!success)
        {
            return false;
        }

        const auto &texture_a = material_a->GetRawMaterial().base_color_texture;
        const auto &texture_b = material_b->GetRawMaterial().base_color_texture;
        success &= Expect(texture_a != nullptr && texture_b != nullptr, "resolved both nested scene textures");
        if (!success)
        {
            return false;
        }

        success &=
            Expect(texture_a->GetName() == NestedTextureAIdentity && texture_b->GetName() == NestedTextureBIdentity,
                   "nested scenes retain distinct packed-relative identities");
        success &= Expect(IsCompressedFormat(texture_a->GetFormat()) && IsCompressedFormat(texture_b->GetFormat()),
                          "nested scene textures resolve cooked artifacts");

        const Vector3 color_a = texture_a->EnsureDecoded().AccessPixel(0, 0).head<3>();
        const Vector3 color_b = texture_b->EnsureDecoded().AccessPixel(0, 0).head<3>();
        success &= Expect(!color_a.isApprox(color_b), "same-named nested textures retain distinct pixels");
        return success;
    }

    bool VerifyGltfTextureIdentities()
    {
        const auto *primitive = FindMeshPrimitive(&gltf_scene_, "Triangle_0");
        const auto *material = primitive ? primitive->GetMaterial() : nullptr;

        if (!Expect(material != nullptr, "loaded external-image glTF material"))
        {
            return false;
        }

        const auto &raw_material = material->GetRawMaterial();
        const auto &base_color = raw_material.base_color_texture;
        const auto &emissive = raw_material.emissive_texture;
        const auto &metallic_roughness = raw_material.metallic_roughness_texture;
        const auto &normal = raw_material.normal_texture;
        if (!Expect(base_color && emissive && metallic_roughness && normal,
                    "loaded every external-image glTF material texture slot"))
        {
            return false;
        }

        const bool success =
            base_color->GetName() == GltfTextureIdentity && emissive->GetName() == GltfTextureIdentity &&
            metallic_roughness->GetName() == GltfTextureIdentity && normal->GetName() == GltfTextureIdentity &&
            IsCompressedFormat(base_color->GetFormat()) && IsCompressedFormat(emissive->GetFormat()) &&
            IsCompressedFormat(metallic_roughness->GetFormat()) && IsCompressedFormat(normal->GetFormat());
        return Expect(success, "external glTF textures resolve their cooked packed-relative identity");
    }

    void CleanupLoadedScenes()
    {
        gltf_scene_.Cleanup();
        nested_scene_b_.Cleanup();
        nested_scene_a_.Cleanup();
    }

    Result BeginFinalization(bool success, bool verify_gltf)
    {
        final_success_ = success;
        verify_gltf_ = verify_gltf;
        render_barrier_ = TaskManager::RunInRenderThread([] {});
        stage_ = Stage::WaitForFinalRenderBarrier;
        return Result::Pending;
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
    static inline const Path NestedSceneAPath = Path::Resource("tests/nested/a/scene.usda");
    static inline const Path NestedSceneBPath = Path::Resource("tests/nested/b/scene.usda");
    static inline const Path GltfScenePath = Path::Resource("tests/gltf_external/scene.gltf");
    static inline const std::string NestedTextureAIdentity = "tests/nested/a/texture.png";
    static inline const std::string NestedTextureBIdentity = "tests/nested/b/texture.png";
    static inline const std::string GltfTextureIdentity = "tests/gltf_external/texture.png";

    Stage stage_ = Stage::InsertRenderBarrier;
    std::shared_ptr<TaskFuture<>> render_barrier_;
    std::shared_ptr<TaskFuture<bool>> load_task_;
    bool final_success_ = false;
    bool verify_gltf_ = false;
    Scene nested_scene_a_;
    Scene nested_scene_b_;
    Scene gltf_scene_;
};

static TestCaseRegistrar<UsdLoaderSemanticsTest> usd_loader_semantics_test_registrar("usd_loader_semantics");
} // namespace sparkle
