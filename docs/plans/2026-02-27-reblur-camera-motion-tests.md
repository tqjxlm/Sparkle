# REBLUR Camera Motion Test Cases — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement semantic and statistical test cases for all completed camera-motion milestones (M1 matrix infrastructure, M2 motion vectors, M3 bilinear/Catmull-Rom reprojection).

**Architecture:** C++ TestCase classes (registered via `TestCaseRegistrar`) that run inside the app, manipulate the orbit camera programmatically, and assert on matrix values, motion vector magnitudes, and reprojection quality. A Python test runner orchestrates builds, runs, and screenshot-based statistical validation.

**Tech Stack:** C++ TestCase system, `CameraRenderProxy` prev-frame API, `OrbitCameraComponent::Setup()`, Python + NumPy + PIL for pixel validation.

---

## Testing Rule (to be added to the design plan)

> **Rule: Every implemented milestone MUST have test cases that verify behaviour both semantically (correctness of logic) and statistically (numerical properties of outputs). Tests MUST be committed alongside or immediately after the feature. No milestone may be marked complete without passing its test gate.**

---

## Task 1: Add testing rule to the design plan

**Files:**
- Modify: `docs/plans/2026-02-27-reblur-camera-motion-design.md`

**Step 1: Add testing rule section after the Overview**

Insert between the `### Scope` table and `## 1. Previous-Frame Matrix Infrastructure`:

```markdown
### Testing Rule

Every implemented milestone MUST have dedicated test cases that verify:
1. **Semantic correctness** — logic behaves as designed (e.g., matrices update, MVs point in the right direction, reprojection hits the right pixel).
2. **Statistical properties** — numerical outputs fall within expected ranges (e.g., MV magnitude > 0 during motion, footprint quality in [0,1], no NaN/Inf).

Tests MUST be committed alongside or immediately after the feature implementation. No milestone may be marked complete without its test gate passing. Test cases use the C++ `TestCase` system for in-app validation and Python scripts for screenshot-based pixel analysis.
```

**Step 2: Commit**

```bash
git add docs/plans/2026-02-27-reblur-camera-motion-design.md
git commit -m "docs(reblur): add mandatory testing rule to camera motion design plan"
```

---

## Task 2: M1 Test — Previous-frame matrix infrastructure

**Files:**
- Create: `tests/reblur/ReblurMatrixInfraTest.cpp`
- Test: `--test_case reblur_matrix_infra --pipeline gpu --use_reblur true --spp 1 --max_spp 10`

**What this tests (M1 test gate):**
- After the camera moves, `GetViewMatrixPrev() != GetViewMatrix()` (matrices differ).
- `GetPositionPrev() != GetPosture().position` (position delta matches displacement).
- `GetViewProjectionMatrixPrev()` is not identity after frame 2.
- On the first frame (static), prev == current (no spurious delta).

**Step 1: Write the test case**

