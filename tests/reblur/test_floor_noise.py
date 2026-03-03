"""Floor noise quality test for REBLUR denoiser.

Validates that the denoised output floor region has acceptable noise levels
compared to the vanilla (PT-converged) reference. Specifically targets the
temporal stabilization quality — at convergence, the stabilized frame counter
should accumulate well beyond the TA accum_speed (63) up to
max_stabilized_frame_num (255), providing sqrt(255) ≈ 16x noise reduction.

Runs two captures:
  1. Vanilla baseline: converge, screenshot (ground truth floor quality)
  2. Denoised-only: converge with REBLUR + --reblur_no_pt_blend, screenshot

Metrics (floor region = lower 40%, moderate luminance):
  - Floor noise ratio: denoised_lap_var / vanilla_lap_var (< 5.0)
  - Floor local_std: mean per-pixel 5x5 std (< 0.045)
  - Floor luminance: should be within 40% of vanilla (demod/remod gap known)

Usage:
  python tests/reblur/test_floor_noise.py --framework macos [--skip_build]
"""

import argparse
import glob
import os
import subprocess
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import convolve, uniform_filter

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, PROJECT_ROOT)

# Thresholds (stab=1024 gives ~1.3x noise ratio and local_std ~0.021)
FLOOR_NOISE_RATIO_MAX = 2.0    # denoised floor lap_var / vanilla floor lap_var
FLOOR_LOCAL_STD_MAX = 0.030     # max acceptable floor local_std at convergence
FLOOR_LUMA_MIN_RATIO = 0.60     # floor luma at least 60% of vanilla (demod/remod gap)


def parse_args():
    parser = argparse.ArgumentParser(description="Floor noise quality test")
    parser.add_argument("--framework", default="macos", choices=("glfw", "macos"))
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


def load_luminance(path):
    img = np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
    luma = img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722
    return img, luma


def get_floor_mask(luma):
    """Floor = lower 40% of image, moderate luminance."""
    h, w = luma.shape
    mask = np.zeros_like(luma, dtype=bool)
    mask[int(h * 0.6):, :] = True
    mask &= (luma > 0.1) & (luma < 0.8)
    return mask


def floor_laplacian_var(luma, mask):
    kernel = np.array([[0, 1, 0], [1, -4, 1], [0, 1, 0]], dtype=np.float32)
    lap = convolve(luma, kernel, mode="reflect")
    return float(np.var(lap[mask]))


def floor_local_std(luma, mask):
    mean = uniform_filter(luma, size=5, mode='reflect')
    sqmean = uniform_filter(luma * luma, size=5, mode='reflect')
    local_var = np.maximum(sqmean - mean * mean, 0)
    local_std = np.sqrt(local_var)
    return float(np.mean(local_std[mask]))


