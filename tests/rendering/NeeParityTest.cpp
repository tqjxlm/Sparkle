#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "application/TestCase.h"
#include "scene/Scene.h"
#include "scene/SceneNode.h"
#include "scene/component/primitive/PrimitiveComponent.h"
#include "scene/material/LambertianMaterial.h"
#include "scene/material/MaterialManager.h"

namespace sparkle
{
// turns one mesh into an emitter, so the sky and an emissive surface both light the scene, then screenshots the
// converged gpu path tracer without and with next event estimation. tests/rendering/nee_parity_test.py drives this
// test and requires both screenshots to carry the same energy.
class NeeParityTest : public TestCase
{
public:
    // frames the renderer gets to pick a change up before its convergence is polled
    static constexpr uint32_t SettleFrames = 10;

    // below the path tracer's firefly clamp, which would otherwise cut the two estimators differently
    static constexpr float EmitterRadiance = 5.f;

    void OnEnforceConfigs() override
    {
        EnforceConfig("enable_nee", false);
    }

    Result OnTick(AppFramework &app) override
    {
        auto *render_framework = app.GetRenderFramework();

        switch (stage_)
        {
        case Stage::WaitScene:
            if (render_framework->IsSceneFullyLoaded())
            {
                if (app.GetRenderConfig().pipeline != RenderConfig::Pipeline::Gpu)
                {
                    Log(Error, "NeeParityTest: the gpu pipeline is not running");
                    return Result::Fail;
                }
                if (!AddEmitter(app))
                {
                    Log(Error, "NeeParityTest: the scene has no primitive to turn into an emitter");
                    return Result::Fail;
                }

                settle_frame_ = frame_ + SettleFrames;
                stage_ = Stage::WaitReferenceRender;
            }
            break;

        case Stage::WaitReferenceRender:
            if (frame_ >= settle_frame_ && render_framework->IsReadyForAutoScreenshot())
            {
                screenshot_ = render_framework->RequestTakeScreenshot("nee_parity_off");
                stage_ = Stage::WaitReferenceScreenshot;
            }
            break;

        case Stage::WaitReferenceScreenshot:
            if (screenshot_->IsCompleted())
            {
                EnforceConfig("enable_nee", true);

                settle_frame_ = frame_ + SettleFrames;
                stage_ = Stage::WaitNeeRender;
            }
            break;

        case Stage::WaitNeeRender:
            restarted_ = restarted_ || !render_framework->IsReadyForAutoScreenshot();
            if (frame_ >= settle_frame_)
            {
                // a finished accumulation that keeps its samples would make the second screenshot a copy of the first
                if (!restarted_)
                {
                    Log(Error, "NeeParityTest: enabling next event estimation did not restart the accumulation");
                    return Result::Fail;
                }
                if (render_framework->IsReadyForAutoScreenshot())
                {
                    screenshot_ = render_framework->RequestTakeScreenshot("nee_parity_on");
                    stage_ = Stage::WaitNeeScreenshot;
                }
            }
            break;

        case Stage::WaitNeeScreenshot:
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
    static bool AddEmitter(AppFramework &app)
    {
        // the primitive set is unordered: pick deterministically by node name
        PrimitiveComponent *picked = nullptr;
        for (auto *primitive : app.GetScene()->GetPrimitives())
        {
            if (!picked || primitive->GetNode()->GetName() < picked->GetNode()->GetName())
            {
                picked = primitive;
            }
        }

        if (!picked)
        {
            return false;
        }

        picked->SetMaterial(MaterialManager::Instance().CreateMaterial<LambertianMaterial>(
            {.emissive_color = Ones * EmitterRadiance, .name = "NeeParityEmitter"}));

        return true;
    }

    enum class Stage : uint8_t
    {
        WaitScene,
        WaitReferenceRender,
        WaitReferenceScreenshot,
        WaitNeeRender,
        WaitNeeScreenshot,
    };

    Stage stage_ = Stage::WaitScene;

    std::shared_ptr<ScreenshotRequest> screenshot_;

    uint32_t settle_frame_ = 0;

    bool restarted_ = false;
};

static TestCaseRegistrar<NeeParityTest> nee_parity_test_registrar("nee_parity");
} // namespace sparkle