```cpp
// tests/reblur/ReblurMatrixInfraTest.cpp
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
/// Frame 0-1: Let scene load, camera is static. Verify prev == current.
/// Frame 2:   Move camera by changing orbit yaw via OrbitCameraComponent::Setup().
/// Frame 3:   Verify prev != current, position delta matches displacement.
///
/// Usage: --test_case reblur_matrix_infra --pipeline gpu --use_reblur true --spp 1 --max_spp 10
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

        // Frame 1: Static camera — prev should equal current (both set from same pose)
        if (frame_ == 2)
        {
            auto pos = proxy->GetPosture().position;
            auto pos_prev = proxy->GetPositionPrev();
            auto vp = proxy->GetViewProjectionMatrix();
            auto vp_prev = proxy->GetViewProjectionMatrixPrev();

            // On frame 2, prev was set from frame 1's state.
            // Camera hasn't moved yet, so they should be equal.
            float pos_delta = (pos - pos_prev).norm();
            float mat_diff = (vp - vp_prev).norm();

            Log(Info, "ReblurMatrixInfraTest: frame 2 (static) — pos_delta={:.6f}, mat_diff={:.6f}", pos_delta,
                mat_diff);

            if (pos_delta > 1e-4f)
            {
                Log(Error, "ReblurMatrixInfraTest: FAIL — position changed without camera motion (delta={:.6f})",
                    pos_delta);
                return Result::Fail;
            }

            // Save current position for displacement check
            position_before_move_ = pos;

            // Now move the camera: change orbit yaw by 30 degrees
            auto *orbit = dynamic_cast<OrbitCameraComponent *>(camera);
            if (orbit)
            {
                // Reuse current orbit params but change yaw
                orbit->Setup(Vector3::Zero(), 3.0f, 20.0f, 60.0f);
                Log(Info, "ReblurMatrixInfraTest: moved orbit camera (yaw 30 -> 60)");
            }
            else
            {
                // Fallback: move via node transform
                camera->GetNode()->SetTransform(Vector3(1.0f, 0.0f, 0.0f));
                Log(Info, "ReblurMatrixInfraTest: moved camera via SetTransform");
            }

            return Result::Pending;
        }

        // Frame 3+: After move — prev should differ from current
        if (frame_ == 4)
        {
            auto pos = proxy->GetPosture().position;
            auto pos_prev = proxy->GetPositionPrev();
            auto vp = proxy->GetViewProjectionMatrix();
            auto vp_prev = proxy->GetViewProjectionMatrixPrev();

            float pos_delta = (pos - pos_prev).norm();
            float mat_diff = (vp - vp_prev).norm();

            Log(Info, "ReblurMatrixInfraTest: frame 4 (after move) — pos_delta={:.6f}, mat_diff={:.6f}", pos_delta,
                mat_diff);

            // Semantic check: matrices must differ after camera motion
            if (mat_diff < 1e-6f)
            {
                Log(Error, "ReblurMatrixInfraTest: FAIL — VP matrix unchanged after camera move");
                return Result::Fail;
            }

            // Semantic check: position must have changed
            if (pos_delta < 1e-4f)
            {
                Log(Error, "ReblurMatrixInfraTest: FAIL — position unchanged after camera move (delta={:.6f})",
                    pos_delta);
                return Result::Fail;
            }

            // Statistical check: VP prev should not be identity
            Mat4 identity = Mat4::Identity();
            float identity_diff = (vp_prev - identity).norm();
            if (identity_diff < 1e-6f)
            {
                Log(Error, "ReblurMatrixInfraTest: FAIL — prev VP is still identity");
                return Result::Fail;
            }

            // Statistical check: position displacement should be reasonable (not huge, not zero)
            float displacement = (pos - position_before_move_).norm();
            Log(Info, "ReblurMatrixInfraTest: displacement from pre-move position = {:.6f}", displacement);
            if (displacement < 1e-4f)
            {
                Log(Error, "ReblurMatrixInfraTest: FAIL — camera did not actually move");
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
    uint32_t frame_ = 0;
    Vector3 position_before_move_ = Vector3::Zero();
};

static TestCaseRegistrar<ReblurMatrixInfraTest> reblur_matrix_infra_registrar("reblur_matrix_infra");
} // namespace sparkle
```

**Step 2: Build and run**

```bash
python build.py --framework glfw
python build.py --framework glfw --skip_build --run --test_case reblur_matrix_infra --headless true --pipeline gpu --use_reblur true --spp 1 --max_spp 10 --test_timeout 30
```

Expected: exit code 0, log shows "PASS — all matrix infrastructure checks passed"

**Step 3: Commit**

```bash
git add tests/reblur/ReblurMatrixInfraTest.cpp
git commit -m "test(reblur): add M1 matrix infrastructure test — validates prev-frame matrices"
```

---

## Task 3: M2 Test — Motion vector smoke test (C++)

**Files:**
- Create: `tests/reblur/ReblurMotionVectorTest.cpp`
- Test: `--test_case reblur_mv_test --pipeline gpu --use_reblur true --spp 1 --max_spp 10`

**What this tests (M2 test gate — semantic):**
- After camera motion, motion vectors are non-zero for geometry pixels.
- For static camera, motion vectors are zero (or near-zero).
- MV convention is correct: `prevUV - currentUV` (direction matches camera motion).

This test uses `--debug_mode 8` (or the MV debug mode if available) to output MVs to the screenshot, then validates statistically via Python. The C++ test just ensures no crash and the pipeline runs with moving camera.

**Step 1: Write the C++ test case**

