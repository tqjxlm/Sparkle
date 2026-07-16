#include "application/AppFramework.h"
#include "application/TestCase.h"
#include "core/Logger.h"
#include "core/math/Intersection.h"
#include "core/task/TaskManager.h"
#include "io/Mesh.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "renderer/proxy/MeshRenderProxy.h"
#include "renderer/proxy/PrimitiveRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "rhi/RHI.h"
#include "rhi/RHIRayTracing.h"

#include <cmath>

namespace sparkle
{
namespace
{
class DynamicPrimitive final : public PrimitiveRenderProxy
{
public:
    DynamicPrimitive() : PrimitiveRenderProxy("DynamicPrimitive", AABB(Zeros, Ones * 0.5f))
    {
    }

    bool Intersect(const Ray &ray, IntersectionCandidate &candidate) const override
    {
        const auto local_ray = ray.TransformedBy(GetTransform().GetInverse());
        if (std::abs(local_ray.Origin().x()) > 0.5f || std::abs(local_ray.Origin().z()) > 0.5f ||
            local_ray.Direction().y() <= 0.f)
        {
            return false;
        }

        const auto t = -local_ray.Origin().y() / local_ray.Direction().y();
        if (t <= 0.f)
        {
            return false;
        }

        candidate.t = t;
        return true;
    }

    bool IntersectAnyHit(const Ray &ray, IntersectionCandidate &candidate) const override
    {
        return Intersect(ray, candidate);
    }

    void BuildBVH() override
    {
        build_count_++;
    }

    [[nodiscard]] uint32_t GetBuildCount() const
    {
        return build_count_;
    }

private:
    uint32_t build_count_ = 0;
};

bool TraceAt(const SceneRenderProxy &scene, float x)
{
    Ray ray;
    ray.Reset(Vector3(x, -3.f, 0.f), Vector3::UnitY());

    Intersection intersection;
    scene.Intersect<true>(ray, intersection);
    return intersection.IsHit();
}
} // namespace

class DynamicPrimitiveBvhTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        if (!result_)
        {
            auto config = app.GetRenderConfig();
            config.pipeline = RenderConfig::Pipeline::Cpu;
            result_ = TaskManager::RunInRenderThread(
                [rhi = app.GetRHI(), config, test_name = GetName()] { return Run(rhi, config, test_name); });
            return Result::Pending;
        }

        if (!result_->IsReady())
        {
            return Result::Pending;
        }

        return result_->Get() ? Result::Pass : Result::Fail;
    }

private:
    static bool Run(RHIContext *rhi, const RenderConfig &config, const std::string &test_name)
    {
        bool passed = true;
        auto expect = [&](bool condition, std::string_view behavior) {
            if (condition)
            {
                Log(Info, "{}: OK - {}", test_name, behavior);
            }
            else
            {
                Log(Error, "{}: FAILED - {}", test_name, behavior);
            }
            passed &= condition;
        };

        CameraRenderProxy camera;
        SceneRenderProxy scene;
        scene.SetCamera(&camera);

        auto primitive_owner = std::make_unique<DynamicPrimitive>();
        auto *primitive = primitive_owner.get();
        scene.AddRenderProxy(std::move(primitive_owner));
        scene.InitRenderResources(rhi, config);

        camera.ClearPixels();
        scene.Update(rhi, camera, config);

        const auto &new_changes = scene.GetPrimitiveChangeList();
        expect(new_changes.size() == 1 && new_changes.front().type == SceneRenderProxy::PrimitiveChangeType::New &&
                   new_changes.front().primitive == primitive,
               "an uninitialized primitive is reported only as New");
        expect(camera.NeedClear(), "a new primitive invalidates camera accumulation");
        expect(TraceAt(scene, 0.f), "the initial CPU TLAS contains the primitive");
        scene.EndUpdate(rhi);

        camera.ClearPixels();
        primitive->UpdateTransform(Transform(Vector3(4.f, 0.f, 0.f), Zeros, Ones));
        scene.Update(rhi, camera, config);

        const auto &update_changes = scene.GetPrimitiveChangeList();
        expect(update_changes.size() == 1 &&
                   update_changes.front().type == SceneRenderProxy::PrimitiveChangeType::Update &&
                   update_changes.front().primitive == primitive &&
                   update_changes.front().to_id == primitive->GetPrimitiveIndex(),
               "an initialized transformed primitive is reported as Update");
        expect(camera.NeedClear(), "a primitive transform invalidates camera accumulation");
        expect(!TraceAt(scene, 0.f), "the rebuilt CPU TLAS no longer intersects the old transform");
        expect(TraceAt(scene, 4.f), "the rebuilt CPU TLAS intersects the new transform");
        expect(primitive->GetBuildCount() == 1, "a transform rebuilds only the TLAS");
        scene.EndUpdate(rhi);

        camera.ClearPixels();
        scene.Update(rhi, camera, config);
        expect(scene.GetPrimitiveChangeList().empty(), "an unchanged primitive emits no repeated Update");
        expect(!camera.NeedClear(), "an unchanged primitive preserves camera accumulation");
        expect(primitive->GetBuildCount() == 1, "an unchanged primitive does not rebuild its local BVH");
        scene.EndUpdate(rhi);

        if (rhi->SupportsHardwareRayTracing())
        {
            auto gpu_config = config;
            gpu_config.pipeline = RenderConfig::Pipeline::Gpu;

            auto mesh_data = Mesh::GetUnitCube();
            MeshRenderProxy mesh(mesh_data, "DynamicPrimitiveMesh", AABB(mesh_data->center, mesh_data->extent));

            rhi->BeginCommandBuffer();
            mesh.InitRenderResources(rhi, gpu_config);
            mesh.Update(rhi, camera, gpu_config);

            auto tlas = rhi->CreateTLAS("DynamicPrimitiveTLAS");
            tlas->SetBLAS(mesh.GetAccelerationStructure().get(), 0);
            tlas->Build();
            rhi->SubmitCommandBuffer();
            rhi->WaitForDeviceIdle();

            rhi->BeginCommandBuffer();
            const Transform moved_transform(Vector3(4.f, 0.f, 0.f), Zeros, Ones);
            mesh.UpdateTransform(moved_transform);
            mesh.Update(rhi, camera, gpu_config);
            expect(mesh.GetAccelerationStructure()->GetTransform().isApprox(moved_transform.GetMatrix()),
                   "the mesh transform reaches its BLAS instance");
            tlas->Update({0});
            rhi->SubmitCommandBuffer();
            rhi->WaitForDeviceIdle();

            expect(true, "the hardware TLAS accepts a transformed mesh instance");
        }
        else
        {
            Log(Info, "{}: SKIPPED - hardware ray tracing is unavailable", test_name);
        }

        return passed;
    }

    std::shared_ptr<TaskFuture<bool>> result_;
};

static TestCaseRegistrar<DynamicPrimitiveBvhTest> dynamic_primitive_bvh_test_registrar("dynamic_primitive_bvh");
} // namespace sparkle
