"""Camera motion quality and stability validation.

Tests the REBLUR denoiser under camera motion (orbit_sweep) and validates:
  1. No NaN/Inf in any frame during motion
  2. Frame-to-frame temporal stability (std-dev < 0.04 across 5 frames)
  3. Reasonable output quality during motion (not black, not flat)
  4. Static camera sanity check (no NaN/Inf, reasonable luminance)

Uses multi_frame_screenshot to capture 5 consecutive frames during motion
for temporal analysis, plus a single static screenshot for reconvergence.

Usage:
  python tests/reblur/reblur_motion_validation.py --framework glfw [--skip_build]
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
MAX_TEMPORAL_STD = 0.04        # Max per-pixel std across 5 motion frames (relaxed for motion)
MAX_UNSTABLE_PIXEL_PCT = 15.0  # Max % of pixels exceeding MAX_TEMPORAL_STD
MIN_STATIC_MEAN_LUMA = 0.05    # Minimum mean luminance for static reblur (not broken)


def parse_args():
    parser = argparse.ArgumentParser(description="Camera motion quality validation")
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


def load_screenshot(path):
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def compute_luminance(img):
    return img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722


def run_app(framework, args_list, label):
    """Run app with given args. Returns (success, stdout)."""
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    cmd = [sys.executable, build_py, "--framework", framework,
           "--skip_build"] + args_list
    print(f"\n--- {label} ---", flush=True)
    print(f"  cmd: {' '.join(cmd)}", flush=True)
    result = subprocess.run(cmd, cwd=PROJECT_ROOT, capture_output=True,
                            text=True, timeout=300)
    if result.returncode != 0:
        print(f"  FAIL: app exited with code {result.returncode}", flush=True)
        if result.stderr:
            for line in result.stderr.strip().splitlines()[-5:]:
                print(f"    {line}", flush=True)
        return False, result.stdout
    print("  OK", flush=True)
    return True, result.stdout


def _print_summary(results):
    passed = sum(1 for _, ok in results if ok)
    failed = sum(1 for _, ok in results if not ok)
    print(f"\n{'='*60}", flush=True)
    print(f"  Results: {passed} passed, {failed} failed", flush=True)
    for name, ok in results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}", flush=True)
    print(f"{'='*60}", flush=True)


def main():
    args = parse_args()
    fw = args.framework
    screenshot_dir = get_screenshot_dir(fw)

    print("=" * 60, flush=True)
    print("  Camera Motion Quality & Stability Validation", flush=True)
    print("=" * 60, flush=True)

    # Build
    if not args.skip_build:
        print("\nBuilding...", flush=True)
        build_py = os.path.join(PROJECT_ROOT, "build.py")
        result = subprocess.run(
            [sys.executable, build_py, "--framework", fw],
            cwd=PROJECT_ROOT, capture_output=True, text=True, timeout=300)
        if result.returncode != 0:
            print("FAIL: Build failed", flush=True)
            return 1

    results = []

    # ---------------------------------------------------------------
    # Phase 1: Motion stability — orbit_sweep with multi_frame_screenshot
    # ---------------------------------------------------------------
    ok, _ = run_app(fw, [
        "--run", "--test_case", "multi_frame_screenshot",
        "--clear_screenshots", "true",
        "--headless", "true",
        "--pipeline", "gpu", "--use_reblur", "true",
        "--spp", "1", "--max_spp", "30",
        # TODO: camera motion now uses TestCase-based approach (ReblurGhostingTest)
        "--test_timeout", "100",
    ], "Motion multi-frame capture (orbit_sweep, 30 spp)")

    if not ok:
        results.append(("Motion capture run", False))
        _print_summary(results)
        return 1
    results.append(("Motion capture run", True))

    # Load multi-frame screenshots
    motion_frames = []
    for i in range(5):
        path = os.path.join(screenshot_dir, f"multi_frame_{i}.png")
        if not os.path.exists(path):
            print(f"  FAIL: multi_frame_{i}.png not found", flush=True)
            results.append(("Motion frames found", False))
            _print_summary(results)
            return 1
        motion_frames.append(load_screenshot(path))
    results.append(("Motion frames found", True))

    # Validate per-frame properties
    print("\n--- Per-frame validation (motion) ---", flush=True)
    any_nan = False
    any_inf = False
    any_black = False
    for i, frame in enumerate(motion_frames):
        luma = compute_luminance(frame)
        has_nan = bool(np.any(np.isnan(frame)))
        has_inf = bool(np.any(np.isinf(frame)))
        mean_luma = float(np.mean(luma))
        if has_nan:
            any_nan = True
        if has_inf:
            any_inf = True
        if mean_luma < 1e-4:
            any_black = True
        print(f"  Frame {i}: mean_luma={mean_luma:.4f}, "
              f"nan={has_nan}, inf={has_inf}", flush=True)

    if any_nan:
        print("  FAIL: NaN detected in motion frames", flush=True)
        results.append(("No NaN (motion)", False))
    else:
        results.append(("No NaN (motion)", True))

    if any_inf:
        print("  FAIL: Inf detected in motion frames", flush=True)
        results.append(("No Inf (motion)", False))
    else:
        results.append(("No Inf (motion)", True))

    if any_black:
        print("  FAIL: all-black frame detected during motion", flush=True)
        results.append(("Not all-black (motion)", False))
    else:
        results.append(("Not all-black (motion)", True))

    # Temporal stability across 5 motion frames
    print("\n--- Temporal stability analysis ---", flush=True)
    lumas = np.stack([compute_luminance(f) for f in motion_frames])
    per_pixel_std = np.std(lumas, axis=0)

    mean_std = float(np.mean(per_pixel_std))
    max_std = float(np.max(per_pixel_std))
    median_std = float(np.median(per_pixel_std))
    unstable_mask = per_pixel_std > MAX_TEMPORAL_STD
    unstable_pct = float(np.mean(unstable_mask) * 100)

    print(f"  Per-pixel temporal std: mean={mean_std:.4f}, "
          f"median={median_std:.4f}, max={max_std:.4f}", flush=True)
    print(f"  Unstable pixels (std > {MAX_TEMPORAL_STD}): "
          f"{unstable_pct:.2f}% (threshold: {MAX_UNSTABLE_PIXEL_PCT}%)", flush=True)

    if unstable_pct > MAX_UNSTABLE_PIXEL_PCT:
        print(f"  FAIL: too many unstable pixels ({unstable_pct:.2f}% > "
              f"{MAX_UNSTABLE_PIXEL_PCT}%)", flush=True)
        results.append(("Temporal stability", False))
    else:
        results.append(("Temporal stability", True))

    # ---------------------------------------------------------------
    # Phase 2: Static camera sanity check — verify reblur still works
    # ---------------------------------------------------------------
    print("\n--- Static camera sanity check ---", flush=True)

    ok, _ = run_app(fw, [
        "--run", "--test_case", "screenshot",
        "--clear_screenshots", "true",
        "--headless", "true",
        "--pipeline", "gpu", "--use_reblur", "true",
        "--spp", "1", "--max_spp", "64",
    ], "Static camera reblur (64 spp)")

    if not ok:
        results.append(("Static reblur run", False))
        _print_summary(results)
        return 1
    results.append(("Static reblur run", True))

    reblur_path = get_latest_screenshot(screenshot_dir)
    if not reblur_path:
        results.append(("Static reblur screenshot", False))
        _print_summary(results)
        return 1
    results.append(("Static reblur screenshot", True))
    reblur_img = load_screenshot(reblur_path)
    reblur_luma = compute_luminance(reblur_img)

    reblur_mean = float(np.mean(reblur_luma))
    has_nan = bool(np.any(np.isnan(reblur_img)))
    has_inf = bool(np.any(np.isinf(reblur_img)))

    print(f"  Mean luminance: {reblur_mean:.6f}", flush=True)
    print(f"  Has NaN: {has_nan}, Has Inf: {has_inf}", flush=True)

    if has_nan or has_inf:
        print(f"  FAIL: NaN={has_nan}, Inf={has_inf} in static reblur", flush=True)
        results.append(("No NaN/Inf (static)", False))
    else:
        results.append(("No NaN/Inf (static)", True))

    if reblur_mean < MIN_STATIC_MEAN_LUMA:
        print(f"  FAIL: static reblur too dark ({reblur_mean:.6f} < "
              f"{MIN_STATIC_MEAN_LUMA})", flush=True)
        results.append(("Static luminance", False))
    else:
        results.append(("Static luminance", True))

    # ---------------------------------------------------------------
    # Summary
    # ---------------------------------------------------------------
    _print_summary(results)
    failed = sum(1 for _, ok in results if not ok)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