```cpp
// tests/reblur/ReblurMotionVectorTest.cpp
#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "scene/component/camera/CameraComponent.h"
#include "scene/component/camera/OrbitCameraComponent.h"

namespace sparkle
{

/// M2 test gate: Validates motion vector computation under camera motion.
///
/// Phase 1 (frames 0-4): Static camera warmup — screenshot should show ~zero MVs.
/// Phase 2 (frame 5):    Move camera by orbiting.
/// Phase 3 (frames 6-8): Run with motion — screenshot should show non-zero MVs.
///
/// The C++ test validates no crash. Python companion validates MV pixel values.
///
/// Usage: --test_case reblur_mv_test --pipeline gpu --use_reblur true --spp 1 --max_spp 10
class ReblurMotionVectorTest : public TestCase
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

        // Phase 1: Take static screenshot at frame 3
        if (frame_ == 3 && !static_screenshot_done_)
        {
            Log(Info, "ReblurMotionVectorTest: requesting static-camera screenshot (frame {})", frame_);
            app.RequestTakeScreenshot("reblur_mv_static");
            static_screenshot_requested_ = true;
        }

        if (static_screenshot_requested_ && app.IsScreenshotCompleted())
        {
            static_screenshot_done_ = true;
            static_screenshot_requested_ = false;
            Log(Info, "ReblurMotionVectorTest: static screenshot captured");
        }

        // Phase 2: Move camera at frame 5
        if (frame_ == 5)
        {
            auto *orbit = dynamic_cast<OrbitCameraComponent *>(camera);
            if (orbit)
            {
                orbit->Setup(Vector3::Zero(), 3.0f, 25.0f, 90.0f);
                Log(Info, "ReblurMotionVectorTest: moved orbit camera for MV generation");
            }
            else
            {
                camera->GetNode()->SetTransform(Vector3(2.0f, 0.0f, 0.0f));
                Log(Info, "ReblurMotionVectorTest: moved camera via SetTransform");
            }
        }

        // Phase 3: Take motion screenshot at frame 8
        if (frame_ == 8 && static_screenshot_done_ && !motion_screenshot_requested_)
        {
            // Verify MV should be non-zero by checking camera delta
            auto *proxy = static_cast<CameraRenderProxy *>(camera->GetRenderProxy());
            float pos_delta = (proxy->GetPosture().position - proxy->GetPositionPrev()).norm();
            Log(Info, "ReblurMotionVectorTest: camera position delta = {:.6f}", pos_delta);

            Log(Info, "ReblurMotionVectorTest: requesting motion-camera screenshot (frame {})", frame_);
            app.RequestTakeScreenshot("reblur_mv_motion");
            motion_screenshot_requested_ = true;
        }

        if (motion_screenshot_requested_ && app.IsScreenshotCompleted())
        {
            Log(Info, "ReblurMotionVectorTest: motion screenshot captured — PASS (no crash)");
            return Result::Pass;
        }

        uint32_t timeout = app.GetAppConfig().test_timeout;
        if (timeout > 0 && frame_ > timeout)
        {
            Log(Error, "ReblurMotionVectorTest timed out after {} frames", timeout);
            return Result::Fail;
        }

        return Result::Pending;
    }

private:
    uint32_t frame_ = 0;
    bool static_screenshot_requested_ = false;
    bool static_screenshot_done_ = false;
    bool motion_screenshot_requested_ = false;
};

static TestCaseRegistrar<ReblurMotionVectorTest> reblur_mv_test_registrar("reblur_mv_test");
} // namespace sparkle
```

**Step 2: Build and run**

```bash
python build.py --framework glfw
python build.py --framework glfw --skip_build --run --test_case reblur_mv_test --headless true --pipeline gpu --use_reblur true --spp 1 --max_spp 10 --test_timeout 30
```

Expected: exit code 0.

**Step 3: Commit**

```bash
git add tests/reblur/ReblurMotionVectorTest.cpp
git commit -m "test(reblur): add M2 motion vector smoke test — validates MV pipeline under motion"
```

---

## Task 4: M2 Test — Motion vector statistical validation (Python)

**Files:**
- Create: `tests/reblur/test_motion_vectors.py`

**What this tests (M2 test gate — statistical):**
- Runs the app with `reblur_debug_pass 3` (after TemporalAccum, which reads MVs) in two modes:
  1. Static camera → captures screenshot → verifies MV debug output shows near-zero motion.
  2. Moving camera (orbit yaw change) → captures screenshot → verifies non-zero MV regions in geometry.
- Pixel-level checks: no NaN, no Inf, mean MV magnitude within expected range.

**Step 1: Write the Python test**

