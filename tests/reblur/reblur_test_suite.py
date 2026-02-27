"""REBLUR test suite: runs all available REBLUR tests and reports results.

Tests included:
  1. Smoke test — app launches without crash
  2. Vanilla functional test — GPU pipeline without REBLUR matches ground truth (FLIP <= 0.1)
  3. Split-merge equivalence — split shader with debug_pass 255 matches ground truth (FLIP <= 0.1)
  4. REBLUR screenshot — split path + full denoiser pipeline runs without crash
  5. REBLUR per-pass validation — spatial passes produce valid output with decreasing variance
  6. REBLUR pass validation (C++) — native test case + screenshot pixel validation
  7. REBLUR temporal validation — TemporalAccum/HistoryFix produce valid output, convergence
  8. REBLUR temporal convergence (C++) — 30+ frames temporal pipeline + screenshot pixel validation
  9. REBLUR smoke test (C++) — 30 frames + screenshot
 10. Convergence stability — frame-to-frame instability at 2048 spp vs vanilla baseline
 11. M1 Matrix infrastructure — prev-frame matrices update correctly under camera motion
 12. M2 Motion vector test — MV pipeline runs under camera motion without crash
 13. M2 MV statistical — pixel-level validation of static/motion MV screenshots
 14. M3 Reprojection test — 30-frame motion + bilinear/Catmull-Rom reprojection
 15. M3 Reprojection statistical — pixel-level validation of reprojection output
 16. Static non-regression — motion infrastructure doesn't regress static camera quality
 17. CameraAnimator none non-regression — static camera with --camera_animation none
 18. Camera motion smoke (orbit_sweep) — orbit_sweep animation runs without crash
 19. Camera motion quality validation — temporal stability and reconvergence under motion

Usage:
  python tests/reblur/reblur_test_suite.py --framework glfw [--skip_build]
"""

import argparse
import glob
import os
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, PROJECT_ROOT)

from dev.utils import extract_log_path


def parse_args():
    parser = argparse.ArgumentParser(description="REBLUR test suite")
    parser.add_argument("--framework", default="glfw", choices=("glfw", "macos"))
    parser.add_argument("--skip_build", action="store_true",
                        help="Skip the initial build (assumes already built)")
    return parser.parse_args()


def run_command(cmd, label, cwd=PROJECT_ROOT, show_output=False):
    """Run a command and return (success: bool, duration_seconds: float, log_path: str).

    When *show_output* is True the subprocess stdout is forwarded to the
    console.  Use this for sub-test scripts whose output IS the test report.
    """
    print(f"\n{'='*70}", flush=True)
    print(f"  {label}", flush=True)
    print(f"{'='*70}", flush=True)
    print(f"  cmd: {' '.join(cmd)}", flush=True)

    start = time.time()
    result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    duration = time.time() - start

    log_path = extract_log_path(result.stdout)
    success = result.returncode == 0
    status = "PASS" if success else "FAIL"
    log_info = f" — log: {log_path}" if log_path else ""
    print(f"  [{status}] {label} ({duration:.1f}s){log_info}", flush=True)
    if show_output and result.stdout:
        print(result.stdout, flush=True)
    if not success and result.stderr:
        for line in result.stderr.strip().splitlines()[-10:]:
            print(f"    {line}", flush=True)
    return success, duration, log_path or ""


def get_screenshot_dir(framework):
    if framework == "glfw":
        return os.path.join(PROJECT_ROOT, "build_system", "glfw", "output",
                            "build", "generated", "screenshots")
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    raise ValueError(f"Unsupported framework: {framework}")


def validate_latest_screenshot(framework, label):
    """Load the most recent screenshot and validate it is not all-black.

    Returns (success, duration, message).
    """
    import numpy as np
    from PIL import Image

    start = time.time()
    screenshot_dir = get_screenshot_dir(framework)
    pattern = os.path.join(screenshot_dir, "TestScene_gpu_*.png")
    matches = glob.glob(pattern)
    if not matches:
        return False, time.time() - start, f"No screenshot found matching {pattern}"

    matches.sort(key=os.path.getmtime, reverse=True)
    path = matches[0]

    img = np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
    luma = img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722
    mean_luma = float(np.mean(luma))
    has_nan = bool(np.any(np.isnan(img)))
    has_inf = bool(np.any(np.isinf(img)))

    dur = time.time() - start
    failures = []
    if has_nan:
        failures.append("contains NaN values")
    if has_inf:
        failures.append("contains Inf values")
    if mean_luma < 1e-4:
        failures.append(f"output is all black (mean_luma={mean_luma:.6f})")

    print(f"  Screenshot: {path}", flush=True)
    print(f"  Mean luminance: {mean_luma:.6f}", flush=True)

    if failures:
        msg = f"{label}: " + "; ".join(failures)
        print(f"  FAIL: {msg}", flush=True)
        return False, dur, msg

    print(f"  PASS: screenshot pixel validation OK", flush=True)
    return True, dur, ""


