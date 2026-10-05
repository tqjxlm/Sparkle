#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/NativeView.h"
#include "application/RenderFramework.h"
#include "core/Event.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"

#include <atomic>
#include <memory>

namespace sparkle
{
// changes the resolution at runtime once the scene has loaded, then screenshots the converged frame. the renderer
// resizes in place: no renderer is created after the change.
class ResizeTestBase : public TestCase
{
    static constexpr uint32_t SettleFrames = 10;

public:
    Result OnTick(AppFramework &app) override
    {
        if (task_pending_.load(std::memory_order_acquire))
        {
            return Result::Pending;
        }

        auto *framework = app.GetRenderFramework();

        switch (phase_)
        {
        case Phase::WaitLoaded:
            if (!framework->IsSceneFullyLoaded())
            {
                return Result::Pending;
            }
            RunOnRenderThread([this, framework] {
                renderer_created_ = framework->ListenRendererCreatedEvent().Subscribe(
                    [this] { renderers_created_.fetch_add(1, std::memory_order_relaxed); });
            });
            phase_ = Phase::Resize;
            wait_until_frame_ = frame_ + SettleFrames;
            return Result::Pending;

        case Phase::Resize:
            if (frame_ < wait_until_frame_)
            {
                return Result::Pending;
            }
            Resize(app);
            phase_ = Phase::WaitConverged;
            wait_until_frame_ = frame_ + SettleFrames;
            return Result::Pending;

        case Phase::WaitConverged:
            if (frame_ < wait_until_frame_ || !framework->IsReadyForAutoScreenshot())
            {
                return Result::Pending;
            }
            screenshot_ = framework->RequestTakeScreenshot("screenshot");
            phase_ = Phase::Screenshot;
            return Result::Pending;

        case Phase::Screenshot:
            if (!screenshot_->IsCompleted())
            {
                return Result::Pending;
            }
            RunOnRenderThread([this] { renderer_created_.reset(); });
            phase_ = Phase::Done;
            return Result::Pending;

        case Phase::Done:
            Expect(renderers_created_.load(std::memory_order_relaxed) == 0,
                   "the resolution change resizes the renderer instead of creating one");
            return HasFailed() ? Result::Fail : Result::Pass;

        default:
            return Result::Fail;
        }
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return 3000;
    }

protected:
    virtual void Resize(AppFramework &app) = 0;

private:
    enum class Phase : uint8_t
    {
        WaitLoaded,
        Resize,
        WaitConverged,
        Screenshot,
        Done,
    };

    template <typename Task> void RunOnRenderThread(Task &&task)
    {
        task_pending_.store(true, std::memory_order_release);
        TaskManager::RunInRenderThread([this, render_task = std::forward<Task>(task)] {
            render_task();
            task_pending_.store(false, std::memory_order_release);
        });
    }

    Phase phase_ = Phase::WaitLoaded;
    uint32_t wait_until_frame_ = 0;
    std::shared_ptr<ScreenshotRequest> screenshot_;

    std::atomic<bool> task_pending_{false};

    // only accessed from the render thread
    std::unique_ptr<EventSubscription> renderer_created_;
    std::atomic<uint32_t> renderers_created_{0};
};

// a desktop window resize to 1280x720 at render_scale 1, so the frame matches the static ground truth
class ResizeTest : public ResizeTestBase
{
    static constexpr uint32_t Width = 1280;
    static constexpr uint32_t Height = 720;

protected:
    void Resize(AppFramework &app) override
    {
        const auto scale = app.GetNativeView()->GetWindowScale();
        app.FrameBufferResizeCallback(static_cast<int>(static_cast<float>(Width) * scale.x()),
                                      static_cast<int>(static_cast<float>(Height) * scale.y()));
        EnforceConfig("render_scale", 1.f);

        const auto &config = app.GetRenderConfig();
        Log(Info, "{}: resized the frame buffer, output [{}, {}]", GetName(), config.image_width, config.image_height);
        Expect(config.image_width == Width && config.image_height == Height,
               "the output resolution follows the frame buffer");
    }
};

class RenderScaleChangeTest : public ResizeTestBase
{
protected:
    void Resize(AppFramework & /*app*/) override
    {
        EnforceConfig("render_scale", 0.5f);
        Log(Info, "{}: render_scale 0.5", GetName());
    }
};

static TestCaseRegistrar<ResizeTest> resize_test_registrar("resize");
static TestCaseRegistrar<RenderScaleChangeTest> render_scale_change_test_registrar("render_scale_change");
} // namespace sparkle
