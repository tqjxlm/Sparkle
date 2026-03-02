"""Denoised output stability during continuous camera motion.

Tests that the REBLUR denoised output maintains stable brightness during
continuous camera motion, without expanding dim regions or luminance collapse.
Uses --reblur_no_pt_blend to isolate the denoiser from the PT blend ramp.

Key metrics:
  1. Frame-to-frame luminance stability: no sudden drops after warm-up
  2. Monotonic convergence: denoised output should increase (or stay stable)
     during motion as temporal history accumulates, not decrease
  3. No expanding dim region: luminance slope should be >= 0 (not dimming)
  4. Settled quality: after motion stops, output reaches expected level

Runs a single capture with orbit_sweep animation + reblur_no_pt_blend.

Usage:
  python3 tests/reblur/test_denoised_motion_luma.py --framework macos [--skip_build]
"""

import argparse
import glob
import os
import subprocess
import sys

import numpy as np
from PIL import Image

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, PROJECT_ROOT)

# Thresholds
# After warm-up (frame 1+), each frame's luma should not drop below
# a fraction of the previous frame's luma
MIN_FRAME_TO_FRAME_RATIO = 0.85  # max 15% drop frame-to-frame
# Luminance slope across motion frames should be non-negative (not dimming)
MIN_LUMA_SLOPE = -0.005  # small negative slope tolerated due to noise
# Settled luminance should be at least this fraction of the peak motion luma
MIN_SETTLED_RATIO = 0.90


def parse_args():
    parser = argparse.ArgumentParser(
        description="Denoised motion luminance stability test")
    parser.add_argument("--framework", default="macos", choices=("glfw", "macos"))
    parser.add_argument("--skip_build", action="store_true")
    return parser.parse_args()


def get_screenshot_dir(framework):
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    return os.path.join(PROJECT_ROOT, "build_system", "glfw", "output",
                        "build", "generated", "screenshots")


def load_luminance(path):
    img = np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
    luma = img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722
    return float(np.mean(luma))


def find_motion_screenshots(screenshot_dir):
    paths = {}
    for i in range(5):
        pattern = f"*motion_track_{i}*"
        matches = glob.glob(os.path.join(screenshot_dir, pattern))
        if matches:
            matches.sort(key=os.path.getmtime, reverse=True)
            paths[i] = matches[0]
    settled = glob.glob(os.path.join(screenshot_dir, "*motion_track_settled*"))
    if settled:
        settled.sort(key=os.path.getmtime, reverse=True)
        paths["settled"] = settled[0]
    return paths


def main():
    args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)

    print("=" * 60)
    print("  Denoised Motion Luminance Stability")
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

    # Run reblur with continuous motion, denoised-only
    print(f"\n{'—'*60}")
    print("  Reblur denoised-only (orbit_sweep, --reblur_no_pt_blend true)")
    print(f"{'—'*60}")
    cmd = [py, build_py, "--framework", fw, "--skip_build",
           "--run", "--test_case", "motion_luminance_track",
           "--headless", "true", "--pipeline", "gpu", "--spp", "1",
           "--max_spp", "60", "--camera_animation", "orbit_sweep",
           "--clear_screenshots", "true", "--test_timeout", "120",
           "--use_reblur", "true", "--reblur_no_pt_blend", "true"]
    print(f"  cmd: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=PROJECT_ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  FAIL: app exited with code {result.returncode}")
        all_results.append(("Test run", False))
        _print_summary(all_results)
        return 1
    all_results.append(("Test run", True))

    paths = find_motion_screenshots(screenshot_dir)
    if len(paths) < 3:
        print("  FAIL: fewer than 3 motion screenshots found")
        all_results.append(("Screenshots found", False))
        _print_summary(all_results)
        return 1
    all_results.append(("Screenshots found", True))

    # Load all luminances
    motion_lumas = []
    settled_luma = None
    for key in sorted([k for k in paths if isinstance(k, int)]):
        luma = load_luminance(paths[key])
        motion_lumas.append(luma)
        print(f"  Frame {key}: mean_luma={luma:.6f}")
    if "settled" in paths:
        settled_luma = load_luminance(paths["settled"])
        print(f"  Settled:  mean_luma={settled_luma:.6f}")

    # --- Check 1: Frame-to-frame stability (skip frame 0, warm-up transient) ---
    print(f"\n  --- Frame-to-frame stability (frames 1+) ---")
    min_ratio = 1.0
    for i in range(2, len(motion_lumas)):  # skip frame 0 and 1 (warm-up)
        ratio = motion_lumas[i] / max(motion_lumas[i - 1], 1e-6)
        min_ratio = min(min_ratio, ratio)
        if ratio < MIN_FRAME_TO_FRAME_RATIO:
            print(f"  Frame {i}: ratio={ratio:.4f} (DROP from {motion_lumas[i-1]:.4f} to {motion_lumas[i]:.4f})")

    if min_ratio >= MIN_FRAME_TO_FRAME_RATIO:
        print(f"  PASS: min f2f ratio {min_ratio:.4f} >= {MIN_FRAME_TO_FRAME_RATIO}")
        all_results.append(("Frame-to-frame stability", True))
    else:
        print(f"  FAIL: min f2f ratio {min_ratio:.4f} < {MIN_FRAME_TO_FRAME_RATIO}")
        all_results.append(("Frame-to-frame stability", False))

    # --- Check 2: Luminance trend (no expanding dim region) ---
    # Use frames 1+ (skip warm-up frame 0)
    if len(motion_lumas) >= 3:
        trend_lumas = motion_lumas[1:]  # skip frame 0
        x = np.arange(len(trend_lumas))
        slope = float(np.polyfit(x, trend_lumas, 1)[0])
        print(f"\n  --- Luminance trend (frames 1+) ---")
        print(f"  Slope: {slope:.6f} per frame-step")
        if slope >= MIN_LUMA_SLOPE:
            print(f"  PASS: slope {slope:.6f} >= {MIN_LUMA_SLOPE} (not dimming)")
            all_results.append(("No expanding dim region", True))
        else:
            print(f"  FAIL: slope {slope:.6f} < {MIN_LUMA_SLOPE} (luminance decreasing)")
            all_results.append(("No expanding dim region", False))

    # --- Check 3: Settled quality ---
    if settled_luma is not None and len(motion_lumas) >= 2:
        peak_luma = max(motion_lumas[1:])
        settled_ratio = settled_luma / max(peak_luma, 1e-6)
        print(f"\n  --- Settled quality ---")
        print(f"  Peak motion luma: {peak_luma:.6f}")
        print(f"  Settled luma:     {settled_luma:.6f}")
        print(f"  Ratio:            {settled_ratio:.4f}")
        if settled_ratio >= MIN_SETTLED_RATIO:
            print(f"  PASS: settled ratio {settled_ratio:.4f} >= {MIN_SETTLED_RATIO}")
            all_results.append(("Settled quality", True))
        else:
            print(f"  FAIL: settled ratio {settled_ratio:.4f} < {MIN_SETTLED_RATIO}")
            all_results.append(("Settled quality", False))

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
