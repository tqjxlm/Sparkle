# Converged History Camera Delta Test — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Test that after a fully converged static frame, a small camera yaw delta preserves most temporal history, producing a clean (non-noisy) output — validating that motion vector reprojection correctly maps the converged history to the new viewpoint.

**Architecture:** A C++ TestCase converges 50 frames statically, screenshots "before", applies a 2° yaw nudge, waits 2 frames for reprojection, screenshots "after". A Python script orchestrates two runs (full pipeline + debug_pass 3 for temporal accumulation) and validates noise levels, structural similarity, and history preservation with quantitative metrics.

**Tech Stack:** C++ TestCase system, Python (numpy, PIL, scipy), REBLUR denoiser pipeline, reblur_debug_pass 3

---

## Context

### The Problem

When the camera moves slightly, the REBLUR denoiser should reprojection-fetch history from the converged previous frame using motion vectors. If reprojection works correctly:
- `accumSpeed` stays high (~50 after 50 frames), so `weight = 1/(1+50) ≈ 0.02` — only 2% new sample
- Result: nearly noise-free frame, structurally similar to the converged "before" frame

If reprojection fails:
- `accumSpeed` resets to 1 (disoccluded), so `weight = 0.5` — 50% raw 1spp noise
- Result: extremely noisy frame, visually terrible

### Key Files to Understand

- `tests/reblur/ReblurReprojectionTest.cpp` — existing motion test pattern (orbit + settle + screenshot)
- `tests/reblur/ReblurMotionVectorTest.cpp` — existing MV test with before/after screenshots
- `shaders/ray_trace/reblur_temporal_accumulation.cs.slang` — the shader that does the reprojection
- `shaders/include/reblur_reprojection.h.slang` — BilinearHistorySample, CatmullRomHistorySample
- `libraries/include/scene/component/camera/OrbitCameraComponent.h` — GetYaw/GetPitch/GetCenter/GetRadius + Setup()
- `libraries/include/renderer/proxy/CameraRenderProxy.h` — GetPositionPrev(), GetPosture()
- `tests/reblur/reblur_test_suite.py` — test suite to register new test in

### Intermediate Textures

| debug config | What it captures | Why it matters |
|---|---|---|
| (none) | Full pipeline end-to-end | End-to-end quality validation |
| `--reblur_debug_pass 3` | After TemporalAccumulation, before HistoryFix/Blur/Stabilize | Directly shows whether BilinearHistorySample found valid history or returned disoccluded output |

---

## Task 1: Write C++ Test Case `ReblurConvergedHistoryTest`

**Files:**
- Create: `tests/reblur/ReblurConvergedHistoryTest.cpp`

**Step 1: Write the test case**