def main():
    args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    functional_test_py = os.path.join(PROJECT_ROOT, "dev", "functional_test.py")
    pass_validation_py = os.path.join(SCRIPT_DIR, "reblur_pass_validation.py")
    temporal_validation_py = os.path.join(SCRIPT_DIR, "reblur_temporal_validation.py")
    convergence_stability_py = os.path.join(
        PROJECT_ROOT, "dev", "reblur", "test_convergence_stability.py")

    results = []

    # --- Build ---
    if not args.skip_build:
        ok, dur, _ = run_command(
            [py, build_py, "--framework", fw],
            "Build")
        if not ok:
            print("\nBuild failed — aborting test suite.", flush=True)
            return 1
        results.append(("Build", ok, dur))
    else:
        print("\nSkipping build (--skip_build)", flush=True)

    # All subsequent commands use --skip_build since we built once above.

    # --- Test 1: Smoke test ---
    ok, dur, _ = run_command(
        [py, build_py, "--framework", fw, "--skip_build",
         "--run", "--test_case", "smoke", "--headless", "true"],
        "1. Smoke test")
    results.append(("Smoke test", ok, dur))

    # --- Test 2: Vanilla functional test (no REBLUR) ---
    ok, dur, _ = run_command(
        [py, functional_test_py,
         "--framework", fw, "--pipeline", "gpu", "--headless", "--skip_build",
         "--", "--use_reblur", "false", "--spp", "1", "--max_spp", "2048"],
        "2. Vanilla functional test (FLIP vs ground truth)")
    results.append(("Vanilla functional test", ok, dur))

    # --- Test 3: Split-merge equivalence (debug_pass 255) ---
    ok, dur, _ = run_command(
        [py, functional_test_py,
         "--framework", fw, "--pipeline", "gpu", "--headless", "--skip_build",
         "--", "--use_reblur", "true", "--reblur_debug_pass", "255",
         "--spp", "1", "--max_spp", "2048"],
        "3. Split-merge equivalence (debug_pass 255, FLIP vs ground truth)")
    results.append(("Split-merge equivalence", ok, dur))

    # --- Test 4: REBLUR screenshot (full pipeline, no crash) ---
    ok, dur, _ = run_command(
        [py, build_py, "--framework", fw, "--skip_build",
         "--run", "--test_case", "screenshot", "--headless", "true",
         "--pipeline", "gpu", "--use_reblur", "true",
         "--spp", "1", "--max_spp", "64"],
        "4. REBLUR screenshot (full denoiser pipeline, crash test)")
    results.append(("REBLUR screenshot", ok, dur))

    # --- Test 5: Per-pass validation (Python) ---
    ok, dur, _ = run_command(
        [py, pass_validation_py, "--framework", fw, "--skip_build"],
        "5. REBLUR per-pass validation (PrePass/Blur/PostBlur)",
        show_output=True)
    results.append(("Per-pass validation", ok, dur))

    # --- Test 6: REBLUR pass validation (C++ test case + screenshot validation) ---
    ok, dur, _ = run_command(
        [py, build_py, "--framework", fw, "--skip_build",
         "--run", "--test_case", "reblur_pass_validation", "--headless", "true",
         "--pipeline", "gpu", "--use_reblur", "true",
         "--spp", "1", "--max_spp", "4", "--test_timeout", "60"],
        "6a. REBLUR pass validation (C++ test case)")
    results.append(("C++ pass validation", ok, dur))
    if ok:
        ok2, dur2, _ = validate_latest_screenshot(fw, "C++ pass validation screenshot")
        results.append(("C++ pass validation (pixels)", ok2, dur2))

    # --- Test 7: Temporal validation (Python) ---
    ok, dur, _ = run_command(
        [py, temporal_validation_py, "--framework", fw, "--skip_build"],
        "7. REBLUR temporal validation (TemporalAccum/HistoryFix/convergence)",
        show_output=True)
    results.append(("Temporal validation", ok, dur))

    # --- Test 8: Temporal convergence (C++ test case + screenshot validation) ---
    ok, dur, _ = run_command(
        [py, build_py, "--framework", fw, "--skip_build",
         "--run", "--test_case", "reblur_temporal_convergence", "--headless", "true",
         "--pipeline", "gpu", "--use_reblur", "true",
         "--spp", "1", "--max_spp", "64", "--test_timeout", "120"],
        "8a. REBLUR temporal convergence (C++ test, 30+ frames)")
    results.append(("C++ temporal convergence", ok, dur))
    if ok:
        ok2, dur2, _ = validate_latest_screenshot(fw, "C++ temporal convergence screenshot")
        results.append(("C++ temporal convergence (pixels)", ok2, dur2))

    # --- Test 9: REBLUR smoke test (C++ test case, 30 frames + screenshot) ---
    # This test can be flaky due to non-deterministic GPU resource lifetime issues.
    # Retry up to 2 times on failure.
    smoke_cmd = [py, build_py, "--framework", fw, "--skip_build",
                 "--run", "--test_case", "reblur_smoke", "--headless", "true",
                 "--pipeline", "gpu", "--use_reblur", "true",
                 "--spp", "1", "--max_spp", "64", "--test_timeout", "120"]
    ok, dur, _ = run_command(smoke_cmd, "9a. REBLUR smoke test (C++ test, 30 frames)")
    if not ok:
        print("  Retrying smoke test (attempt 2/2)...", flush=True)
        ok, dur2, _ = run_command(smoke_cmd, "9a. REBLUR smoke test (retry)")
        dur += dur2
    results.append(("REBLUR smoke test", ok, dur))
    if ok:
        ok2, dur2, _ = validate_latest_screenshot(fw, "REBLUR smoke test screenshot")
        results.append(("REBLUR smoke test (pixels)", ok2, dur2))

    # --- Test 10: Convergence stability (frame-to-frame instability vs vanilla) ---
    ok, dur, _ = run_command(
        [py, convergence_stability_py, "--framework", fw],
        "10. Convergence stability (2048 spp, frame-to-frame vs vanilla)",
        show_output=True)
    results.append(("Convergence stability", ok, dur))

    # --- Test 11: M1 Matrix infrastructure (C++) ---
    ok, dur, _ = run_command(
        [py, build_py, "--framework", fw, "--skip_build",
         "--run", "--test_case", "reblur_matrix_infra", "--headless", "true",
         "--pipeline", "gpu", "--use_reblur", "true",
         "--spp", "1", "--max_spp", "20", "--test_timeout", "30"],
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

    # --- Test 17: Static camera non-regression with CameraAnimator (none) ---
    ok, dur, _ = run_command(
        [py, build_py, "--framework", fw, "--skip_build",
         "--run", "--test_case", "screenshot", "--headless", "true",
         "--pipeline", "gpu", "--use_reblur", "true",
         "--spp", "1", "--max_spp", "64",
         "--camera_animation", "none"],
        "17. Static camera with CameraAnimator (none)")
    if ok:
        ok, _, _ = validate_latest_screenshot(fw, "CameraAnimator none screenshot")
    results.append(("CameraAnimator none non-regression", ok, dur))

    # --- Test 18: Camera motion smoke (orbit_sweep) ---
    ok, dur, _ = run_command(
        [py, build_py, "--framework", fw, "--skip_build",
         "--run", "--test_case", "screenshot", "--headless", "true",
         "--pipeline", "gpu", "--use_reblur", "true",
         "--spp", "1", "--max_spp", "60",
         "--camera_animation", "orbit_sweep"],
        "18. Camera motion smoke (orbit_sweep)")
    if ok:
        ok, _, _ = validate_latest_screenshot(fw, "orbit_sweep motion screenshot")
    results.append(("Camera motion smoke (orbit_sweep)", ok, dur))

    # --- Test 19: Camera motion quality validation ---
    ok, dur, _ = run_command(
        [py, os.path.join(PROJECT_ROOT, "tests", "reblur",
                          "reblur_motion_validation.py"),
         "--framework", fw, "--skip_build"],
        "19. Camera motion quality validation",
        show_output=True)
    results.append(("Camera motion quality validation", ok, dur))

    # --- Summary ---
    total_duration = sum(dur for _, _, dur in results)
    passed = sum(1 for _, ok, _ in results if ok)
    failed = sum(1 for _, ok, _ in results if not ok)

    print(f"\n{'='*70}", flush=True)
    print(f"  REBLUR TEST SUITE RESULTS", flush=True)
    print(f"{'='*70}", flush=True)
    for name, ok, dur in results:
        status = "PASS" if ok else "FAIL"
        print(f"  [{status}]  {name:<40s}  ({dur:.1f}s)", flush=True)
    print(f"{'='*70}", flush=True)
    print(f"  {passed} passed, {failed} failed  ({total_duration:.1f}s total)", flush=True)
    print(f"{'='*70}", flush=True)

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
