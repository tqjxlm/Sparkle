#!/usr/bin/env python3
"""
Test: Stabilization stale history detection

Validates that temporal stabilization does not maintain stale bright history
values for pixels where PostBlur consistently outputs dark values.

Root cause: The stabilization blend weight uses max(accumSpeed, frame_index),
which gives blend=0.9995 for edge pixels that disocclude every frame
(accumSpeed=1 but frame_index=2048). Combined with sigma_scale=5.0 (wide
clamping box for low accumSpeed), the stabilization locks onto stale bright
history and ignores the current PostBlur input.

Evidence: Pixel (933,120) — PostBlur consistently dark (0.13) but full pipeline
bright (0.80) for 4 out of 5 frames. The stabilization is outputting values 6x
brighter than its input.

Test strategy:
  1. Capture PostBlur output (debug_pass=2) and full pipeline at 2048 spp
  2. Find pixels where PostBlur is stable but full pipeline is unstable
     (stabilization CREATING instability)
  3. Find pixels where full pipeline diverges greatly from PostBlur mean
     (stale history)
  4. Both counts should be near zero

Usage:
  python tests/reblur/test_stabilization_stale_history.py --framework glfw [--skip_build]
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
# Max pixels where stabilization CREATES instability (PostBlur stable, full unstable)
MAX_STAB_CREATED_INSTABILITY = 100

# Max pixels where full pipeline mean diverges >0.3 from PostBlur mean
MAX_STALE_HISTORY_PIXELS = 50

# Max ratio of full/postblur luminance for any pixel (stale bright history indicator)
MAX_AMPLIFICATION_RATIO = 4.0


def get_screenshot_dir(framework):
    if framework == "glfw":
        return os.path.join(
            PROJECT_ROOT, "build_system", "glfw", "output",
            "build", "generated", "screenshots"
        )
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    raise ValueError(f"Unsupported framework: {framework}")


def run_app(framework, debug_pass="Full", skip_build=False, passthrough_args=()):
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
    cmd += list(passthrough_args)

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
    args, extra_args = parser.parse_known_args()

    print("=== Test: Stabilization Stale History ===\n")
    passed = True

    # Capture PostBlur output
    print("Capturing PostBlur output (debug_pass=2)...")
    pb_frames = run_app(args.framework, debug_pass="PostBlur", skip_build=args.skip_build,
                        passthrough_args=extra_args)
    if pb_frames is None:
        print("FAIL: Could not capture PostBlur output")
        return 1

    # Capture full pipeline output
    print("Capturing full pipeline output...")
    full_frames = run_app(args.framework, debug_pass="Full", skip_build=args.skip_build,
                         passthrough_args=extra_args)
    if full_frames is None:
        print("FAIL: Could not capture full pipeline output")
        return 1

    pb_lumas = np.stack([get_luma(f) for f in pb_frames])
    full_lumas = np.stack([get_luma(f) for f in full_frames])

    pb_std = np.std(pb_lumas, axis=0)
    full_std = np.std(full_lumas, axis=0)
    pb_mean = np.mean(pb_lumas, axis=0)
    full_mean = np.mean(full_lumas, axis=0)

    # Per-pixel max jump
    threshold = 5.0 / 255
    pb_maxjump = np.zeros(pb_lumas.shape[1:])
    full_maxjump = np.zeros(full_lumas.shape[1:])
    for i in range(4):
        pb_maxjump = np.maximum(pb_maxjump, np.abs(pb_lumas[i + 1] - pb_lumas[i]))
        full_maxjump = np.maximum(full_maxjump, np.abs(full_lumas[i + 1] - full_lumas[i]))

    total = pb_std.size

    # Test 1: Stabilization creating instability
    # Pixels where PostBlur is stable (max_jump < 5/255) but full pipeline is unstable
    pb_stable = pb_maxjump < threshold
    full_unstable = full_maxjump > threshold
    stab_created = pb_stable & full_unstable
    n_created = int(np.sum(stab_created))

    print(f"\n  Test 1: Stabilization-created instability")
    print(f"    PostBlur stable pixels: {np.sum(pb_stable)} ({np.sum(pb_stable)/total*100:.1f}%)")
    print(f"    Full unstable pixels: {np.sum(full_unstable)} ({np.sum(full_unstable)/total*100:.1f}%)")
    print(f"    Stabilization-created: {n_created} (threshold: {MAX_STAB_CREATED_INSTABILITY})")

    if n_created > MAX_STAB_CREATED_INSTABILITY:
        print(f"    FAIL: {n_created} pixels have stabilization-created instability")
        passed = False

    # Test 2: Stale history — full pipeline mean diverges from PostBlur mean
    # Only check non-sky pixels (pb_mean > 0.01)
    non_sky = pb_mean > 0.01
    mean_divergence = np.abs(full_mean - pb_mean)
    stale = non_sky & (mean_divergence > 0.3)
    n_stale = int(np.sum(stale))

    print(f"\n  Test 2: Stale history pixels")
    print(f"    Non-sky pixels: {np.sum(non_sky)}")
    print(f"    Mean divergence > 0.3: {n_stale} (threshold: {MAX_STALE_HISTORY_PIXELS})")

    if n_stale > 0:
        # Show worst examples
        ys, xs = np.where(stale)
        worst_idx = np.argsort(mean_divergence[stale])[::-1][:5]
        print(f"    Worst examples:")
        for idx in worst_idx:
            y, x = ys[idx], xs[idx]
            print(f"      ({x},{y}): PB_mean={pb_mean[y,x]:.3f} Full_mean={full_mean[y,x]:.3f} "
                  f"divergence={mean_divergence[y,x]:.3f}")

    if n_stale > MAX_STALE_HISTORY_PIXELS:
        print(f"    FAIL: {n_stale} pixels have stale history")
        passed = False

    # Test 3: Amplification — full pipeline brighter than PostBlur
    # Ratio of full_mean / max(pb_mean, 0.01) for non-sky pixels
    safe_pb_mean = np.maximum(pb_mean, 0.01)
    amplification = full_mean / safe_pb_mean
    # Only check pixels where both are non-trivial
    check_mask = non_sky & (pb_mean > 0.05)
    if np.any(check_mask):
        max_amp = float(np.max(amplification[check_mask]))
        amp_above_threshold = check_mask & (amplification > MAX_AMPLIFICATION_RATIO)
        n_amp = int(np.sum(amp_above_threshold))

        print(f"\n  Test 3: History amplification")
        print(f"    Max amplification ratio: {max_amp:.2f}x (threshold: {MAX_AMPLIFICATION_RATIO}x)")
        print(f"    Pixels with ratio > {MAX_AMPLIFICATION_RATIO}x: {n_amp}")

        if max_amp > MAX_AMPLIFICATION_RATIO:
            print(f"    FAIL: Stabilization amplifying by {max_amp:.2f}x")
            passed = False
    else:
        print(f"\n  Test 3: No qualifying pixels for amplification check")

    # Summary
    print(f"\n{'='*60}")
    if passed:
        print("PASS: Stabilization is not maintaining stale history")
    else:
        print("FAIL: Stabilization has stale history issues")
        print("\nThe stabilization blend weight (max(accumSpeed, frame_index)) gives")
        print("blend=0.9995 for disoccluded pixels. Combined with wide sigma_scale=5.0,")
        print("the stabilization locks onto stale values and ignores PostBlur input.")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