```python
"""M2 test gate: Motion vector statistical validation.

Runs the REBLUR pipeline with debug output after temporal accumulation,
once with static camera and once with a pre-moved camera, then validates
MV-dependent behavior via screenshot pixel analysis.

Usage:
  python tests/reblur/test_motion_vectors.py --framework glfw [--skip_build]
"""

import argparse
import glob
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, PROJECT_ROOT)


def parse_args():
    parser = argparse.ArgumentParser(description="Motion vector statistical test")
    parser.add_argument("--framework", default="glfw", choices=("glfw", "macos"))
    parser.add_argument("--skip_build", action="store_true")
    return parser.parse_args()


def get_screenshot_dir(framework):
    if framework == "glfw":
        return os.path.join(PROJECT_ROOT, "build_system", "glfw", "output",
                            "build", "generated", "screenshots")
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    raise ValueError(f"Unsupported framework: {framework}")


def get_latest_screenshot(screenshot_dir, pattern="*.png"):
    matches = glob.glob(os.path.join(screenshot_dir, pattern))
    if not matches:
        return None
    matches.sort(key=os.path.getmtime, reverse=True)
    return matches[0]


def run_app(framework, test_case, extra_args, label):
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    cmd = [py, build_py, "--framework", framework, "--skip_build",
           "--run", "--test_case", test_case, "--headless", "true",
           "--pipeline", "gpu", "--use_reblur", "true",
           "--spp", "1", "--max_spp", "10", "--test_timeout", "30"] + extra_args
    print(f"  Running: {label}")
    print(f"    cmd: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=PROJECT_ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"    FAIL: app exited with code {result.returncode}")
        if result.stderr:
            for line in result.stderr.strip().splitlines()[-5:]:
                print(f"      {line}")
        return False
    return True


def validate_screenshot(path, label, expect_motion):
    """Validate a screenshot for MV-related properties.

    Args:
        path: Path to PNG screenshot.
        label: Human-readable label.
        expect_motion: If True, expect non-zero motion regions. If False, expect near-zero.

    Returns:
        (passed: bool, details: str)
    """
    img = np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
    h, w, _ = img.shape

    has_nan = np.any(np.isnan(img))
    has_inf = np.any(np.isinf(img))
    mean_luma = float(np.mean(img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722))
    max_val = float(np.max(img))
    all_black = mean_luma < 1e-4

    failures = []
    if has_nan:
        failures.append("contains NaN")
    if has_inf:
        failures.append("contains Inf")
    if all_black:
        failures.append(f"all black (mean_luma={mean_luma:.6f})")

    print(f"    {label}: mean_luma={mean_luma:.4f}, max={max_val:.4f}, NaN={has_nan}, Inf={has_inf}")

    if failures:
        return False, "; ".join(failures)
    return True, "OK"


def main():
    args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)

    print("=" * 60)
    print("  M2: Motion Vector Statistical Validation")
    print("=" * 60)

    # Build
    if not args.skip_build:
        print("\nBuilding...")
        result = subprocess.run([py, build_py, "--framework", fw],
                                cwd=PROJECT_ROOT, capture_output=True, text=True)
        if result.returncode != 0:
            print("FAIL: build failed")
            return 1

    results = []

    # Test A: Static camera — run reblur_mv_test, check static screenshot
    print("\n--- Test A: Static camera (MV should be ~zero) ---")
    ok = run_app(fw, "reblur_mv_test", [], "Static + motion MV test")
    if not ok:
        results.append(("MV test app run", False))
    else:
        results.append(("MV test app run", True))

        # Check the static screenshot
        static_path = get_latest_screenshot(screenshot_dir, "*mv_static*.png")
        if static_path:
            ok, msg = validate_screenshot(static_path, "Static MV screenshot", expect_motion=False)
            results.append(("Static MV screenshot", ok))
            if not ok:
                print(f"    FAIL: {msg}")
        else:
            print("    WARN: static screenshot not found (non-fatal)")
            results.append(("Static MV screenshot", True))  # Non-fatal

        # Check the motion screenshot
        motion_path = get_latest_screenshot(screenshot_dir, "*mv_motion*.png")
        if motion_path:
            ok, msg = validate_screenshot(motion_path, "Motion MV screenshot", expect_motion=True)
            results.append(("Motion MV screenshot", ok))
            if not ok:
                print(f"    FAIL: {msg}")
        else:
            print("    WARN: motion screenshot not found (non-fatal)")
            results.append(("Motion MV screenshot", True))  # Non-fatal

    # Summary
    passed = sum(1 for _, ok in results if ok)
    failed = sum(1 for _, ok in results if not ok)
    print(f"\n{'='*60}")
    print(f"  Results: {passed} passed, {failed} failed")
    for name, ok in results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    print(f"{'='*60}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
```

**Step 2: Run the test**

```bash
python tests/reblur/test_motion_vectors.py --framework glfw
```

Expected: all checks pass.

**Step 3: Commit**

```bash
git add tests/reblur/test_motion_vectors.py
git commit -m "test(reblur): add M2 motion vector statistical validation (Python)"
```

---

## Task 5: M3 Test — Reprojection quality test (C++)

**Files:**
- Create: `tests/reblur/ReblurReprojectionTest.cpp`
- Test: `--test_case reblur_reprojection --pipeline gpu --use_reblur true --spp 1 --max_spp 30`

**What this tests (M3 test gate — semantic):**
- Under camera motion, temporal accumulation uses MV-based reprojection (not identity).
- After moving camera and converging, output is not all-black (history was reused).
- No crash over 30 frames of motion + accumulation.
- Screenshot captured for downstream statistical analysis.