def run_app(py, build_py, framework, test_case, extra_args, label,
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
        print(f"  FAIL: {label} exited with code {result.returncode}")
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
    print("  Floor Noise Quality Test")
    print("=" * 60)

    if not args.skip_build:
        print("\nBuilding...")
        result = subprocess.run([py, build_py, "--framework", fw],
                                cwd=PROJECT_ROOT, capture_output=True, text=True)
        if result.returncode != 0:
            print("FAIL: build failed")
            return 1

    results = []

    # Run 1: Vanilla baseline (converge, screenshot)
    print(f"\n{'—' * 60}")
    print("  Run 1: Vanilla baseline (converge, screenshot)")
    print(f"{'—' * 60}")
    ok = run_app(py, build_py, fw, "screenshot", ["--max_spp", "2048"],
                 "vanilla", use_reblur=False, clear_screenshots=True)
    if not ok:
        results.append(("Vanilla: test run", False))
        _print_summary(results)
        return 1
    results.append(("Vanilla: test run", True))

    vanilla_path = find_screenshot(screenshot_dir, "*screenshot*")
    if not vanilla_path:
        results.append(("Vanilla: screenshot found", False))
        _print_summary(results)
        return 1
    results.append(("Vanilla: screenshot found", True))

    _, vanilla_luma = load_luminance(vanilla_path)
    floor_mask = get_floor_mask(vanilla_luma)
    vanilla_flv = floor_laplacian_var(vanilla_luma, floor_mask)
    vanilla_fls = floor_local_std(vanilla_luma, floor_mask)
    vanilla_floor_luma = float(np.mean(vanilla_luma[floor_mask]))
    print(f"  Vanilla floor: luma={vanilla_floor_luma:.4f} "
          f"lap_var={vanilla_flv:.6f} local_std={vanilla_fls:.6f}")

    # Rename
    os.rename(vanilla_path, os.path.join(screenshot_dir, "floor_vanilla.png"))

    # Run 2: Denoised-only (converge with REBLUR, no PT blend)
    print(f"\n{'—' * 60}")
    print("  Run 2: Denoised-only (converge, --reblur_no_pt_blend)")
    print(f"{'—' * 60}")
    ok = run_app(py, build_py, fw, "screenshot",
                 ["--max_spp", "2048", "--reblur_no_pt_blend", "true"],
                 "denoised-only", use_reblur=True)
    if not ok:
        results.append(("Denoised: test run", False))
        _print_summary(results)
        return 1
    results.append(("Denoised: test run", True))

    denoised_path = find_screenshot(screenshot_dir, "*screenshot*")
    if not denoised_path:
        results.append(("Denoised: screenshot found", False))
        _print_summary(results)
        return 1
    results.append(("Denoised: screenshot found", True))

    _, denoised_luma = load_luminance(denoised_path)
    denoised_flv = floor_laplacian_var(denoised_luma, floor_mask)
    denoised_fls = floor_local_std(denoised_luma, floor_mask)
    denoised_floor_luma = float(np.mean(denoised_luma[floor_mask]))
    print(f"  Denoised floor: luma={denoised_floor_luma:.4f} "
          f"lap_var={denoised_flv:.6f} local_std={denoised_fls:.6f}")

    # Rename
    os.rename(denoised_path,
              os.path.join(screenshot_dir, "floor_denoised.png"))

    # Evaluate metrics
    noise_ratio = denoised_flv / max(vanilla_flv, 1e-10)
    luma_ratio = denoised_floor_luma / max(vanilla_floor_luma, 1e-6)

    print(f"\n  Floor noise ratio (denoised/vanilla): {noise_ratio:.2f}x")
    print(f"  Floor local_std: {denoised_fls:.6f}")
    print(f"  Floor luma ratio: {luma_ratio:.4f} "
          f"({(1-luma_ratio)*100:.1f}% gap)")

    if noise_ratio <= FLOOR_NOISE_RATIO_MAX:
        print(f"  PASS: noise ratio {noise_ratio:.2f} <= {FLOOR_NOISE_RATIO_MAX}")
        results.append(
            (f"Floor noise ratio <= {FLOOR_NOISE_RATIO_MAX}", True))
    else:
        print(f"  FAIL: noise ratio {noise_ratio:.2f} > {FLOOR_NOISE_RATIO_MAX}")
        results.append(
            (f"Floor noise ratio <= {FLOOR_NOISE_RATIO_MAX}", False))

    if denoised_fls <= FLOOR_LOCAL_STD_MAX:
        print(f"  PASS: local_std {denoised_fls:.6f} <= {FLOOR_LOCAL_STD_MAX}")
        results.append(
            (f"Floor local_std <= {FLOOR_LOCAL_STD_MAX}", True))
    else:
        print(f"  FAIL: local_std {denoised_fls:.6f} > {FLOOR_LOCAL_STD_MAX}")
        results.append(
            (f"Floor local_std <= {FLOOR_LOCAL_STD_MAX}", False))

    if luma_ratio >= FLOOR_LUMA_MIN_RATIO:
        print(f"  PASS: luma ratio {luma_ratio:.4f} >= {FLOOR_LUMA_MIN_RATIO}")
        results.append(
            (f"Floor luma ratio >= {FLOOR_LUMA_MIN_RATIO}", True))
    else:
        print(f"  FAIL: luma ratio {luma_ratio:.4f} < {FLOOR_LUMA_MIN_RATIO}")
        results.append(
            (f"Floor luma ratio >= {FLOOR_LUMA_MIN_RATIO}", False))

    _print_summary(results)
    return 0 if all(ok for _, ok in results) else 1


def _print_summary(results):
    passed = sum(1 for _, ok in results if ok)
    failed = sum(1 for _, ok in results if not ok)
    print(f"\n{'=' * 60}")
    print(f"  Results: {passed} passed, {failed} failed")
    for name, ok in results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    print(f"{'=' * 60}")


if __name__ == "__main__":
    sys.exit(main())
