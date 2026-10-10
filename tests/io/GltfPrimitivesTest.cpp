#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/FileManager.h"
#include "core/task/TaskManager.h"
#include "io/Mesh.h"
#include "scene/FindMeshPrimitive.h"
#include "scene/Scene.h"
#include "scene/SceneManager.h"
#include "scene/component/primitive/MeshPrimitive.h"
#include "scene/material/MaterialManager.h"

#include <array>
#include <format>
#include <string_view>

namespace sparkle
{
class GltfPrimitivesTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        auto *scene = app.GetScene();

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
            load_task_ = SceneManager::LoadScene(scene, ScenePath, false, false);
            stage_ = Stage::WaitForScene;
            return Result::Pending;

        case Stage::WaitForScene:
            if (!load_task_->IsReady() || scene->HasPendingAsyncTasks())
            {
                return Result::Pending;
            }
            CleanupFixtures();
            Expect(load_task_->Get(), "loaded the glTF scene");
            VerifyPrimitives(scene);
            return HasFailed() ? Result::Fail : Result::Pass;

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
    };

    // six vertices in the z = 0 plane: v0 (0,0) v1 (1,0) v2 (1,1) v3 (0,1) v4 (-1,1) v5 (-1,0)
    static constexpr std::array<float, 18> Positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, -1, 1, 0, -1, 0, 0};
    static constexpr std::array<float, 18> Normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    static constexpr std::array<float, 24> Tangents = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1,
                                                       1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
    static constexpr std::array<double, 18> DoublePositions = {0, 0, 0.5, 2,  0, 0.5, 2,  2, 0.5,
                                                               0, 2, 0.5, -2, 2, 0.5, -2, 0, 0.5};
    // fan around v0, and a zigzag strip whose triangles all face +z
    static constexpr std::array<uint8_t, 11> Indices = {0, 1, 2, 3, 4, 4, 5, 3, 0, 2, 1};

    static constexpr size_t BufferSize =
        sizeof(Positions) + sizeof(Normals) + sizeof(Tangents) + sizeof(DoublePositions) + sizeof(Indices);
    static_assert(BufferSize == 395, "byte offsets in SceneData follow this layout");

    static constexpr std::string_view SceneData = R"GLTF({
    "asset": {"version": "2.0"},
    "scene": 0,
    "scenes": [{"nodes": [0, 1, 2, 3]}],
    "nodes": [{"mesh": 0}, {"mesh": 1}, {"mesh": 2}, {"mesh": 3}],
    "meshes": [
        {"name": "Fan", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TANGENT": 2}, "indices": 4, "mode": 6, "material": 0}]},
        {"name": "Strip", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TANGENT": 2}, "indices": 5, "mode": 5, "material": 0}]},
        {"name": "NonIndexed", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TANGENT": 2}, "mode": 4}]},
        {"name": "DoublePositions", "primitives": [{"attributes": {"POSITION": 3, "NORMAL": 1, "TANGENT": 2}, "mode": 4, "material": 0}]}
    ],
    "materials": [{"name": "Plain"}],
    "buffers": [{"uri": "scene.bin", "byteLength": 395}],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 72},
        {"buffer": 0, "byteOffset": 72, "byteLength": 72},
        {"buffer": 0, "byteOffset": 144, "byteLength": 96},
        {"buffer": 0, "byteOffset": 240, "byteLength": 144},
        {"buffer": 0, "byteOffset": 384, "byteLength": 11}
    ],
    "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 6, "type": "VEC3", "min": [-1, 0, 0], "max": [1, 1, 0]},
        {"bufferView": 1, "componentType": 5126, "count": 6, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": 6, "type": "VEC4"},
        {"bufferView": 3, "componentType": 5130, "count": 6, "type": "VEC3", "min": [-2, 0, 0.5], "max": [2, 2, 0.5]},
        {"bufferView": 4, "byteOffset": 0, "componentType": 5121, "count": 5, "type": "SCALAR"},
        {"bufferView": 4, "byteOffset": 5, "componentType": 5121, "count": 6, "type": "SCALAR"}
    ]
})GLTF";

    template <typename T, size_t N> static void Append(std::vector<char> &buffer, const std::array<T, N> &values)
    {
        const auto *bytes = reinterpret_cast<const char *>(values.data());
        buffer.insert(buffer.end(), bytes, bytes + sizeof(values));
    }

    static bool WriteFixtures()
    {
        CleanupFixtures();

        std::vector<char> buffer;
        buffer.reserve(BufferSize);
        Append(buffer, Positions);
        Append(buffer, Normals);
        Append(buffer, Tangents);
        Append(buffer, DoublePositions);
        Append(buffer, Indices);

        auto *file_manager = FileManager::GetNativeFileManager();
        bool success = !file_manager->Write(BufferPath, buffer).empty();
        success &= !file_manager->Write(ScenePath, SceneData.data(), SceneData.size()).empty();
        return success;
    }

    static void CleanupFixtures()
    {
        auto *file_manager = FileManager::GetNativeFileManager();
        for (const auto &path : {ScenePath, BufferPath})
        {
            if (file_manager->Exists(path))
            {
                file_manager->Remove(path);
            }
        }
    }

    void ExpectIndices(Scene *scene, std::string_view name, const std::vector<unsigned int> &expected_indices)
    {
        const auto *primitive = FindMeshPrimitive(scene, name);
        Expect(primitive != nullptr && primitive->GetMeshResource()->indices == expected_indices,
               std::format("{} assembles the triangles its glTF topology defines", name));
    }

    void VerifyPrimitives(Scene *scene)
    {
        ExpectIndices(scene, "Fan_0", {1, 2, 0, 2, 3, 0, 3, 4, 0});
        ExpectIndices(scene, "Strip_0", {4, 5, 3, 5, 0, 3, 3, 0, 2, 0, 1, 2});
        ExpectIndices(scene, "NonIndexed_0", {0, 1, 2, 3, 4, 5});
        ExpectIndices(scene, "DoublePositions_0", {0, 1, 2, 3, 4, 5});

        const auto *non_indexed = FindMeshPrimitive(scene, "NonIndexed_0");
        Expect(non_indexed != nullptr &&
                   non_indexed->GetMaterial() == MaterialManager::Instance().GetDefaultMaterial().get(),
               "a primitive without a material uses the default material");

        const auto *double_positions = FindMeshPrimitive(scene, "DoublePositions_0");
        std::vector<Vector3> expected_vertices;
        for (size_t i = 0; i < DoublePositions.size(); i += 3)
        {
            expected_vertices.emplace_back(
                Vector3d(DoublePositions[i], DoublePositions[i + 1], DoublePositions[i + 2]).cast<Scalar>());
        }
        Expect(double_positions != nullptr && double_positions->GetMeshResource()->vertices == expected_vertices,
               "double-precision positions are converted");
    }

    static inline const Path ScenePath = Path::Internal("tests/gltf_primitives/scene.gltf");
    static inline const Path BufferPath = Path::Internal("tests/gltf_primitives/scene.bin");

    Stage stage_ = Stage::InsertRenderBarrier;
    std::shared_ptr<TaskFuture<>> render_barrier_;
    std::shared_ptr<TaskFuture<bool>> load_task_;
};

static TestCaseRegistrar<GltfPrimitivesTest> gltf_primitives_test_registrar("gltf_primitives");
} // namespace sparkle
