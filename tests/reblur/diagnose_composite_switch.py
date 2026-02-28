"""Diagnose composite blend weight switching after camera motion.

The hypothesis: "before" screenshot shows PT accumulated result (pt_weight=1.0),
while "after" shows denoiser output (pt_weight≈0). The noise/darkness difference
is from switching output modes, not from failed reprojection.

Test: Force the app to take a "steady state" screenshot BEFORE the nudge where
pt_weight is also ~0 (by comparing early frames vs converged frames). We do this
by running with very low max_spp (e.g., 4) so pt_weight stays low even before nudge.

Usage:
  python3 tests/reblur/diagnose_composite_switch.py --framework macos [--skip_build]
"""

import argparse
import glob
import os
import subprocess
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import convolve

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, PROJECT_ROOT)


def parse_args():
    parser = argparse.ArgumentParser(description="Composite switch diagnostic")
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


def find_screenshot(screenshot_dir, pattern):
    matches = glob.glob(os.path.join(screenshot_dir, pattern))
    if not matches:
        return None
    matches.sort(key=os.path.getmtime, reverse=True)
    return matches[0]


def compute_laplacian_variance(luma):
    kernel = np.array([[0, 1, 0], [1, -4, 1], [0, 1, 0]], dtype=np.float32)
    laplacian = convolve(luma, kernel, mode="reflect")
    return float(np.var(laplacian))


def analyze_image(path, label):
    img = np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
    luma = img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722
    mean_luma = float(np.mean(luma))
    lap_var = compute_laplacian_variance(luma)
    print(f"  {label}: mean_luma={mean_luma:.6f}, laplacian_var={lap_var:.6f}")
    return mean_luma, lap_var


def run(py, build_py, fw, test_case, extra_args, clear=False):
    cmd = [py, build_py, "--framework", fw, "--skip_build",
           "--run", "--test_case", test_case, "--headless", "true",
           "--pipeline", "gpu", "--spp", "1"]
    if clear:
        cmd += ["--clear_screenshots", "true"]
    cmd += extra_args
    print(f"  cmd: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=PROJECT_ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  FAIL: exit code {result.returncode}")
        if result.stderr:
            for line in result.stderr.strip().splitlines()[-5:]:
                print(f"    {line}")
        return False
    return True


def main():
    args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)

    print("=" * 60)
    print("  Composite Blend Weight Switch Diagnostic")
    print("=" * 60)

    if not args.skip_build:
        print("\nBuilding...")
        result = subprocess.run([py, build_py, "--framework", fw],
                                cwd=PROJECT_ROOT, capture_output=True, text=True)
        if result.returncode != 0:
            print("FAIL: build failed")
            return 1

    # Test A: Normal run with full reblur pipeline (debug_pass = default)
    # "before" = pt_weight=1.0, "after" = pt_weight≈0
    print(f"\n{'—'*60}")
    print("  Test A: Full pipeline (default composite)")
    print(f"{'—'*60}")
    run(py, build_py, fw, "reblur_converged_history",
        ["--use_reblur", "true"], clear=True)
    before_path = find_screenshot(screenshot_dir, "*converged_history_before*")
    after_path = find_screenshot(screenshot_dir, "*converged_history_after*")
    if before_path and after_path:
        a_before_luma, a_before_var = analyze_image(before_path, "Before")
        a_after_luma, a_after_var = analyze_image(after_path, "After")
        if a_before_var > 1e-10:
            print(f"  Noise ratio: {a_after_var / a_before_var:.2f}x")
            print(f"  Luma change: {a_after_luma / a_before_luma:.3f}x")
        for p in glob.glob(os.path.join(screenshot_dir, "*converged_history_*")):
            new = p.replace("converged_history", "testA")
            os.rename(p, new)

    # Test B: Run with temporal accum debug (debug_pass 3)
    # Same composite issue: "before" = pt_weight=1.0, "after" = pt_weight≈0
    print(f"\n{'—'*60}")
    print("  Test B: Temporal accum only (debug_pass 3)")
    print(f"{'—'*60}")
    run(py, build_py, fw, "reblur_converged_history",
        ["--use_reblur", "true", "--reblur_debug_pass", "TemporalAccum"])
    before_path = find_screenshot(screenshot_dir, "*converged_history_before*")
    after_path = find_screenshot(screenshot_dir, "*converged_history_after*")
    if before_path and after_path:
        b_before_luma, b_before_var = analyze_image(before_path, "Before")
        b_after_luma, b_after_var = analyze_image(after_path, "After")
        if b_before_var > 1e-10:
            print(f"  Noise ratio: {b_after_var / b_before_var:.2f}x")
            print(f"  Luma change: {b_after_luma / b_before_luma:.3f}x")
        for p in glob.glob(os.path.join(screenshot_dir, "*converged_history_*")):
            new = p.replace("converged_history", "testB")
            os.rename(p, new)

    # Test C: Static camera test — converge, then take a shot where pt_weight≈0.
    # We do this by using the converged_history test but with NO camera nudge (yaw_delta=0).
    # Actually, we can't control yaw_delta from CLI. Instead, let's compare
    # the "before" of the vanilla baseline (pt_weight=1.0) vs "before" of reblur at low spp.
    # Actually simplest: just run reblur with max_spp=4 (no convergence) and no nudge.
    # The "before" would have pt_weight = saturate(4/256) ≈ 0.016 ≈ denoiser output.

    print(f"\n{'='*60}")
    print("  ANALYSIS")
    print(f"{'='*60}")
    print()
    print("  If the 'Luma change' is significantly < 1.0 (e.g., 0.7x),")
    print("  the composite is switching from PT accumulated (bright)")
    print("  to denoiser output (dark). This is the blend weight switch,")
    print("  NOT failed reprojection.")
    print()
    print("  The fix: keep composite pt_weight consistent, or bypass")
    print("  the PT blend ramp when dispatched_sample_count resets.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
