"""Continuous motion luminance tracking test.

Tests that the denoised output maintains correct brightness during continuous
camera motion. Detects the "expanding dim region" artifact where disoccluded
areas accumulate dim denoised output that persists because convergence is slow.

Runs two captures:
  1. Reblur with orbit_sweep animation — measures luminance per frame during motion
  2. Vanilla with orbit_sweep animation — provides ground-truth luminance reference

Metrics:
  - Per-frame mean luminance for reblur and vanilla
  - Luminance gap: (vanilla - reblur) / vanilla per frame
  - Maximum luminance gap during motion (threshold: 15%)
  - Luminance trend: does the gap increase over time? (expanding dim region)

Usage:
  python3 tests/reblur/test_motion_luminance.py --framework macos [--skip_build]
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
MAX_LUMINANCE_GAP = 0.15       # max allowed (vanilla-reblur)/vanilla per frame
# max slope of gap vs frame (expanding dim detection)
MAX_GAP_TREND_SLOPE = 0.005


def parse_args():
    parser = argparse.ArgumentParser(
        description="Motion luminance tracking test")
    parser.add_argument("--framework", default="macos",
                        choices=("glfw", "macos"))
    parser.add_argument("--skip_build", action="store_true")
    return parser.parse_known_args()


def get_screenshot_dir(framework):
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    return os.path.join(PROJECT_ROOT, "build_system", "glfw", "output",
                        "build", "generated", "screenshots")


def load_luminance(path):
    img = np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
    luma = img[:, :, 0] * 0.2126 + img[:, :, 1] * \
        0.7152 + img[:, :, 2] * 0.0722
    return float(np.mean(luma))


def run_capture(py, build_py, framework, use_reblur, label, passthrough_args=()):
    """Run motion_luminance_track test case and return screenshot paths."""
    cmd = [py, build_py, "--framework", framework, "--skip_build",
           "--run", "--test_case", "motion_luminance_track",
           "--headless", "true",
           "--clear_screenshots", "true", "--test_timeout", "120"]
    if use_reblur:
        cmd += ["--use_reblur", "true"]
    cmd += list(passthrough_args)
    print(f"  cmd: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=PROJECT_ROOT,
                            capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  FAIL: {label} exited with code {result.returncode}")
        if result.stderr:
            for line in result.stderr.strip().splitlines()[-5:]:
                print(f"    {line}")
        return None
    return True


def find_motion_screenshots(screenshot_dir):
    """Find motion_track_N screenshots in order."""
    paths = {}
    for i in range(5):
        pattern = f"*motion_track_{i}*"
        matches = glob.glob(os.path.join(screenshot_dir, pattern))
        if matches:
            matches.sort(key=os.path.getmtime, reverse=True)
            paths[i] = matches[0]
    # Settled screenshot
    settled = glob.glob(os.path.join(screenshot_dir, "*motion_track_settled*"))
    if settled:
        settled.sort(key=os.path.getmtime, reverse=True)
        paths["settled"] = settled[0]
    return paths


def main():
    args, extra_args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)

    print("=" * 60)
    print("  Continuous Motion Luminance Tracking")
    print("=" * 60)

    # Build
    if not args.skip_build:
        print("\nBuilding...")
        result = subprocess.run([py, build_py, "--framework", fw] + extra_args,
                                cwd=PROJECT_ROOT, capture_output=True, text=True)
        if result.returncode != 0:
            print("FAIL: build failed")
            return 1

    all_results = []

    # --- Run 1: Vanilla baseline ---
    print(f"\n{'—'*60}")
    print("  Run 1: Vanilla baseline (orbit_sweep)")
    print(f"{'—'*60}")
    ok = run_capture(py, build_py, fw, use_reblur=False, label="vanilla",
                     passthrough_args=extra_args)
    vanilla_lumas = {}
    if ok:
        all_results.append(("Vanilla: test run", True))
        paths = find_motion_screenshots(screenshot_dir)
        for key, path in sorted(paths.items(), key=lambda x: str(x[0])):
            luma = load_luminance(path)
            vanilla_lumas[key] = luma
            print(f"  Frame {key}: mean_luma={luma:.6f}")
        if len(paths) >= 3:
            all_results.append(("Vanilla: screenshots found", True))
        else:
            print("  FAIL: fewer than 3 screenshots found")
            all_results.append(("Vanilla: screenshots found", False))
    else:
        all_results.append(("Vanilla: test run", False))

    # --- Run 2: Reblur ---
    print(f"\n{'—'*60}")
    print("  Run 2: Reblur (orbit_sweep)")
    print(f"{'—'*60}")
    ok = run_capture(py, build_py, fw, use_reblur=True, label="reblur",
                     passthrough_args=extra_args)
    reblur_lumas = {}
    if ok:
        all_results.append(("Reblur: test run", True))
        paths = find_motion_screenshots(screenshot_dir)
        for key, path in sorted(paths.items(), key=lambda x: str(x[0])):
            luma = load_luminance(path)
            reblur_lumas[key] = luma
            print(f"  Frame {key}: mean_luma={luma:.6f}")
        if len(paths) >= 3:
            all_results.append(("Reblur: screenshots found", True))
        else:
            print("  FAIL: fewer than 3 screenshots found")
            all_results.append(("Reblur: screenshots found", False))
    else:
        all_results.append(("Reblur: test run", False))

    # --- Analysis ---
    print(f"\n{'—'*60}")
    print("  Analysis: Luminance Gap (vanilla - reblur) / vanilla")
    print(f"{'—'*60}")

    if vanilla_lumas and reblur_lumas:
        motion_gaps = []
        for key in sorted(set(vanilla_lumas.keys()) & set(reblur_lumas.keys()),
                          key=lambda x: str(x)):
            v = vanilla_lumas[key]
            r = reblur_lumas[key]
            gap = (v - r) / max(v, 1e-6)
            print(f"  Frame {key}: vanilla={v:.4f}  reblur={r:.4f}  "
                  f"gap={gap:.4f} ({gap*100:.1f}%)")
            if key != "settled":
                motion_gaps.append(gap)

        if motion_gaps:
            max_gap = max(motion_gaps)
            mean_gap = sum(motion_gaps) / len(motion_gaps)
            print(
                f"\n  Max gap during motion: {max_gap:.4f} ({max_gap*100:.1f}%)")
            print(
                f"  Mean gap during motion: {mean_gap:.4f} ({mean_gap*100:.1f}%)")

            # Check gap threshold
            if max_gap <= MAX_LUMINANCE_GAP:
                print(f"  PASS: max gap {max_gap:.4f} <= {MAX_LUMINANCE_GAP}")
                all_results.append(
                    (f"Max luminance gap <= {MAX_LUMINANCE_GAP}", True))
            else:
                print(f"  FAIL: max gap {max_gap:.4f} > {MAX_LUMINANCE_GAP} "
                      f"(denoised output too dim)")
                all_results.append(
                    (f"Max luminance gap <= {MAX_LUMINANCE_GAP}", False))

            # Check trend (expanding dim region)
            if len(motion_gaps) >= 3:
                x = np.arange(len(motion_gaps))
                slope = np.polyfit(x, motion_gaps, 1)[0]
                print(f"  Gap trend slope: {slope:.6f} per frame-step")
                if slope <= MAX_GAP_TREND_SLOPE:
                    print(
                        f"  PASS: gap trend {slope:.6f} <= {MAX_GAP_TREND_SLOPE}")
                    all_results.append(
                        (f"Gap trend slope <= {MAX_GAP_TREND_SLOPE}", True))
                else:
                    print(f"  FAIL: gap trend {slope:.6f} > {MAX_GAP_TREND_SLOPE} "
                          f"(dim region expanding)")
                    all_results.append(
                        (f"Gap trend slope <= {MAX_GAP_TREND_SLOPE}", False))

    # Summary
    passed = sum(1 for _, ok in all_results if ok)
    failed = sum(1 for _, ok in all_results if not ok)
    print(f"\n{'='*60}")
    print(f"  Results: {passed} passed, {failed} failed")
    for name, ok in all_results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    print(f"{'='*60}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
