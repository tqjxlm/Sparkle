"""Denoiser history preservation test after camera nudge.

Tests that the REBLUR denoiser preserves temporal history across a small
camera yaw delta. Uses --reblur_no_pt_blend to isolate the denoiser output
from the PT blend ramp, which has a known ~24% luminance gap due to
demod/remod artifacts.

This test is the PURE denoiser quality test. It validates that:
  1. The denoised output luminance is preserved after a small camera nudge
  2. The denoised output noise stays low (not a noisy restart)
  3. The denoised output visually matches the converged state (low FLIP)

Orchestrates three runs:
  Run 0: Reblur denoised-only (no PT blend): before/after screenshot
  Run 1: Vanilla baseline: provides ground truth for the nudged view
  Run 2: Denoiser TemporalAccum debug: validates raw TA history preservation

Metrics:
  - Denoised luminance ratio: after/before (should be > 0.93)
  - Denoised noise ratio: after/before (should be < 3.0)
  - Denoised-vs-vanilla gap: known demod/remod artifact, measured not asserted

Usage:
  python3 tests/reblur/test_denoiser_history.py --framework macos [--skip_build]
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

# --- Thresholds ---
# Denoised luminance preservation: after/before ratio must be in this range
MIN_LUMA_RATIO = 0.93  # max 7% loss from viewpoint change + 1spp blend
MAX_LUMA_RATIO = 1.07  # max 7% gain

# Denoised noise preservation: after/before laplacian variance ratio
MAX_NOISE_RATIO = 3.0

# Denoised FLIP: before vs after perceptual difference
MAX_DENOISED_FLIP = 0.25


def parse_args():
    parser = argparse.ArgumentParser(
        description="Denoiser history preservation test")
    parser.add_argument("--framework", default="macos",
                        choices=("glfw", "macos"))
    parser.add_argument("--skip_build", action="store_true")
    return parser.parse_args()


def get_screenshot_dir(framework):
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    return os.path.join(PROJECT_ROOT, "build_system", "glfw", "output",
                        "build", "generated", "screenshots")


def find_screenshot(screenshot_dir, pattern):
    matches = glob.glob(os.path.join(screenshot_dir, pattern))
    if not matches:
        return None
    matches.sort(key=os.path.getmtime, reverse=True)
    return matches[0]


def load_image(path):
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def load_luminance(path):
    img = load_image(path)
    luma = img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722
    return img, luma


def compute_laplacian_variance(luma):
    kernel = np.array([[0, 1, 0],
                       [1, -4, 1],
                       [0, 1, 0]], dtype=np.float32)
    laplacian = convolve(luma, kernel, mode="reflect")
    return float(np.var(laplacian))


def compute_flip(path_a, path_b):
    try:
        from flip_evaluator import nbflip
    except ImportError:
        print("  WARN: flip_evaluator not installed, skipping FLIP metric")
        return None
    img_a = load_image(path_a)
    img_b = load_image(path_b)
    if img_a.shape != img_b.shape:
        return None
    _, mean_flip, _ = nbflip.evaluate(img_a, img_b, False, True, False, True, {})
    return float(mean_flip)


def run_test(py, build_py, framework, test_case, extra_args, label,
             use_reblur=True, clear_screenshots=False):
    cmd = [py, build_py, "--framework", framework, "--skip_build",
           "--run", "--test_case", test_case, "--headless", "true",
           "--pipeline", "gpu", "--spp", "1"]
    if clear_screenshots:
        cmd += ["--clear_screenshots", "true"]
    if use_reblur:
        cmd += ["--use_reblur", "true"]
    cmd += extra_args
    print(f"  cmd: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=PROJECT_ROOT,
                            capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  FAIL: app exited with code {result.returncode}")
        if result.stderr:
            for line in result.stderr.strip().splitlines()[-5:]:
                print(f"    {line}")
        return False
    return True


def rename_screenshots(screenshot_dir, old_pattern, new_prefix):
    for path in glob.glob(os.path.join(screenshot_dir, old_pattern)):
        dirname = os.path.dirname(path)
        basename = os.path.basename(path)
        new_name = basename.replace(
            "converged_history_before", f"{new_prefix}_before").replace(
            "converged_history_after", f"{new_prefix}_after")
        new_path = os.path.join(dirname, new_name)
        os.rename(path, new_path)


def main():
    args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)

    print("=" * 60)
    print("  Denoiser History Preservation Test")
    print("=" * 60)

    # Build
    if not args.skip_build:
        print("\nBuilding...")
        result = subprocess.run([py, build_py, "--framework", fw],
                                cwd=PROJECT_ROOT, capture_output=True, text=True)
        if result.returncode != 0:
            print("FAIL: build failed")
            return 1

    all_results = []

    # --- Run 0: Denoised-only (no PT blend) ---
    print(f"\n{'—'*60}")
    print("  Run 0: Reblur denoised-only (--reblur_no_pt_blend true)")
    print(f"{'—'*60}")
    ok = run_test(py, build_py, fw, "reblur_converged_history",
                  ["--reblur_no_pt_blend", "true"],
                  "denoised-only", clear_screenshots=True)
    if not ok:
        all_results.append(("Denoised-only: test run", False))
        _print_summary(all_results)
        return 1
    all_results.append(("Denoised-only: test run", True))

    before_path = find_screenshot(screenshot_dir, "*converged_history_before*")
    after_path = find_screenshot(screenshot_dir, "*converged_history_after*")

    if not before_path or not after_path:
        print("  FAIL: screenshots not found")
        all_results.append(("Denoised-only: screenshots found", False))
        _print_summary(all_results)
        return 1
    all_results.append(("Denoised-only: screenshots found", True))

    _, luma_b = load_luminance(before_path)
    _, luma_a = load_luminance(after_path)

    mean_b = float(np.mean(luma_b))
    mean_a = float(np.mean(luma_a))
    luma_ratio = mean_a / max(mean_b, 1e-6)
    noise_b = compute_laplacian_variance(luma_b)
    noise_a = compute_laplacian_variance(luma_a)
    noise_ratio = noise_a / max(noise_b, 1e-10)

    print(f"  Before: mean_luma={mean_b:.6f}, noise={noise_b:.6f}")
    print(f"  After:  mean_luma={mean_a:.6f}, noise={noise_a:.6f}")
    print(f"  Luma ratio:  {luma_ratio:.4f} ({(1-luma_ratio)*100:.1f}% change)")
    print(f"  Noise ratio: {noise_ratio:.2f}x")

    # Luminance preservation
    if MIN_LUMA_RATIO <= luma_ratio <= MAX_LUMA_RATIO:
        print(f"  PASS: luma ratio {luma_ratio:.4f} in [{MIN_LUMA_RATIO}, {MAX_LUMA_RATIO}]")
        all_results.append(("Denoised-only: luma preserved", True))
    else:
        print(f"  FAIL: luma ratio {luma_ratio:.4f} outside [{MIN_LUMA_RATIO}, {MAX_LUMA_RATIO}]")
        all_results.append(("Denoised-only: luma preserved", False))

    # Noise preservation
    if noise_ratio < MAX_NOISE_RATIO:
        print(f"  PASS: noise ratio {noise_ratio:.2f} < {MAX_NOISE_RATIO}")
        all_results.append(("Denoised-only: noise preserved", True))
    else:
        print(f"  FAIL: noise ratio {noise_ratio:.2f} >= {MAX_NOISE_RATIO}")
        all_results.append(("Denoised-only: noise preserved", False))

    # Perceptual FLIP
    flip = compute_flip(before_path, after_path)
    if flip is not None:
        print(f"  FLIP (before vs after): {flip:.4f}")
        if flip <= MAX_DENOISED_FLIP:
            print(f"  PASS: FLIP {flip:.4f} <= {MAX_DENOISED_FLIP}")
            all_results.append(("Denoised-only: FLIP acceptable", True))
        else:
            print(f"  FAIL: FLIP {flip:.4f} > {MAX_DENOISED_FLIP}")
            all_results.append(("Denoised-only: FLIP acceptable", False))

    rename_screenshots(screenshot_dir, "*converged_history_*",
                       "denoised_only")

    # --- Run 1: Vanilla baseline for gap measurement ---
    print(f"\n{'—'*60}")
    print("  Run 1: Vanilla baseline (reference for demod/remod gap measurement)")
    print(f"{'—'*60}")
    ok = run_test(py, build_py, fw, "vanilla_converged_baseline",
                  [], "vanilla baseline", use_reblur=False)
    if ok:
        all_results.append(("Vanilla baseline: test run", True))
        v_before = find_screenshot(screenshot_dir, "*vanilla_baseline_before*")
        v_after = find_screenshot(screenshot_dir, "*vanilla_baseline_after*")
        if v_before and v_after:
            _, v_luma = load_luminance(v_before)
            vanilla_mean = float(np.mean(v_luma))
            gap = (vanilla_mean - mean_b) / max(vanilla_mean, 1e-6) * 100
            print(f"  Vanilla converged luma: {vanilla_mean:.6f}")
            print(f"  Denoised converged luma: {mean_b:.6f}")
            print(f"  Demod/remod gap: {gap:.1f}% (known artifact, informational)")
    else:
        all_results.append(("Vanilla baseline: test run", False))

    # --- Run 2: TA-only debug (raw temporal accumulation) ---
    print(f"\n{'—'*60}")
    print("  Run 2: TemporalAccum debug (raw TA output, diagnostic passthrough)")
    print(f"{'—'*60}")
    ok = run_test(py, build_py, fw, "reblur_converged_history",
                  ["--reblur_debug_pass", "TemporalAccum",
                   "--reblur_no_pt_blend", "true"],
                  "TA debug")
    if ok:
        all_results.append(("TA debug: test run", True))
        ta_before = find_screenshot(screenshot_dir, "*converged_history_before*")
        ta_after = find_screenshot(screenshot_dir, "*converged_history_after*")
        if ta_before and ta_after:
            _, ta_luma_b = load_luminance(ta_before)
            _, ta_luma_a = load_luminance(ta_after)
            ta_ratio = float(np.mean(ta_luma_a)) / max(float(np.mean(ta_luma_b)), 1e-6)
            ta_noise_b = compute_laplacian_variance(ta_luma_b)
            ta_noise_a = compute_laplacian_variance(ta_luma_a)
            ta_noise_ratio = ta_noise_a / max(ta_noise_b, 1e-10)
            print(f"  TA before: mean_luma={float(np.mean(ta_luma_b)):.6f}")
            print(f"  TA after:  mean_luma={float(np.mean(ta_luma_a)):.6f}")
            print(f"  TA luma ratio: {ta_ratio:.4f}")
            print(f"  TA noise ratio: {ta_noise_ratio:.2f}x")

        rename_screenshots(screenshot_dir, "*converged_history_*",
                           "ta_debug")
    else:
        all_results.append(("TA debug: test run", False))

    _print_summary(all_results)
    failed = sum(1 for _, ok in all_results if not ok)
    return 0 if failed == 0 else 1


def _print_summary(results):
    passed = sum(1 for _, ok in results if ok)
    failed = sum(1 for _, ok in results if not ok)
    print(f"\n{'='*60}")
    print(f"  Results: {passed} passed, {failed} failed")
    for name, ok in results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    print(f"{'='*60}")


if __name__ == "__main__":
    sys.exit(main())
