#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "application/TestCase.h"
#include "core/task/TaskManager.h"
#include "renderer/proxy/PrimitiveRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "scene/Scene.h"
#include "scene/SceneNode.h"
#include "scene/component/Component.h"
#include "scene/component/primitive/SpherePrimitive.h"
#include "scene/material/LambertianMaterial.h"
#include "scene/material/MaterialManager.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <tuple>

namespace sparkle
{
// adds and removes spheres within one render frame: a new sphere changes its material, which removes its first render
// proxy again and swaps a second new sphere into its slot, and a sphere attached before is destroyed, which swaps the
// new render proxy of the first sphere into its slot. the frame is screenshotted, then the same spheres are removed
// and added again over separate frames and screenshotted as the reference. tests/scene/primitive_churn_test.py
// requires both screenshots to match, so a mesh pass that drew a slot with the wrong mesh, or crashed, fails.
class PrimitiveChurnTest : public TestCase
{
    // a few frames for the render passes to consume the changes before a screenshot
    static constexpr uint32_t SettleFrames = 10;

    // free spots between the objects of the TestScene, in view of its camera
    static constexpr float SphereRadius = 1.5f;
    inline static const Vector3 LeftPosition{-4.f, 0.f, SphereRadius};
    inline static const Vector3 RightPosition{4.f, 0.f, SphereRadius};
    inline static const Vector3 RemovedPosition{0.f, -4.f, SphereRadius};

public:
    Result OnTick(AppFramework &app) override
    {
        if (task_pending_.load(std::memory_order_acquire))
        {
            return Result::Pending;
        }

        auto *scene = app.GetScene();
        auto *framework = app.GetRenderFramework();

        switch (phase_)
        {
        case Phase::WaitLoaded:
            if (!framework->IsReadyForAutoScreenshot())
            {
                return Result::Pending;
            }
            CreateMaterials();
            std::tie(removed_, std::ignore) = AddSphere(scene, "ChurnRemoved", RemovedPosition, removed_material_);
            Settle(Phase::Churn);
            return Result::Pending;

        case Phase::Churn:
            Churn(scene);
            Settle(Phase::ChurnScreenshot);
            return Result::Pending;

        case Phase::ChurnScreenshot:
            screenshot_ = framework->RequestTakeScreenshot("primitive_churn");
            phase_ = Phase::RemoveChurned;
            return Result::Pending;

        case Phase::RemoveChurned:
            if (!screenshot_->IsCompleted())
            {
                return Result::Pending;
            }
            scene->GetRootNode()->RemoveChild(left_);
            scene->GetRootNode()->RemoveChild(right_);
            Settle(Phase::Rebuild);
            return Result::Pending;

        case Phase::Rebuild:
            std::tie(left_, std::ignore) = AddSphere(scene, "ChurnLeft", LeftPosition, left_material_);
            std::tie(right_, std::ignore) = AddSphere(scene, "ChurnRight", RightPosition, right_material_);
            Settle(Phase::ReferenceScreenshot);
            return Result::Pending;

        case Phase::ReferenceScreenshot:
            screenshot_ = framework->RequestTakeScreenshot("primitive_churn_reference");
            phase_ = Phase::Done;
            return Result::Pending;

        case Phase::Done:
            if (!screenshot_->IsCompleted())
            {
                return Result::Pending;
            }
            return HasFailed() ? Result::Fail : Result::Pass;

        case Phase::Settle:
            if (frame_ >= settle_until_frame_ && framework->IsReadyForAutoScreenshot())
            {
                phase_ = next_phase_;
            }
            return Result::Pending;

        default:
            return Result::Fail;
        }
    }

private:
    enum class Phase : uint8_t
    {
        WaitLoaded,
        Churn,
        ChurnScreenshot,
        RemoveChurned,
        Rebuild,
        ReferenceScreenshot,
        Done,
        Settle,
    };

    void Settle(Phase next_phase)
    {
        phase_ = Phase::Settle;
        next_phase_ = next_phase;
        settle_until_frame_ = frame_ + SettleFrames;
    }

