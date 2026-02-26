#!/usr/bin/env python3
"""
Test: Temporal accumulation firefly suppression

Validates that extreme 1-spp outlier samples (specular caustics) do not cause
large frame-to-frame jumps in the temporal accumulation output.

Root cause: At accumSpeed=63 (weight=1/64), an unclamped 1-spp sample with 40-60x
the average radiance causes a luminance jump of ~1.0 in a single frame. This
propagates through the pipeline because:
  1. HistoryFix anti-firefly uses spatial statistics that are also noisy
  2. Stabilization's 5x5 neighborhood is too small to average out the variance
  3. The stabilization clamping box tracks the noisy PostBlur neighborhood mean

Evidence from analyze_correlated_firefly.py:
  - Inferred 1-spp values reach 40-60x the temporal average
  - 2,482 pixels have PostBlur std>0.05 AND full pipeline std>0.05
  - Stabilization retention ratio = 0.78 (only 22% reduction)
  - enable_firefly_suppression exists in temporal accum UBO but is unused

Test strategy:
  1. Run temporal accumulation output (debug_pass=3) at 2048 spp
  2. Measure max per-pixel luminance jump between consecutive frames
  3. The max jump should be bounded (not reaching near 1.0)
  4. Also measure the percentage of pixels with std > 0.1 across 5 frames

Pass criteria:
  - Max frame-to-frame luminance jump per pixel < 0.3 (currently ~0.99)
  - Percentage of pixels with std > 0.1 < 2% (currently ~7.5%)
  - Full pipeline >5/255 instability < 0.1% (currently 0.29%)

Usage:
  python tests/reblur/test_temporal_firefly.py --framework glfw [--skip_build]
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
# Max per-pixel luminance jump between consecutive frames in temporal accum output.
# Without firefly suppression, this reaches ~0.99. With suppression, should be < 0.3.
MAX_TEMPORAL_ACCUM_JUMP = 0.3

# Max percentage of pixels with std > 0.1 in temporal accum output.
# Without suppression: ~7.5%. With suppression: should be < 2%.
MAX_HIGH_STD_PCT = 2.0

# Max percentage of pixels changing by >5/255 per frame in full pipeline.
# Current: 0.29%. Target: < 0.1%.
MAX_FULL_PIPELINE_PCT5 = 0.1


def get_screenshot_dir(framework):
    if framework == "glfw":
        return os.path.join(
            PROJECT_ROOT, "build_system", "glfw", "output",
            "build", "generated", "screenshots"
        )
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    raise ValueError(f"Unsupported framework: {framework}")


def run_app(framework, use_reblur, debug_pass=99, max_spp=2048, skip_build=False):
    """Run the app and return screenshots as numpy arrays."""
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    cmd = [
        sys.executable, build_py,
        "--framework", framework,
        "--pipeline", "gpu",
        "--use_reblur", "true" if use_reblur else "false",
        "--max_spp", str(max_spp),
        "--run", "--headless", "true",
        "--test_case", "multi_frame_screenshot",
        "--clear_screenshots", "true",
    ]
    if skip_build:
        cmd.append("--skip_build")
    if use_reblur and debug_pass != 99:
        cmd += ["--reblur_debug_pass", str(debug_pass)]

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if result.returncode != 0:
        print(f"  App failed (exit={result.returncode})")
        return None

    screenshot_dir = get_screenshot_dir(framework)
    frames = []
    for i in range(5):
        path = os.path.join(screenshot_dir, f"multi_frame_{i}.png")
        if not os.path.exists(path):
            print(f"  Missing screenshot: {path}")
            return None
        frames.append(np.array(Image.open(path), dtype=np.float32) / 255.0)
    return frames


def get_luma(frame):
    return np.dot(frame[:, :, :3], [0.2126, 0.7152, 0.0722])


def measure_temporal_accum_jumps(frames):
    """Measure per-pixel luminance jumps in temporal accumulation output."""
    lumas = np.stack([get_luma(f) for f in frames])  # (5, H, W)

    # Max jump across any consecutive frame pair per pixel
    max_jumps = np.zeros(lumas.shape[1:])
    for i in range(4):
        jumps = np.abs(lumas[i + 1] - lumas[i])
        max_jumps = np.maximum(max_jumps, jumps)

    # Per-pixel std across all 5 frames
    per_pixel_std = np.std(lumas, axis=0)

    return {
        "max_jump": float(np.max(max_jumps)),
        "p99_jump": float(np.percentile(max_jumps, 99)),
        "p999_jump": float(np.percentile(max_jumps, 99.9)),
        "high_std_pct": float(np.mean(per_pixel_std > 0.1) * 100),
        "firefly_pct": float(np.mean(max_jumps > 0.5) * 100),
    }


def measure_full_pipeline_stability(frames):
    """Measure frame-to-frame instability of full pipeline output."""
    pct5_list = []
    pct1_list = []
    for i in range(len(frames) - 1):
        diff = np.abs(frames[i + 1][:, :, :3] - frames[i][:, :, :3])
        pct5_list.append(float(np.mean(np.any(diff > 5.0 / 255, axis=2)) * 100))
        pct1_list.append(float(np.mean(np.any(diff > 1.0 / 255, axis=2)) * 100))
    return {
        "pct_5": np.mean(pct5_list),
        "pct_1": np.mean(pct1_list),
    }


def main():
    parser = argparse.ArgumentParser(description="Test temporal firefly suppression")
    parser.add_argument("--framework", required=True, choices=("glfw", "macos"))
    parser.add_argument("--skip_build", action="store_true")
    args = parser.parse_args()

    print("=== Test: Temporal Accumulation Firefly Suppression ===\n")
    passed = True

    # Test 1: Temporal accumulation output (debug_pass=3)
    print("Step 1: Measuring temporal accumulation output jumps (debug_pass=3)...")
    ta_frames = run_app(args.framework, use_reblur=True, debug_pass=3,
                        skip_build=args.skip_build)
    if ta_frames is None:
        print("FAIL: Could not capture temporal accumulation output")
        return 1

    ta_metrics = measure_temporal_accum_jumps(ta_frames)
    print(f"  Max per-pixel jump:  {ta_metrics['max_jump']:.4f} "
          f"(threshold: {MAX_TEMPORAL_ACCUM_JUMP})")
    print(f"  P99 jump:            {ta_metrics['p99_jump']:.4f}")
    print(f"  P99.9 jump:          {ta_metrics['p999_jump']:.4f}")
    print(f"  Pixels with std>0.1: {ta_metrics['high_std_pct']:.2f}% "
          f"(threshold: {MAX_HIGH_STD_PCT}%)")
    print(f"  Pixels with jump>0.5: {ta_metrics['firefly_pct']:.3f}%")

    if ta_metrics["max_jump"] > MAX_TEMPORAL_ACCUM_JUMP:
        print(f"\n  FAIL: Max temporal accum jump ({ta_metrics['max_jump']:.4f}) "
              f"exceeds {MAX_TEMPORAL_ACCUM_JUMP}")
        passed = False

    if ta_metrics["high_std_pct"] > MAX_HIGH_STD_PCT:
        print(f"  FAIL: High-std pixel percentage ({ta_metrics['high_std_pct']:.2f}%) "
              f"exceeds {MAX_HIGH_STD_PCT}%")
        passed = False

    # Test 2: Full pipeline output (stabilization)
    print(f"\nStep 2: Measuring full pipeline stability...")
    full_frames = run_app(args.framework, use_reblur=True, debug_pass=99,
                          skip_build=args.skip_build)
    if full_frames is None:
        print("FAIL: Could not capture full pipeline output")
        return 1

    full_metrics = measure_full_pipeline_stability(full_frames)
    print(f"  >5/255 instability:  {full_metrics['pct_5']:.3f}% "
          f"(threshold: {MAX_FULL_PIPELINE_PCT5}%)")
    print(f"  >1/255 instability:  {full_metrics['pct_1']:.3f}%")

    if full_metrics["pct_5"] > MAX_FULL_PIPELINE_PCT5:
        print(f"\n  FAIL: Full pipeline >5/255 instability ({full_metrics['pct_5']:.3f}%) "
              f"exceeds {MAX_FULL_PIPELINE_PCT5}%")
        passed = False

    # Summary
    print(f"\n{'='*60}")
    if passed:
        print("PASS: Temporal firefly suppression is working correctly")
        return 0
    else:
        print("FAIL: Temporal firefly suppression needs improvement")
        print("\nRemaining issues:")
        if ta_metrics["max_jump"] > MAX_TEMPORAL_ACCUM_JUMP:
            print(f"  - Unclamped 1-spp outliers cause jumps of {ta_metrics['max_jump']:.2f}")
            print(f"    The enable_firefly_suppression flag in temporal accum UBO is unused")
        if ta_metrics["high_std_pct"] > MAX_HIGH_STD_PCT:
            print(f"  - {ta_metrics['high_std_pct']:.1f}% of pixels have std>0.1")
        if full_metrics["pct_5"] > MAX_FULL_PIPELINE_PCT5:
            print(f"  - {full_metrics['pct_5']:.2f}% visible flicker (>5/255) at convergence")
        return 1


if __name__ == "__main__":
    sys.exit(main())