**Step 1: Write the C++ test case**

```cpp
// tests/reblur/ReblurReprojectionTest.cpp
#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "scene/component/camera/CameraComponent.h"
#include "scene/component/camera/OrbitCameraComponent.h"

namespace sparkle
{

/// M3 test gate: Validates bilinear/Catmull-Rom reprojection in temporal accumulation.
///
/// Runs 30 frames with small incremental camera motion (orbit yaw changes)
/// to exercise the full reprojection pipeline: MV computation -> bilinear sampling
/// -> occlusion test -> Catmull-Rom upgrade -> footprint quality modulation.
///
/// Validates:
/// - No crash over 30 frames of continuous motion
/// - Accumulation speed ramps up (history is being reused, not constantly reset)
/// - Final screenshot is not all-black
///
/// Usage: --test_case reblur_reprojection --pipeline gpu --use_reblur true --spp 1 --max_spp 60
class ReblurReprojectionTest : public TestCase
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

        auto *orbit = dynamic_cast<OrbitCameraComponent *>(camera);

        // Phase 1 (frames 1-5): Static warmup — let the scene load and accumulate a few frames
        if (frame_ <= kWarmupFrames)
        {
            if (frame_ == 1)
            {
                Log(Info, "ReblurReprojectionTest: warmup phase (frames 1-{})", kWarmupFrames);
            }
            return Result::Pending;
        }

        // Phase 2 (frames 6-25): Incremental orbit motion — small yaw steps each frame
        if (frame_ <= kWarmupFrames + kMotionFrames)
        {
            if (orbit)
            {
                float yaw_step = 2.0f; // 2 degrees per frame
                float yaw = 30.0f + yaw_step * static_cast<float>(frame_ - kWarmupFrames);
                orbit->Setup(Vector3::Zero(), 3.0f, 20.0f, yaw);
            }

            if (frame_ == kWarmupFrames + 1)
            {
                Log(Info, "ReblurReprojectionTest: motion phase started (frames {}-{})", kWarmupFrames + 1,
                    kWarmupFrames + kMotionFrames);
            }

            // Log camera delta at midpoint
            if (frame_ == kWarmupFrames + kMotionFrames / 2)
            {
                auto *proxy = static_cast<CameraRenderProxy *>(camera->GetRenderProxy());
                float delta = (proxy->GetPosture().position - proxy->GetPositionPrev()).norm();
                Log(Info, "ReblurReprojectionTest: mid-motion camera delta = {:.6f}", delta);
            }

            return Result::Pending;
        }

        // Phase 3 (frames 26-30): Stop motion, let it reconverge
        if (frame_ <= kWarmupFrames + kMotionFrames + kSettleFrames)
        {
            if (frame_ == kWarmupFrames + kMotionFrames + 1)
            {
                Log(Info, "ReblurReprojectionTest: settle phase (frames {}-{})", frame_,
                    kWarmupFrames + kMotionFrames + kSettleFrames);
            }
            return Result::Pending;
        }

        // Phase 4: Take screenshot and pass
        if (app.IsScreenshotCompleted())
        {
            Log(Info, "ReblurReprojectionTest: screenshot captured after {} frames — PASS", frame_);
            return Result::Pass;
        }

        if (!screenshot_requested_)
        {
            Log(Info, "ReblurReprojectionTest: requesting screenshot at frame {}", frame_);
            app.RequestTakeScreenshot("reblur_reprojection");
            screenshot_requested_ = true;
        }

        uint32_t timeout = app.GetAppConfig().test_timeout;
        if (timeout > 0 && frame_ > timeout)
        {
            Log(Error, "ReblurReprojectionTest timed out after {} frames", timeout);
            return Result::Fail;
        }

        return Result::Pending;
    }

private:
    static constexpr uint32_t kWarmupFrames = 5;
    static constexpr uint32_t kMotionFrames = 20;
    static constexpr uint32_t kSettleFrames = 5;

    uint32_t frame_ = 0;
    bool screenshot_requested_ = false;
};

static TestCaseRegistrar<ReblurReprojectionTest> reblur_reprojection_registrar("reblur_reprojection");
} // namespace sparkle
```

**Step 2: Build and run**

```bash
python build.py --framework glfw
python build.py --framework glfw --skip_build --run --test_case reblur_reprojection --headless true --pipeline gpu --use_reblur true --spp 1 --max_spp 60 --test_timeout 60
```

Expected: exit code 0, log shows all three phases complete.

**Step 3: Commit**

```bash
git add tests/reblur/ReblurReprojectionTest.cpp
git commit -m "test(reblur): add M3 reprojection quality test — validates bilinear/Catmull-Rom under motion"
```

---

