#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "scene/component/camera/CameraComponent.h"
#include "scene/component/camera/OrbitCameraComponent.h"

namespace sparkle
{

/// M1 test gate: Validates that CameraRenderProxy stores previous-frame
/// matrices and that they differ from current-frame matrices after camera motion.
///
/// Phase 1 (frames 1-3): Static warmup — verify prev == current.
/// Phase 2 (frames 4-12): Continuous motion — orbit yaw changes each frame.
///   During motion, verify prev != current (frame-to-frame delta > 0).
/// Phase 3 (frame 13+): Validate cumulative displacement from initial position.
///
/// The render proxy update is asynchronous (render thread), so continuous motion
/// ensures prev != current on any given check frame.
///
/// Usage: --test_case reblur_matrix_infra --pipeline gpu --use_reblur true --spp 1 --max_spp 20
class ReblurMatrixInfraTest : public TestCase
{
public:
    Result Tick(AppFramework &app) override
    {
        frame_++;

        auto *camera = app.GetMainCamera();
        if (!camera)
        {
            return Result::Pending;
        }

        auto *proxy = static_cast<CameraRenderProxy *>(camera->GetRenderProxy());
        if (!proxy)
        {
            return Result::Pending;
        }

        auto *orbit = dynamic_cast<OrbitCameraComponent *>(camera);

        // Phase 1: Static check at frame 3
        if (frame_ == 3)
        {
            auto pos = proxy->GetPosture().position;
            auto pos_prev = proxy->GetPositionPrev();

            float pos_delta = (pos - pos_prev).norm();

            Log(Info, "ReblurMatrixInfraTest: frame 3 (static) — pos_delta={:.6f}", pos_delta);

            if (pos_delta > 1e-4f)
            {
                Log(Error, "ReblurMatrixInfraTest: FAIL — position changed without camera motion (delta={:.6f})",
                    pos_delta);
                return Result::Fail;
            }

            // Record initial position before motion begins
            initial_position_ = pos;
            Log(Info, "ReblurMatrixInfraTest: initial position = ({:.4f}, {:.4f}, {:.4f})", pos.x(), pos.y(), pos.z());
        }

        // Phase 2: Continuous motion (frames 4-12) — change yaw by 5 degrees each frame
        if (frame_ >= kMotionStart && frame_ <= kMotionEnd)
        {
            if (orbit)
            {
                float yaw = 30.0f + 5.0f * static_cast<float>(frame_ - kMotionStart + 1);
                orbit->Setup(Vector3::Zero(), 3.0f, 20.0f, yaw);
            }

            // Log delta during motion to observe propagation timing
            auto pos = proxy->GetPosture().position;
            auto pos_prev = proxy->GetPositionPrev();
            auto vp = proxy->GetViewProjectionMatrix();
            auto vp_prev = proxy->GetViewProjectionMatrixPrev();

            float pos_delta = (pos - pos_prev).norm();
            float mat_diff = (vp - vp_prev).norm();

            Log(Info, "ReblurMatrixInfraTest: frame {} (motion) — pos_delta={:.6f}, mat_diff={:.6f}", frame_,
                pos_delta, mat_diff);

            // Once we observe non-zero delta, record it
            if (pos_delta > 1e-4f)
            {
                observed_motion_ = true;
            }
            if (mat_diff > 1e-6f)
            {
                observed_matrix_change_ = true;
            }

            return Result::Pending;
        }

        // Phase 3: Final validation at frame 15 (after motion stopped)
        if (frame_ == kCheckFrame)
        {
            auto pos = proxy->GetPosture().position;
            auto vp_prev = proxy->GetViewProjectionMatrixPrev();

            // Semantic: Must have observed frame-to-frame position delta during motion
            if (!observed_motion_)
            {
                Log(Error, "ReblurMatrixInfraTest: FAIL — never observed position delta during motion phase");
                return Result::Fail;
            }

            // Semantic: Must have observed VP matrix change during motion
            if (!observed_matrix_change_)
            {
                Log(Error, "ReblurMatrixInfraTest: FAIL — never observed VP matrix change during motion phase");
                return Result::Fail;
            }

            // Statistical: VP prev should not be identity (was populated from real camera data)
            Mat4 identity = Mat4::Identity();
            float identity_diff = (vp_prev - identity).norm();
            if (identity_diff < 1e-6f)
            {
                Log(Error, "ReblurMatrixInfraTest: FAIL — prev VP is still identity");
                return Result::Fail;
            }

            // Statistical: Cumulative displacement from initial position should be substantial
            float displacement = (pos - initial_position_).norm();
            Log(Info, "ReblurMatrixInfraTest: cumulative displacement = {:.6f}", displacement);
            if (displacement < 0.01f)
            {
                Log(Error, "ReblurMatrixInfraTest: FAIL — negligible displacement ({:.6f})", displacement);
                return Result::Fail;
            }

            Log(Info, "ReblurMatrixInfraTest: PASS — all matrix infrastructure checks passed");
            return Result::Pass;
        }

        uint32_t timeout = app.GetAppConfig().test_timeout;
        if (timeout > 0 && frame_ > timeout)
        {
            Log(Error, "ReblurMatrixInfraTest timed out after {} frames", timeout);
            return Result::Fail;
        }

        return Result::Pending;
    }

private:
    static constexpr uint32_t kMotionStart = 4;
    static constexpr uint32_t kMotionEnd = 12;
    static constexpr uint32_t kCheckFrame = 15;

    uint32_t frame_ = 0;
    Vector3 initial_position_ = Vector3::Zero();
    bool observed_motion_ = false;
    bool observed_matrix_change_ = false;
};

static TestCaseRegistrar<ReblurMatrixInfraTest> reblur_matrix_infra_registrar("reblur_matrix_infra");
} // namespace sparkle
