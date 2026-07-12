#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "application/TestCase.h"
#include "core/Logger.h"
#include "core/task/TaskFuture.h"
#include "io/scene/USDExporter.h"
#include "scene/Scene.h"
#include "scene/SceneManager.h"

namespace sparkle
{
// exports the loaded scene to USD, loads the exported file back as the active scene, and takes a
// screenshot of both. tests/usd/usd_roundtrip_test.py drives this test and compares the screenshots.
class UsdRoundTripTest : public TestCase
{
public:
    static constexpr const char *ExportedScenePath = "usd_export/scene.usda";

    // give the reimported scene some frames to settle (proxy rebuilds, accumulation restart)
    // before trusting the renderer's screenshot readiness again.
    static constexpr uint32_t ReloadSettleFrames = 60;

    Result OnTick(AppFramework &app) override
    {
        auto *render_framework = app.GetRenderFramework();

        switch (stage_)
        {
        case Stage::WaitOriginalRender:
            if (render_framework->IsReadyForAutoScreenshot())
            {
                screenshot_ = render_framework->RequestTakeScreenshot("usd_round_trip_original");
                stage_ = Stage::WaitOriginalScreenshot;
            }
            break;

        case Stage::WaitOriginalScreenshot:
            if (screenshot_->IsCompleted())
            {
                if (!USDExporter::Export(app.GetScene(), Path::Internal(ExportedScenePath)))
                {
                    Log(Error, "UsdRoundTripTest: export failed");
                    return Result::Fail;
                }

                // the exported file carries its own sky, lights and camera. no defaults wanted.
                reload_task_ = SceneManager::LoadScene(app.GetScene(), Path::Internal(ExportedScenePath), false, false);
                stage_ = Stage::WaitReload;
            }
            break;

        case Stage::WaitReload:
            if (reload_task_->IsReady() && !app.GetScene()->HasPendingAsyncTasks())
            {
                reload_task_.reset();
                settle_frame_ = frame_ + ReloadSettleFrames;
                stage_ = Stage::WaitReimportedRender;
            }
            break;

        case Stage::WaitReimportedRender:
            if (frame_ >= settle_frame_ && render_framework->IsReadyForAutoScreenshot())
            {
                screenshot_ = render_framework->RequestTakeScreenshot("usd_round_trip_reimported");
                stage_ = Stage::WaitReimportedScreenshot;
            }
            break;

        case Stage::WaitReimportedScreenshot:
            if (screenshot_->IsCompleted())
            {
                return Result::Pass;
            }
            break;

        default:
            break;
        }

        return Result::Pending;
    }

private:
    enum class Stage : uint8_t
    {
        WaitOriginalRender,
        WaitOriginalScreenshot,
        WaitReload,
        WaitReimportedRender,
        WaitReimportedScreenshot,
    };

    Stage stage_ = Stage::WaitOriginalRender;

    std::shared_ptr<ScreenshotRequest> screenshot_;
    std::shared_ptr<TaskFuture<void>> reload_task_;

    uint32_t settle_frame_ = 0;
};

static TestCaseRegistrar<UsdRoundTripTest> usd_round_trip_test_registrar("usd_round_trip");
} // namespace sparkle