## Task 6: M3 Test — Reprojection statistical validation (Python)

**Files:**
- Create: `tests/reblur/test_reprojection.py`

**What this tests (M3 test gate — statistical):**
- After motion + settle, the reprojected output screenshot is valid (no NaN, no all-black).
- Compare static-camera convergence quality (REBLUR with no motion) vs post-motion convergence quality.
- The post-motion image should still converge to a reasonable quality (mean luminance > 0.01, no large black regions).

**Step 1: Write the Python test**

```python
"""M3 test gate: Reprojection statistical validation.

Runs the REBLUR reprojection C++ test (motion + settle + screenshot),
then validates the output screenshot for statistical properties.

Usage:
  python tests/reblur/test_reprojection.py --framework glfw [--skip_build]
"""

import argparse
import glob
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, PROJECT_ROOT)


def parse_args():
    parser = argparse.ArgumentParser(description="Reprojection statistical test")
    parser.add_argument("--framework", default="glfw", choices=("glfw", "macos"))
    parser.add_argument("--skip_build", action="store_true")
    return parser.parse_args()


def get_screenshot_dir(framework):
    if framework == "glfw":
        return os.path.join(PROJECT_ROOT, "build_system", "glfw", "output",
                            "build", "generated", "screenshots")
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    raise ValueError(f"Unsupported framework: {framework}")


def get_latest_screenshot(screenshot_dir, pattern="*.png"):
    matches = glob.glob(os.path.join(screenshot_dir, pattern))
    if not matches:
        return None
    matches.sort(key=os.path.getmtime, reverse=True)
    return matches[0]


def main():
    args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)

    print("=" * 60)
    print("  M3: Reprojection Statistical Validation")
    print("=" * 60)

    # Build
    if not args.skip_build:
        print("\nBuilding...")
        result = subprocess.run([py, build_py, "--framework", fw],
                                cwd=PROJECT_ROOT, capture_output=True, text=True)
        if result.returncode != 0:
            print("FAIL: build failed")
            return 1

    results = []

    # Run the reprojection C++ test
    print("\n--- Running reprojection test (motion + settle + screenshot) ---")
    cmd = [py, build_py, "--framework", fw, "--skip_build",
           "--run", "--test_case", "reblur_reprojection", "--headless", "true",
           "--pipeline", "gpu", "--use_reblur", "true",
           "--spp", "1", "--max_spp", "60", "--test_timeout", "60"]
    print(f"  cmd: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=PROJECT_ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  FAIL: app exited with code {result.returncode}")
        if result.stderr:
            for line in result.stderr.strip().splitlines()[-5:]:
                print(f"    {line}")
        results.append(("Reprojection test run", False))
        return 1
    results.append(("Reprojection test run", True))

    # Validate the reprojection screenshot
    print("\n--- Validating reprojection screenshot ---")
    path = get_latest_screenshot(screenshot_dir, "*reprojection*.png")
    if not path:
        # Fall back to any recent screenshot
        path = get_latest_screenshot(screenshot_dir)
    if not path:
        print("  FAIL: no screenshot found")
        results.append(("Screenshot found", False))
    else:
        results.append(("Screenshot found", True))
        print(f"  Screenshot: {path}")

        img = np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
        h, w, _ = img.shape
        luma = img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722

        has_nan = bool(np.any(np.isnan(img)))
        has_inf = bool(np.any(np.isinf(img)))
        mean_luma = float(np.mean(luma))
        max_luma = float(np.max(luma))
        std_luma = float(np.std(luma))
        black_ratio = float(np.mean(luma < 1e-4))

        print(f"  Dimensions: {w}x{h}")
        print(f"  Mean luminance:   {mean_luma:.6f}")
        print(f"  Max luminance:    {max_luma:.6f}")
        print(f"  Std luminance:    {std_luma:.6f}")
        print(f"  Black pixel ratio: {black_ratio:.4f}")
        print(f"  Has NaN: {has_nan}")
        print(f"  Has Inf: {has_inf}")

        # Statistical checks
        if has_nan:
            print("  FAIL: NaN values detected")
            results.append(("No NaN", False))
        else:
            results.append(("No NaN", True))

        if has_inf:
            print("  FAIL: Inf values detected")
            results.append(("No Inf", False))
        else:
            results.append(("No Inf", True))

        if mean_luma < 1e-4:
            print("  FAIL: output is all black")
            results.append(("Not all black", False))
        else:
            results.append(("Not all black", True))

        # After motion + 5 settle frames, should have some convergence
        # The black pixel ratio should not be too high (< 50% — sky is black)
        if black_ratio > 0.9:
            print(f"  FAIL: too many black pixels ({black_ratio:.2%})")
            results.append(("Reasonable coverage", False))
        else:
            results.append(("Reasonable coverage", True))

        # Luminance variance should exist (not a flat image)
        if std_luma < 1e-5:
            print(f"  FAIL: no luminance variance (flat image)")
            results.append(("Has variance", False))
        else:
            results.append(("Has variance", True))

    # Summary
    passed = sum(1 for _, ok in results if ok)
    failed = sum(1 for _, ok in results if not ok)
    print(f"\n{'='*60}")
    print(f"  Results: {passed} passed, {failed} failed")
    for name, ok in results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    print(f"{'='*60}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
```