```cpp
#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/Logger.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "scene/component/camera/CameraComponent.h"
#include "scene/component/camera/OrbitCameraComponent.h"

namespace sparkle
{

/// Validates that a small camera yaw delta preserves converged temporal history.
///
/// Phase 1 (frames 1-50):    Static camera convergence — accumulate 50 denoised frames.
/// Phase 2 (frame 51):       Take "before" screenshot of the fully converged frame.
/// Phase 3 (after complete):  Apply 2° yaw delta to orbit camera.
/// Phase 4 (+2 frames):       Wait for MV computation and reprojection to happen.
/// Phase 5:                   Take "after" screenshot — should be nearly as clean as "before".
///
/// The C++ test validates no crash. Python companion validates noise levels.
///
/// Usage: --test_case reblur_converged_history --pipeline gpu --use_reblur true --spp 1 --max_spp 100
class ReblurConvergedHistoryTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        auto *camera = app.GetMainCamera();
        if (!camera)
        {
            return Result::Pending;
        }

        auto *orbit = dynamic_cast<OrbitCameraComponent *>(camera);

        // Phase 1: Static convergence
        if (frame_ <= ConvergenceFrames)
        {
            if (frame_ == 1)
            {
                Log(Info, "ReblurConvergedHistoryTest: convergence phase (frames 1-{})", ConvergenceFrames);
            }
            return Result::Pending;
        }

        // Phase 2: Request "before" screenshot
        if (!before_request_)
        {
            Log(Info, "ReblurConvergedHistoryTest: requesting 'before' screenshot at frame {}", frame_);
            before_request_ = app.RequestTakeScreenshot("converged_history_before");
            return Result::Pending;
        }

        // Wait for "before" screenshot to complete
        if (!before_done_ && before_request_->IsCompleted())
        {
            before_done_ = true;
            Log(Info, "ReblurConvergedHistoryTest: 'before' screenshot captured");
        }

        if (!before_done_)
        {
            return Result::Pending;
        }

        // Phase 3: Apply small yaw delta (once)
        if (!nudge_applied_)
        {
            if (orbit)
            {
                float old_yaw = orbit->GetYaw();
                float new_yaw = old_yaw + YawDelta;
                orbit->Setup(orbit->GetCenter(), orbit->GetRadius(), orbit->GetPitch(), new_yaw);
                Log(Info, "ReblurConvergedHistoryTest: applied {:.1f}° yaw delta ({}° -> {}°)", YawDelta,
                    old_yaw, new_yaw);
            }
            else
            {
                Log(Warn, "ReblurConvergedHistoryTest: camera is not OrbitCameraComponent, cannot nudge");
                return Result::Fail;
            }
            nudge_applied_ = true;
            nudge_frame_ = frame_;
            return Result::Pending;
        }

        // Phase 4: Wait for reprojection to happen (2 frames after nudge)
        if (frame_ < nudge_frame_ + SettleFrames)
        {
            // Log camera delta on first frame after nudge to confirm motion propagated
            if (frame_ == nudge_frame_ + 1)
            {
                auto *proxy = static_cast<CameraRenderProxy *>(camera->GetRenderProxy());
                float delta = (proxy->GetPosture().position - proxy->GetPositionPrev()).norm();
                Log(Info, "ReblurConvergedHistoryTest: camera position delta = {:.6f}", delta);
            }
            return Result::Pending;
        }

        // Phase 5: Request "after" screenshot
        if (!after_request_)
        {
            Log(Info, "ReblurConvergedHistoryTest: requesting 'after' screenshot at frame {}", frame_);
            after_request_ = app.RequestTakeScreenshot("converged_history_after");
            return Result::Pending;
        }

        if (after_request_->IsCompleted())
        {
            Log(Info, "ReblurConvergedHistoryTest: 'after' screenshot captured at frame {} — PASS", frame_);
            return Result::Pass;
        }

        return Result::Pending;
    }

private:
    static constexpr uint32_t ConvergenceFrames = 50;
    static constexpr float YawDelta = 2.0f; // degrees
    static constexpr uint32_t SettleFrames = 2;

    std::shared_ptr<ScreenshotRequest> before_request_;
    std::shared_ptr<ScreenshotRequest> after_request_;
    bool before_done_ = false;
    bool nudge_applied_ = false;
    uint32_t nudge_frame_ = 0;
};

static TestCaseRegistrar<ReblurConvergedHistoryTest> reblur_converged_history_registrar(
    "reblur_converged_history");
} // namespace sparkle
```

**Step 2: Build and run smoke check**

```bash
python build.py --framework glfw
python build.py --framework glfw --skip_build --run \
    --test_case reblur_converged_history --headless true \
    --pipeline gpu --use_reblur true --spp 1 --max_spp 100 \
    --test_timeout 120 --clear_screenshots true
```

Expected: Exit code 0, two screenshots produced: `converged_history_before.png`, `converged_history_after.png`.

**Step 3: Commit**

```bash
git add tests/reblur/ReblurConvergedHistoryTest.cpp
git commit -m "test(reblur): add converged history camera delta test case"
```

---

## Task 2: Write Python Validation Script `test_converged_history.py`

**Files:**
- Create: `tests/reblur/test_converged_history.py`

**Design:**

