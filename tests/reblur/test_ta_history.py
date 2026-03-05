"""TAHistory debug mode convergence test.

Validates that the TAHistory debug mode (--reblur_debug_pass TAHistory) shows
history that improves over time. The TAHistory visualization outputs the raw
reprojected history buffer contents, which should converge (become less noisy)
as temporal accumulation progresses.

If the history feedback loop is broken (diagnostic data fed back as history
instead of properly accumulated radiance), the noise level will NOT decrease.

Orchestrates one run with the reblur_ta_history C++ test case, which captures
screenshots at frame 10 ("early") and frame 60 ("late"). Validates:
  1. Both screenshots exist and are non-degenerate (no NaN, non-black)
  2. Noise decreases from early to late (history is converging)
  3. Late screenshot luminance is reasonable (history contains real data)

Usage:
  python3 tests/reblur/test_ta_history.py --framework macos [--skip_build]
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
# Luminance must increase from early to late (history accumulating).
# With 60 frames vs 10 frames of accumulation, expect significant increase.
MIN_LUMA_INCREASE = 1.5  # late_mean / early_mean >= 1.5

# Relative noise (coefficient of variation = std/mean) must decrease.
# TAHistory shows demodulated signal: as accumulation grows, SNR improves.
# Absolute noise increases with signal but relative noise must decrease.
MIN_RELATIVE_NOISE_REDUCTION = 1.2  # early_cv / late_cv >= 1.2

# Late screenshot must have reasonable luminance (not all-black).
MIN_LATE_LUMA = 0.05

# Late screenshot must not be all NaN or Inf.
MAX_BAD_PIXEL_FRAC = 0.001


def parse_args():
    parser = argparse.ArgumentParser(
        description="TAHistory debug mode convergence test")
    parser.add_argument("--framework", default="macos",
                        choices=("glfw", "macos"))
    parser.add_argument("--skip_build", action="store_true")
    return parser.parse_known_args()


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


def compute_laplacian_variance(luma):
    kernel = np.array([[0, 1, 0],
                       [1, -4, 1],
                       [0, 1, 0]], dtype=np.float32)
    laplacian = convolve(luma, kernel, mode="reflect")
    return float(np.var(laplacian))


def compute_local_std(luma, block_size=8):
    """Compute mean local standard deviation over non-overlapping blocks."""
    h, w = luma.shape
    bh = h // block_size
    bw = w // block_size
    cropped = luma[:bh * block_size, :bw * block_size]
    blocks = cropped.reshape(bh, block_size, bw, block_size)
    block_std = np.std(blocks, axis=(1, 3))
    return float(np.mean(block_std))


def main():
    args, extra_args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)

    print("=" * 60)
    print("  TAHistory Debug Mode Convergence Test")
    print("=" * 60)

    # Build
    if not args.skip_build:
        print("\nBuilding...")
        result = subprocess.run([py, build_py, "--framework", fw] + extra_args,
                                cwd=PROJECT_ROOT, capture_output=True,
                                text=True)
        if result.returncode != 0:
            print("FAIL: build failed")
            if result.stderr:
                for line in result.stderr.strip().splitlines()[-5:]:
                    print(f"  {line}")
            return 1

    all_results = []

    # --- Run: TAHistory debug pass with early + late screenshots ---
    print(f"\n{'—'*60}")
    print("  Run: TAHistory debug mode (early frame 10, late frame 60)")
    print(f"{'—'*60}")

    cmd = [py, build_py, "--framework", fw, "--skip_build",
           "--run", "--test_case", "reblur_ta_history", "--headless", "true",
           "--pipeline", "gpu", "--use_reblur", "true", "--spp", "1",
           "--max_spp", "64", "--reblur_debug_pass", "TAHistory",
           "--reblur_no_pt_blend", "true",
           "--clear_screenshots", "true",
           "--test_timeout", "120"] + extra_args
    print(f"  cmd: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=PROJECT_ROOT, capture_output=True,
                            text=True)
    if result.returncode != 0:
        print(f"  FAIL: app exited with code {result.returncode}")
        if result.stderr:
            for line in result.stderr.strip().splitlines()[-5:]:
                print(f"    {line}")
        all_results.append(("TAHistory run", False))
        _print_summary(all_results)
        return 1
    all_results.append(("TAHistory run", True))

    # Find screenshots
    early_path = find_screenshot(screenshot_dir, "*ta_history_early*")
    late_path = find_screenshot(screenshot_dir, "*ta_history_late*")

    if not early_path:
        print("  FAIL: early screenshot not found")
        all_results.append(("Early screenshot found", False))
        _print_summary(all_results)
        return 1
    if not late_path:
        print("  FAIL: late screenshot not found")
        all_results.append(("Late screenshot found", False))
        _print_summary(all_results)
        return 1
    all_results.append(("Screenshots found", True))

    print(f"  Early: {early_path}")
    print(f"  Late:  {late_path}")

    # Load and analyze
    early_img, early_luma = load_luminance(early_path)
    late_img, late_luma = load_luminance(late_path)

    early_mean = float(np.mean(early_luma))
    late_mean = float(np.mean(late_luma))
    early_noise = compute_laplacian_variance(early_luma)
    late_noise = compute_laplacian_variance(late_luma)
    early_std = compute_local_std(early_luma)
    late_std = compute_local_std(late_luma)

    # Check for NaN/Inf
    early_bad = float(np.mean(np.isnan(early_img) | np.isinf(early_img)))
    late_bad = float(np.mean(np.isnan(late_img) | np.isinf(late_img)))

    print(f"\n  Early (frame 10):")
    print(f"    mean_luma:      {early_mean:.6f}")
    print(f"    laplacian_var:  {early_noise:.6f}")
    print(f"    local_std:      {early_std:.6f}")
    print(f"    bad_pixel_frac: {early_bad:.6f}")

    print(f"\n  Late (frame 60):")
    print(f"    mean_luma:      {late_mean:.6f}")
    print(f"    laplacian_var:  {late_noise:.6f}")
    print(f"    local_std:      {late_std:.6f}")
    print(f"    bad_pixel_frac: {late_bad:.6f}")

    # Relative noise: coefficient of variation (std / mean)
    # This is scale-independent — absolute noise increases with signal but CV decreases
    early_cv = early_std / max(early_mean, 1e-6)
    late_cv = late_std / max(late_mean, 1e-6)
    luma_increase = late_mean / max(early_mean, 1e-6)
    cv_reduction = early_cv / max(late_cv, 1e-6)

    print(f"\n  Convergence metrics:")
    print(f"    luma increase (late/early):  {luma_increase:.2f}x")
    print(f"    early CV (std/mean):         {early_cv:.4f}")
    print(f"    late  CV (std/mean):         {late_cv:.4f}")
    print(f"    relative noise reduction:    {cv_reduction:.2f}x")

    # --- Validate ---

    # 1. No NaN/Inf
    if early_bad <= MAX_BAD_PIXEL_FRAC:
        print(f"\n  PASS: early bad pixels {early_bad:.6f} <= {MAX_BAD_PIXEL_FRAC}")
        all_results.append(("Early: no NaN/Inf", True))
    else:
        print(f"\n  FAIL: early bad pixels {early_bad:.6f} > {MAX_BAD_PIXEL_FRAC}")
        all_results.append(("Early: no NaN/Inf", False))

    if late_bad <= MAX_BAD_PIXEL_FRAC:
        print(f"  PASS: late bad pixels {late_bad:.6f} <= {MAX_BAD_PIXEL_FRAC}")
        all_results.append(("Late: no NaN/Inf", True))
    else:
        print(f"  FAIL: late bad pixels {late_bad:.6f} > {MAX_BAD_PIXEL_FRAC}")
        all_results.append(("Late: no NaN/Inf", False))

    # 2. Late screenshot has reasonable luminance
    if late_mean >= MIN_LATE_LUMA:
        print(f"  PASS: late mean_luma {late_mean:.6f} >= {MIN_LATE_LUMA}")
        all_results.append(("Late: non-black", True))
    else:
        print(f"  FAIL: late mean_luma {late_mean:.6f} < {MIN_LATE_LUMA}")
        all_results.append(("Late: non-black", False))

    # 3. Luminance increases (history accumulating signal)
    if luma_increase >= MIN_LUMA_INCREASE:
        print(f"  PASS: luma increase {luma_increase:.2f}x >= {MIN_LUMA_INCREASE}x")
        all_results.append(("History accumulating (luma increase)", True))
    else:
        print(f"  FAIL: luma increase {luma_increase:.2f}x < {MIN_LUMA_INCREASE}x")
        print(f"         History is NOT accumulating")
        all_results.append(("History accumulating (luma increase)", False))

    # 4. Relative noise decreases (SNR improving)
    if cv_reduction >= MIN_RELATIVE_NOISE_REDUCTION:
        print(f"  PASS: relative noise reduction {cv_reduction:.2f}x >= {MIN_RELATIVE_NOISE_REDUCTION}x")
        all_results.append(("History converging (relative noise)", True))
    else:
        print(f"  FAIL: relative noise reduction {cv_reduction:.2f}x < {MIN_RELATIVE_NOISE_REDUCTION}x")
        print(f"         History SNR is NOT improving")
        all_results.append(("History converging (relative noise)", False))

    _print_summary(all_results)
    return 0 if all(ok for _, ok in all_results) else 1


def _print_summary(results):
    passed = sum(1 for _, ok in results if ok)
    failed = sum(1 for _, ok in results if not ok)
    print(f"\n{'='*60}")
    print(f"  TAHistory Convergence Test: {passed} passed, {failed} failed")
    print(f"{'='*60}")
    for name, ok in results:
        status = "PASS" if ok else "FAIL"
        print(f"  [{status}]  {name}")
    print(f"{'='*60}")


if __name__ == "__main__":
    sys.exit(main())