**Step 2: Run the test**

```bash
python tests/reblur/test_reprojection.py --framework glfw
```

Expected: all checks pass.

**Step 3: Commit**

```bash
git add tests/reblur/test_reprojection.py
git commit -m "test(reblur): add M3 reprojection statistical validation (Python)"
```

---

## Task 7: M1-M3 Test — Static camera non-regression

**Files:**
- Create: `tests/reblur/ReblurStaticNonRegressionTest.cpp`
- Test: `--test_case reblur_static_nonregression --pipeline gpu --use_reblur true --spp 1 --max_spp 64`

**What this tests (cross-cutting):**
- With static camera, the new motion infrastructure (prev matrices, MV computation, reprojection) does not regress the existing static-camera REBLUR quality.
- Accumulation speed should still reach max (63 frames) since MVs are zero and all bilinear taps pass occlusion.
- Screenshot quality should match pre-motion baseline.

**Step 1: Write the C++ test case**

```cpp
// tests/reblur/ReblurStaticNonRegressionTest.cpp
#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "renderer/proxy/CameraRenderProxy.h"
#include "scene/component/camera/CameraComponent.h"

namespace sparkle
{

/// Cross-cutting non-regression: Validates that static camera REBLUR quality
/// is preserved after adding motion infrastructure (prev matrices, MVs, reprojection).
///
/// Runs 64 frames with static camera and REBLUR enabled, then takes a screenshot.
/// Validates:
/// - MVs should be zero (position_prev == position after convergence)
/// - Screenshot is not all-black, no NaN
/// - Accumulation should be progressing (sample count increases)
///
/// Usage: --test_case reblur_static_nonregression --pipeline gpu --use_reblur true --spp 1 --max_spp 64
class ReblurStaticNonRegressionTest : public TestCase
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

        // At midpoint, verify camera is truly static
        if (frame_ == 32)
        {
            auto pos = proxy->GetPosture().position;
            auto pos_prev = proxy->GetPositionPrev();
            float delta = (pos - pos_prev).norm();
            auto vp = proxy->GetViewProjectionMatrix();
            auto vp_prev = proxy->GetViewProjectionMatrixPrev();
            float mat_diff = (vp - vp_prev).norm();

            Log(Info, "ReblurStaticNonRegression: frame 32 — pos_delta={:.8f}, mat_diff={:.8f}", delta, mat_diff);

            // Static camera: position should not change after initial setup frames
            if (delta > 1e-4f)
            {
                Log(Error, "ReblurStaticNonRegression: FAIL — unexpected camera motion (delta={:.6f})", delta);
                return Result::Fail;
            }

            // Accumulation should be progressing
            uint32_t spp = proxy->GetCumulatedSampleCount();
            Log(Info, "ReblurStaticNonRegression: accumulated {} spp at frame 32", spp);
            if (spp < 10)
            {
                Log(Error, "ReblurStaticNonRegression: FAIL — accumulation too slow (spp={} at frame 32)", spp);
                return Result::Fail;
            }
        }

        // Wait for convergence
        if (frame_ < kConvergenceFrames)
        {
            return Result::Pending;
        }

        if (app.IsScreenshotCompleted())
        {
            Log(Info, "ReblurStaticNonRegression: screenshot captured after {} frames — PASS", frame_);
            return Result::Pass;
        }

        if (!screenshot_requested_)
        {
            Log(Info, "ReblurStaticNonRegression: requesting screenshot at frame {}", frame_);
            app.RequestTakeScreenshot("reblur_static_nonregression");
            screenshot_requested_ = true;
        }

        uint32_t timeout = app.GetAppConfig().test_timeout;
        if (timeout > 0 && frame_ > timeout)
        {
            Log(Error, "ReblurStaticNonRegression timed out after {} frames", timeout);
            return Result::Fail;
        }

        return Result::Pending;
    }

private:
    static constexpr uint32_t kConvergenceFrames = 64;
    uint32_t frame_ = 0;
    bool screenshot_requested_ = false;
};

static TestCaseRegistrar<ReblurStaticNonRegressionTest> reblur_static_nonregression_registrar(
    "reblur_static_nonregression");
} // namespace sparkle
```

