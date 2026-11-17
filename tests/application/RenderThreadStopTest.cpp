#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/task/TaskManager.h"
#include "scene/Scene.h"
#include "scene/SceneNode.h"
#include "scene/component/primitive/SpherePrimitive.h"
#include "scene/material/LambertianMaterial.h"
#include "scene/material/MaterialManager.h"

#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

namespace sparkle
{
namespace
{
// a material that reports at its destruction whether the render thread released its render proxy before
class ProbeMaterial : public LambertianMaterial
{
public:
    explicit ProbeMaterial(std::function<void(bool)> on_destroy)
        : LambertianMaterial(MaterialResource{.name = "ProbeMaterial"}), on_destroy_(std::move(on_destroy))
    {
    }

    ~ProbeMaterial() override
    {
        on_destroy_(GetRenderProxy() == nullptr);
    }

private:
    std::function<void(bool)> on_destroy_;
};
} // namespace

// switches a sphere to another material in the last frame while the render thread is still busy with the frame before,
// so the task that removes the previous material from the render thread is still queued when the render thread stops.
// that task holds the last reference to the material, which checks at its destruction, after the verdict, that the
// render thread released its render proxy first. the evaluator reads the check from the log.
class RenderThreadStopTest : public TestCase
{
    // long enough for the main thread to queue the last frame and stop the render thread while it stalls
    static constexpr auto RenderThreadStall = std::chrono::milliseconds(500);

public:
    Result OnTick(AppFramework &app) override
    {
        if (render_barrier_ && !render_barrier_->IsReady())
        {
            return Result::Pending;
        }

        switch (phase_)
        {
        case Phase::Attach:
            Attach(app.GetScene());
            render_barrier_ = TaskManager::RunInRenderThread([] {});
            phase_ = Phase::Stall;
            return Result::Pending;

        case Phase::Stall:
            Expect(material_->GetRenderProxy() != nullptr,
                   "the render thread created the render proxy of the material");
            TaskManager::RunInRenderThread([] { std::this_thread::sleep_for(RenderThreadStall); });
            phase_ = Phase::Remove;
            return Result::Pending;

        case Phase::Remove:
            // the sphere holds the only reference to the material, which passes to the task that removes it from the
            // render thread
            sphere_->SetMaterial(MaterialManager::Instance().GetDefaultMaterial());
            return HasFailed() ? Result::Fail : Result::Pass;

        default:
            return Result::Fail;
        }
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return 600;
    }

private:
    enum class Phase : uint8_t
    {
        Attach,
        Stall,
        Remove,
    };

    void Attach(Scene *scene)
    {
        auto material = std::make_shared<ProbeMaterial>([this](bool released) {
            Expect(released, "the render thread released the render proxy of the material before it was destroyed");
        });
        material_ = material.get();

        auto sphere = std::make_shared<SpherePrimitive>();
        sphere->SetMaterial(material);
        sphere_ = sphere.get();

        auto node = std::make_shared<SceneNode>(scene, "RenderThreadStopSphere");
        node->AddComponent(sphere);
        scene->GetRootNode()->AddChild(node);
    }

    Phase phase_ = Phase::Attach;

    std::shared_ptr<TaskFuture<>> render_barrier_;

    // owned by the scene, the material through the sphere
    SpherePrimitive *sphere_ = nullptr;
    Material *material_ = nullptr;
};

static TestCaseRegistrar<RenderThreadStopTest> render_thread_stop_test_registrar("render_thread_stop");
} // namespace sparkle