    void CreateMaterials()
    {
        auto &material_manager = MaterialManager::Instance();
        first_material_ = material_manager.CreateMaterial<LambertianMaterial>({.name = "ChurnFirst"});
        left_material_ = material_manager.CreateMaterial<LambertianMaterial>(
            {.base_color = {0.8f, 0.1f, 0.1f}, .name = "ChurnLeft"});
        right_material_ = material_manager.CreateMaterial<LambertianMaterial>(
            {.base_color = {0.1f, 0.8f, 0.1f}, .name = "ChurnRight"});
        removed_material_ = material_manager.CreateMaterial<LambertianMaterial>(
            {.base_color = {0.1f, 0.1f, 0.8f}, .name = "ChurnRemoved"});
    }

    // every render command queued here reaches the render thread in one batch, which one render frame consumes
    void Churn(Scene *scene)
    {
        // RemoveChild hands the render thread what is queued so far; the sphere's render proxy goes when the node dies
        auto removed = FindChild(scene->GetRootNode(), removed_);
        scene->GetRootNode()->RemoveChild(removed_);

        SpherePrimitive *left_sphere = nullptr;
        std::tie(left_, left_sphere) = AddSphere(scene, "ChurnLeft", LeftPosition, first_material_);
        std::tie(right_, std::ignore) = AddSphere(scene, "ChurnRight", RightPosition, right_material_);
        left_sphere->SetMaterial(left_material_);

        removed.reset();

        RunOnRenderThread([this, scene_proxy = scene->GetRenderProxy()] {
            using ChangeType = SceneRenderProxy::PrimitiveChangeType;
            const auto &changes = scene_proxy->GetPrimitiveChangeList();
            const auto added_this_frame = [&changes](const PrimitiveRenderProxy *primitive) {
                return std::ranges::any_of(changes, [primitive](const SceneRenderProxy::PrimitiveChange &change) {
                    return change.type == ChangeType::New && change.primitive == primitive;
                });
            };

            Expect(std::ranges::any_of(changes,
                                       [](const SceneRenderProxy::PrimitiveChange &change) {
                                           return change.type == ChangeType::New &&
                                                  change.primitive->GetPrimitiveIndex() == UINT_MAX;
                                       }),
                   "one render frame adds a primitive and removes it again");
            Expect(std::ranges::any_of(changes,
                                       [&added_this_frame](const SceneRenderProxy::PrimitiveChange &change) {
                                           return change.type == ChangeType::Move && added_this_frame(change.primitive);
                                       }),
                   "a removal within the render frame moves a primitive the frame added");
        });
    }

    // attaches a sphere and gives it a material, which queues the render command that creates its render proxy
    static std::tuple<SceneNode *, SpherePrimitive *> AddSphere(Scene *scene, const std::string &name,
                                                                const Vector3 &position,
                                                                const std::shared_ptr<Material> &material)
    {
        auto [node, sphere] = MakeNodeWithComponent<SpherePrimitive>(scene, scene->GetRootNode(), name);
        node->SetTransform(position, Zeros, Ones * SphereRadius);
        sphere->SetMaterial(material);
        return {node.get(), sphere.get()};
    }

    static std::shared_ptr<SceneNode> FindChild(const SceneNode *parent, const SceneNode *child)
    {
        return *std::ranges::find_if(parent->GetChildren(),
                                     [child](const std::shared_ptr<SceneNode> &node) { return node.get() == child; });
    }

    template <typename Task> void RunOnRenderThread(Task &&task)
    {
        task_pending_.store(true, std::memory_order_release);
        TaskManager::RunInRenderThread([this, render_task = std::forward<Task>(task)] {
            render_task();
            task_pending_.store(false, std::memory_order_release);
        });
    }

    Phase phase_ = Phase::WaitLoaded;
    Phase next_phase_ = Phase::WaitLoaded;
    uint32_t settle_until_frame_ = 0;

    std::shared_ptr<Material> first_material_;
    std::shared_ptr<Material> left_material_;
    std::shared_ptr<Material> right_material_;
    std::shared_ptr<Material> removed_material_;

    // owned by the scene, which dies before the test case: a node the test case kept would outlive it
    SceneNode *removed_ = nullptr;
    SceneNode *left_ = nullptr;
    SceneNode *right_ = nullptr;

    std::shared_ptr<ScreenshotRequest> screenshot_;

    std::atomic<bool> task_pending_{false};
};

static TestCaseRegistrar<PrimitiveChurnTest> primitive_churn_test_registrar("primitive_churn");
} // namespace sparkle