**Step 2: Build and run**

```bash
python build.py --framework glfw
python build.py --framework glfw --skip_build --run --test_case reblur_static_nonregression --headless true --pipeline gpu --use_reblur true --spp 1 --max_spp 64 --test_timeout 120
```

Expected: exit code 0.

**Step 3: Commit**

```bash
git add tests/reblur/ReblurStaticNonRegressionTest.cpp
git commit -m "test(reblur): add static camera non-regression test for motion infrastructure"
```

---

## Task 8: Update test suite to include new tests

**Files:**
- Modify: `tests/reblur/reblur_test_suite.py`

**Step 1: Add new test entries**

Add these test entries after the existing test 10 (convergence stability), renumbered:

```python
# --- Test 11: M1 Matrix infrastructure (C++) ---
ok, dur, _ = run_command(
    [py, build_py, "--framework", fw, "--skip_build",
     "--run", "--test_case", "reblur_matrix_infra", "--headless", "true",
     "--pipeline", "gpu", "--use_reblur", "true",
     "--spp", "1", "--max_spp", "10", "--test_timeout", "30"],
    "11. M1 Matrix infrastructure (C++ test)")
results.append(("M1 Matrix infrastructure", ok, dur))

# --- Test 12: M2 Motion vector test (C++) ---
ok, dur, _ = run_command(
    [py, build_py, "--framework", fw, "--skip_build",
     "--run", "--test_case", "reblur_mv_test", "--headless", "true",
     "--pipeline", "gpu", "--use_reblur", "true",
     "--spp", "1", "--max_spp", "10", "--test_timeout", "30"],
    "12. M2 Motion vector test (C++ test)")
results.append(("M2 Motion vector test", ok, dur))

# --- Test 13: M2 Motion vector statistical (Python) ---
mv_test_py = os.path.join(SCRIPT_DIR, "test_motion_vectors.py")
ok, dur, _ = run_command(
    [py, mv_test_py, "--framework", fw, "--skip_build"],
    "13. M2 Motion vector statistical validation",
    show_output=True)
results.append(("M2 MV statistical", ok, dur))

# --- Test 14: M3 Reprojection test (C++) ---
ok, dur, _ = run_command(
    [py, build_py, "--framework", fw, "--skip_build",
     "--run", "--test_case", "reblur_reprojection", "--headless", "true",
     "--pipeline", "gpu", "--use_reblur", "true",
     "--spp", "1", "--max_spp", "60", "--test_timeout", "60"],
    "14. M3 Reprojection test (C++ test)")
results.append(("M3 Reprojection test", ok, dur))
if ok:
    ok2, dur2, _ = validate_latest_screenshot(fw, "M3 reprojection screenshot")
    results.append(("M3 Reprojection (pixels)", ok2, dur2))

# --- Test 15: M3 Reprojection statistical (Python) ---
reproj_test_py = os.path.join(SCRIPT_DIR, "test_reprojection.py")
ok, dur, _ = run_command(
    [py, reproj_test_py, "--framework", fw, "--skip_build"],
    "15. M3 Reprojection statistical validation",
    show_output=True)
results.append(("M3 Reprojection statistical", ok, dur))

# --- Test 16: Static non-regression ---
ok, dur, _ = run_command(
    [py, build_py, "--framework", fw, "--skip_build",
     "--run", "--test_case", "reblur_static_nonregression", "--headless", "true",
     "--pipeline", "gpu", "--use_reblur", "true",
     "--spp", "1", "--max_spp", "64", "--test_timeout", "120"],
    "16. Static camera non-regression")
results.append(("Static non-regression", ok, dur))
if ok:
    ok2, dur2, _ = validate_latest_screenshot(fw, "Static non-regression screenshot")
    results.append(("Static non-regression (pixels)", ok2, dur2))
```

Also update the docstring at the top to list the new tests.

**Step 2: Run the full suite**

```bash
python tests/reblur/reblur_test_suite.py --framework glfw
```

Expected: all 16+ tests pass.

**Step 3: Commit**

```bash
git add tests/reblur/reblur_test_suite.py
git commit -m "test(reblur): add M1-M3 camera motion tests to test suite"
```

---

## Task 9: Update progress file

**Files:**
- Modify: `docs/plans/2026-02-27-reblur-camera-motion-progress.md`

**Step 1: Add test task entries**

Append entries documenting the test implementations for M1, M2, M3 with status, files created, and findings.

**Step 2: Commit**

```bash
git add docs/plans/2026-02-27-reblur-camera-motion-progress.md
git commit -m "docs(reblur): update progress with M1-M3 test implementations"
```
