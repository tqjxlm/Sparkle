#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "application/TestCase.h"
#include "scene/Scene.h"
#include "scene/SceneNode.h"
#include "scene/component/primitive/PrimitiveComponent.h"

namespace sparkle
{
// moves one mesh between two screenshots. tests/scene/primitive_move_test.py drives this test
// and asserts the screenshots differ, guarding TLAS/BVH refits for moved primitives.
class PrimitiveMoveTest : public TestCase
{
public:
    // give the moved primitive a few frames so acceleration structures consume the new transform
    static constexpr uint32_t SettleFrames = 10;

    Result OnTick(AppFramework &app) override
    {
        auto *render_framework = app.GetRenderFramework();

        switch (stage_)
        {
        case Stage::WaitInitialRender:
            if (render_framework->IsReadyForAutoScreenshot())
            {
                screenshot_ = render_framework->RequestTakeScreenshot("primitive_move_before");
                stage_ = Stage::WaitBeforeScreenshot;
            }
            break;

        case Stage::WaitBeforeScreenshot:
            if (screenshot_->IsCompleted())
            {
                if (!MoveOnePrimitive(app))
                {
                    Log(Error, "PrimitiveMoveTest: the scene has no primitive to move");
                    return Result::Fail;
                }

                settle_frame_ = frame_ + SettleFrames;
                stage_ = Stage::WaitMovedRender;
            }
            break;

        case Stage::WaitMovedRender:
            if (frame_ >= settle_frame_ && render_framework->IsReadyForAutoScreenshot())
            {
                screenshot_ = render_framework->RequestTakeScreenshot("primitive_move_after");
                stage_ = Stage::WaitAfterScreenshot;
            }
            break;

        case Stage::WaitAfterScreenshot:
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
    static bool MoveOnePrimitive(AppFramework &app)
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

        auto *node = picked->GetNode();
        const auto &local = node->GetLocalTransform();
        node->SetTransform(local.GetTranslation() + Vector3{0.8f, 0.f, 0.f}, local.GetRotation(), local.GetScale());

        return true;
    }

    enum class Stage : uint8_t
    {
        WaitInitialRender,
        WaitBeforeScreenshot,
        WaitMovedRender,
        WaitAfterScreenshot,
    };

    Stage stage_ = Stage::WaitInitialRender;

    std::shared_ptr<ScreenshotRequest> screenshot_;

    uint32_t settle_frame_ = 0;
};

static TestCaseRegistrar<PrimitiveMoveTest> primitive_move_test_registrar("primitive_move");
} // namespace sparkle