The script orchestrates two runs of the C++ test case:
1. **Full pipeline** — end-to-end quality before/after camera nudge
2. **`reblur_debug_pass 3`** — temporal accumulation output to validate history preservation

**Metrics:**
- **Laplacian variance**: Measures high-frequency noise. Apply 3x3 Laplacian kernel to luminance, compute variance. Higher = noisier.
- **Noise ratio**: `laplacian_var_after / laplacian_var_before`. If reprojection works: ≈1-3x. If broken: >10x.
- **Mean luminance**: Validates images aren't black.
- **NaN/Inf checks**: No invalid pixels.

**Thresholds:**
| Metric | Run 1 (full pipeline) | Run 2 (debug_pass 3) |
|---|---|---|
| Noise ratio | < 5.0 | < 5.0 |
| Before mean luma | > 1e-4 | > 1e-4 |
| After mean luma | > 1e-4 | > 1e-4 |
| No NaN/Inf | yes | yes |

The temporal accumulation run (debug_pass 3) has a more relaxed noise ratio because it shows the raw temporal output before spatial blur and stabilization.

**Step 1: Write the script**

See full implementation below. Key functions:
- `compute_laplacian_variance(img)`: 3x3 Laplacian filter → variance
- `validate_run(before_path, after_path, label, noise_ratio_max)`: Compare screenshots
- `main()`: Orchestrate runs, print summary

**Step 2: Run the validation**

```bash
python tests/reblur/test_converged_history.py --framework glfw
```

Expected: Runs build + two test runs, prints validation results.

**Step 3: Commit**

```bash
git add tests/reblur/test_converged_history.py
git commit -m "test(reblur): add converged history Python validation script"
```

---

## Task 3: Register in Test Suite

**Files:**
- Modify: `tests/reblur/reblur_test_suite.py`

**Step 1: Add test 20**

After test 19 (Camera motion quality validation), add:

```python
# --- Test 20: Converged history + camera delta ---
converged_hist_py = os.path.join(SCRIPT_DIR, "test_converged_history.py")
ok, dur, _ = run_command(
    [py, converged_hist_py, "--framework", fw, "--skip_build"],
    "20. Converged history camera delta (history preservation)",
    show_output=True)
results.append(("Converged history camera delta", ok, dur))
```

Update the docstring to include test 20.

**Step 2: Commit**

```bash
git add tests/reblur/reblur_test_suite.py
git commit -m "test(reblur): register converged history test in suite"
```

---

## Task 4: Build, Run, and Validate

**Step 1: Full build**

```bash
python build.py --framework glfw
```

**Step 2: Run the Python validation**

```bash
python tests/reblur/test_converged_history.py --framework glfw --skip_build
```

**Step 3: Record findings**

Update `progress.md` with:
- Test output (noise ratios, pass/fail)
- Screenshots produced
- Any issues discovered

---

## Expected Test Output (When Reprojection Works)

```
==============================================================
  Converged History Camera Delta Validation
==============================================================

--- Run 1: Full pipeline (end-to-end) ---
  Before: mean_luma=0.2345, laplacian_var=0.0012
  After:  mean_luma=0.2301, laplacian_var=0.0018
  Noise ratio: 1.50x
  [PASS] Noise ratio 1.50 < 5.0

--- Run 2: Temporal accumulation (reblur_debug_pass 3) ---
  Before: mean_luma=0.1890, laplacian_var=0.0025
  After:  mean_luma=0.1856, laplacian_var=0.0041
  Noise ratio: 1.64x
  [PASS] Noise ratio 1.64 < 8.0

==============================================================
  Results: 8 passed, 0 failed
==============================================================
```

## Expected Test Output (When Reprojection Is Broken)

```
--- Run 1: Full pipeline (end-to-end) ---
  Before: mean_luma=0.2345, laplacian_var=0.0012
  After:  mean_luma=0.2100, laplacian_var=0.0320
  Noise ratio: 26.67x
  [FAIL] Noise ratio 26.67 >= 5.0 (history not preserved)
```
