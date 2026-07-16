#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "core/math/Utilities.h"
#include "core/task/TaskManager.h"
#include "renderer/BindlessManager.h"
#include "renderer/proxy/PbrMaterialRenderProxy.h"
#include "renderer/proxy/PrimitiveRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "rhi/RHI.h"

#include <algorithm>
#include <atomic>
#include <deque>

namespace sparkle
{
class BindlessResizePrimitive final : public PrimitiveRenderProxy
{
public:
    BindlessResizePrimitive() : PrimitiveRenderProxy("BindlessResizePrimitive", AABB(Zeros, Zeros))
    {
    }

    bool Intersect(const Ray &, IntersectionCandidate &) const override
    {
        return false;
    }

    bool IntersectAnyHit(const Ray &, IntersectionCandidate &) const override
    {
        return false;
    }
};

class BindlessResizeTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        if (task_pending_.load(std::memory_order_acquire))
        {
            return Result::Pending;
        }

        if (started_)
        {
            return failed_.load(std::memory_order_acquire) ? Result::Fail : Result::Pass;
        }

        started_ = true;
        task_pending_.store(true, std::memory_order_release);

        auto *rhi = app.GetRHI();
        TaskManager::RunInRenderThread([this, rhi] {
            rhi->BeginCommandBuffer();
            VerifyDenseResize(rhi);
            VerifySparseResize(rhi);
            rhi->SubmitCommandBuffer();

            task_pending_.store(false, std::memory_order_release);
        });

        return Result::Pending;
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return 1000;
    }

private:
    static constexpr size_t BaseBufferSize = 1024;
    static constexpr size_t DenseEntryCount = BaseBufferSize * 2 + 1;
    static constexpr size_t SparseEntryCount = BaseBufferSize + 1;
    static constexpr size_t SparseHole = BaseBufferSize / 2;

    static MaterialRenderProxy *AddMaterial(SceneRenderProxy &scene, std::deque<MaterialResource> &resources,
                                            size_t index)
    {
        MaterialResource resource;
        resource.metallic = static_cast<float>(index + 1);
        resource.name = std::format("BindlessResizeMaterial{}", index);
        resources.push_back(std::move(resource));

        return scene.AddMaterial(std::make_unique<PbrMaterialRenderProxy>(resources.back()));
    }

    void VerifyDenseResize(RHIContext *rhi)
    {
        std::deque<MaterialResource> resources;
        SceneRenderProxy scene;
        BindlessManager manager(&scene);
        manager.InitRenderResources(rhi);

        Expect(manager.GetMaterialParameterBuffer()->GetSize() ==
                   sizeof(MaterialRenderProxy::MaterialRenderData) * BaseBufferSize,
               "material buffer uses the shader data stride");

        for (size_t i = 0; i < DenseEntryCount; i++)
        {
            auto *material = AddMaterial(scene, resources, i);
            auto primitive = std::make_unique<BindlessResizePrimitive>();
            primitive->SetMaterialRenderProxy(material);
            scene.AddRenderProxy(std::move(primitive));
        }

        manager.UpdateFrameData(rhi);

        const auto &material_id_buffer = manager.GetMaterialIdBuffer();
        const auto &material_parameter_buffer = manager.GetMaterialParameterBuffer();

        Expect(manager.IsBufferDirty(), "resized buffers request descriptor rebinding");
        Expect(material_id_buffer->GetSize() >= DenseEntryCount * sizeof(uint32_t),
               "primitive metadata grows across multiple capacity steps");
        Expect(material_parameter_buffer->GetSize() >=
                   DenseEntryCount * sizeof(MaterialRenderProxy::MaterialRenderData),
               "material metadata grows across multiple capacity steps");

        const auto *material_ids = static_cast<const uint32_t *>(material_id_buffer->Lock());
        Expect(material_ids[0] == 0 && material_ids[BaseBufferSize] == BaseBufferSize &&
                   material_ids[DenseEntryCount - 1] == DenseEntryCount - 1,
               "primitive metadata preserves boundary material IDs");
        const auto material_id_count = material_id_buffer->GetSize() / sizeof(*material_ids);
        Expect(std::all_of(material_ids + DenseEntryCount, material_ids + material_id_count,
                           [](uint32_t value) { return value == 0; }),
               "primitive metadata clears unused capacity");
        material_id_buffer->UnLock();

        const auto *material_data =
            static_cast<const MaterialRenderProxy::MaterialRenderData *>(material_parameter_buffer->Lock());
        Expect(utilities::NearlyEqual(material_data[0].metallic, 1.0f) &&
                   utilities::NearlyEqual(material_data[BaseBufferSize].metallic,
                                          static_cast<float>(BaseBufferSize + 1)) &&
                   utilities::NearlyEqual(material_data[DenseEntryCount - 1].metallic,
                                          static_cast<float>(DenseEntryCount)),
               "material metadata preserves boundary values");
        const auto *material_bytes = reinterpret_cast<const uint8_t *>(material_data);
        Expect(std::all_of(material_bytes + DenseEntryCount * sizeof(*material_data),
                           material_bytes + material_parameter_buffer->GetSize(),
                           [](uint8_t value) { return value == 0; }),
               "material metadata clears unused capacity");
        material_parameter_buffer->UnLock();
    }

    void VerifySparseResize(RHIContext *rhi)
    {
        std::deque<MaterialResource> resources;
        SceneRenderProxy scene;
        BindlessManager manager(&scene);
        manager.InitRenderResources(rhi);

        MaterialRenderProxy *material_to_remove = nullptr;
        for (size_t i = 0; i < SparseEntryCount; i++)
        {
            auto *material = AddMaterial(scene, resources, i);
            if (i == SparseHole)
            {
                material_to_remove = material;
            }
        }
        scene.RemoveMaterial(material_to_remove);

        manager.UpdateFrameData(rhi);

        const auto &buffer = manager.GetMaterialParameterBuffer();
        const auto *material_data = static_cast<const MaterialRenderProxy::MaterialRenderData *>(buffer->Lock());
        const auto *material_bytes = reinterpret_cast<const uint8_t *>(material_data);
        const auto *hole_begin = material_bytes + SparseHole * sizeof(*material_data);

        Expect(std::all_of(hole_begin, hole_begin + sizeof(*material_data), [](uint8_t value) { return value == 0; }),
               "removed material slots remain zeroed");
        Expect(
            utilities::NearlyEqual(material_data[SparseEntryCount - 1].metallic, static_cast<float>(SparseEntryCount)),
            "sparse material upload preserves later indices");
        Expect(std::all_of(material_bytes + SparseEntryCount * sizeof(*material_data),
                           material_bytes + buffer->GetSize(), [](uint8_t value) { return value == 0; }),
               "sparse material upload clears unused capacity");
        buffer->UnLock();
    }

    void Expect(bool condition, const std::string &what)
    {
        if (condition)
        {
            Log(Info, "{}: OK - {}", GetName(), what);
        }
        else
        {
            Log(Error, "{}: FAILED - {}", GetName(), what);
            failed_.store(true, std::memory_order_release);
        }
    }

    bool started_ = false;
    std::atomic<bool> task_pending_{false};
    std::atomic<bool> failed_{false};
};

static TestCaseRegistrar<BindlessResizeTest> bindless_resize_test_registrar("bindless_resize");
} // namespace sparkle
