#!/usr/bin/env python3
"""
Test: Stabilization neighborhood tracking

Validates that temporal stabilization actually suppresses PostBlur noise
instead of tracking it. Measures the stabilization "suppression ratio":
the ratio of full pipeline instability to PostBlur instability.

Root cause: The stabilization pass uses a 5x5 neighborhood of PostBlur
luminance to compute mean/sigma for its color clamping box. When PostBlur
pixels have high per-frame variance (std ~0.3 from extreme 1-spp outliers),
the 25-sample neighborhood mean fluctuates by ~std/sqrt(25) ≈ 0.06. The
clamping box follows this fluctuating mean, so the stabilized output tracks
PostBlur noise instead of suppressing it.

Evidence: For the 2,482 worst pixels, stabilization retention ratio = 0.78
(only 22% noise reduction). Expected ratio with proper stabilization at
frame 255+: should be < 0.1 (90%+ suppression).

Test strategy:
  1. Capture PostBlur output (debug_pass=2) and full pipeline at 2048 spp
  2. For pixels that are unstable in PostBlur (std>0.05):
     - Measure how much stabilization reduces their instability
     - The suppression ratio should be < 0.5 (currently 0.78)
  3. Also measure how stable the 5x5 neighborhood mean is

Usage:
  python tests/reblur/test_stabilization_tracking.py --framework glfw [--skip_build]
"""

import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, PROJECT_ROOT)

# Thresholds
# Stabilization retention ratio for worst pixels (postblur_std > 0.05):
# ratio = full_std / postblur_std. Lower = better suppression.
# At frame 255+, stabilization blend is 0.996. Perfect tracking gives ratio=1.0,
# perfect suppression gives ratio=0.004. Current: 0.78. Target: < 0.5.
MAX_RETENTION_RATIO = 0.5

# Max percentage of PostBlur's unstable pixels surviving through stabilization
MAX_SURVIVING_PCT = 2.0


def get_screenshot_dir(framework):
    if framework == "glfw":
        return os.path.join(
            PROJECT_ROOT, "build_system", "glfw", "output",
            "build", "generated", "screenshots"
        )
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    raise ValueError(f"Unsupported framework: {framework}")


def run_app(framework, debug_pass="Full", skip_build=False):
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    cmd = [
        sys.executable, build_py,
        "--framework", framework,
        "--pipeline", "gpu",
        "--use_reblur", "true",
        "--max_spp", "2048",
        "--run", "--headless", "true",
        "--test_case", "multi_frame_screenshot",
        "--clear_screenshots", "true",
    ]
    if skip_build:
        cmd.append("--skip_build")
    if debug_pass != "Full":
        cmd += ["--reblur_debug_pass", debug_pass]

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if result.returncode != 0:
        return None

    screenshot_dir = get_screenshot_dir(framework)
    frames = []
    for i in range(5):
        path = os.path.join(screenshot_dir, f"multi_frame_{i}.png")
        if not os.path.exists(path):
            return None
        frames.append(np.array(Image.open(path), dtype=np.float32) / 255.0)
    return frames


def get_luma(frame):
    return np.dot(frame[:, :, :3], [0.2126, 0.7152, 0.0722])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--framework", required=True, choices=("glfw", "macos"))
    parser.add_argument("--skip_build", action="store_true")
    args = parser.parse_args()

    print("=== Test: Stabilization Neighborhood Tracking ===\n")
    passed = True

    # Capture PostBlur output
    print("Capturing PostBlur output (debug_pass=2)...")
    pb_frames = run_app(args.framework, debug_pass="PostBlur", skip_build=args.skip_build)
    if pb_frames is None:
        print("FAIL: Could not capture PostBlur output")
        return 1

    # Capture full pipeline output
    print("Capturing full pipeline output...")
    full_frames = run_app(args.framework, debug_pass="Full", skip_build=args.skip_build)
    if full_frames is None:
        print("FAIL: Could not capture full pipeline output")
        return 1

    # Compute per-pixel std across 5 frames
    pb_lumas = np.stack([get_luma(f) for f in pb_frames])
    full_lumas = np.stack([get_luma(f) for f in full_frames])

    pb_std = np.std(pb_lumas, axis=0)
    full_std = np.std(full_lumas, axis=0)

    # Focus on pixels unstable in PostBlur
    pb_unstable_mask = pb_std > 0.05
    n_pb_unstable = np.sum(pb_unstable_mask)
    total = pb_std.size

    print(f"\n  PostBlur unstable pixels (std>0.05): {n_pb_unstable} "
          f"({n_pb_unstable/total*100:.2f}%)")

    if n_pb_unstable == 0:
        print("  No unstable PostBlur pixels - test passes trivially")
        return 0

    # Retention ratio: how much of PostBlur instability survives
    ratios = full_std[pb_unstable_mask] / np.maximum(pb_std[pb_unstable_mask], 1e-8)
    mean_ratio = float(np.mean(ratios))
    median_ratio = float(np.median(ratios))

    print(f"  Stabilization retention ratio: mean={mean_ratio:.3f}, "
          f"median={median_ratio:.3f}")
    print(f"  (1.0 = no suppression, 0.0 = perfect suppression)")
    print(f"  Threshold: {MAX_RETENTION_RATIO}")

    if mean_ratio > MAX_RETENTION_RATIO:
        print(f"\n  FAIL: Retention ratio ({mean_ratio:.3f}) exceeds {MAX_RETENTION_RATIO}")
        passed = False

    # Surviving pixels: PostBlur unstable AND still unstable in full pipeline
    full_unstable_mask = full_std > 0.05
    surviving = pb_unstable_mask & full_unstable_mask
    surviving_pct = float(np.sum(surviving) / total * 100)

    print(f"\n  Surviving unstable pixels: {np.sum(surviving)} ({surviving_pct:.2f}%)")
    print(f"  Threshold: {MAX_SURVIVING_PCT}%")

    if surviving_pct > MAX_SURVIVING_PCT:
        print(f"  FAIL: Surviving percentage ({surviving_pct:.2f}%) "
              f"exceeds {MAX_SURVIVING_PCT}%")
        passed = False

    # Analyze 5x5 neighborhood mean stability for surviving pixels
    if np.sum(surviving) > 0:
        ys, xs = np.where(surviving)
        n_sample = min(100, len(ys))
        neigh_mean_stds = []
        H, W = pb_std.shape
        for idx in range(n_sample):
            y, x = ys[idx], xs[idx]
            ny0, ny1 = max(0, y - 2), min(H, y + 3)
            nx0, nx1 = max(0, x - 2), min(W, x + 3)
            # 5x5 neighborhood mean across frames
            means = [float(np.mean(pb_lumas[f, ny0:ny1, nx0:nx1])) for f in range(5)]
            neigh_mean_stds.append(np.std(means))

        avg_neigh_std = np.mean(neigh_mean_stds)
        print(f"\n  5x5 neighborhood mean stability (surviving pixels):")
        print(f"    Mean of neighborhood-mean-std: {avg_neigh_std:.4f}")
        if avg_neigh_std > 0.01:
            print(f"    ** Neighborhood mean unstable - clamping box oscillates **")

    # Summary
    print(f"\n{'='*60}")
    if passed:
        print("PASS: Stabilization properly suppresses PostBlur noise")
    else:
        print("FAIL: Stabilization tracks PostBlur noise instead of suppressing it")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
