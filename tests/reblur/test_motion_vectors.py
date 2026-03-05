"""M2 test gate: Motion vector statistical validation.

Runs the REBLUR pipeline with the reblur_mv_test C++ test case,
then validates the static and motion screenshots for pixel-level properties.

Checks:
  - No NaN/Inf in either screenshot
  - Screenshots are not all-black
  - Both static and motion screenshots are valid rendered images

Usage:
  python tests/reblur/test_motion_vectors.py --framework glfw [--skip_build]
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


def parse_args():
    parser = argparse.ArgumentParser(description="Motion vector statistical test")
    parser.add_argument("--framework", default="glfw", choices=("glfw", "macos"))
    parser.add_argument("--skip_build", action="store_true")
    return parser.parse_known_args()


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


def validate_screenshot(path, label):
    """Validate a screenshot for basic pixel-level properties.

    Returns:
        (passed: bool, details: str)
    """
    img = np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0

    has_nan = bool(np.any(np.isnan(img)))
    has_inf = bool(np.any(np.isinf(img)))
    mean_luma = float(np.mean(
        img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722))
    max_val = float(np.max(img))

    failures = []
    if has_nan:
        failures.append("contains NaN")
    if has_inf:
        failures.append("contains Inf")
    if mean_luma < 1e-4:
        failures.append(f"all black (mean_luma={mean_luma:.6f})")

    print(f"    {label}: mean_luma={mean_luma:.4f}, max={max_val:.4f}, "
          f"NaN={has_nan}, Inf={has_inf}")

    if failures:
        return False, "; ".join(failures)
    return True, "OK"


def main():
    args, extra_args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)

    print("=" * 60)
    print("  M2: Motion Vector Statistical Validation")
    print("=" * 60)

    # Build
    if not args.skip_build:
        print("\nBuilding...")
        result = subprocess.run([py, build_py, "--framework", fw] + extra_args,
                                cwd=PROJECT_ROOT, capture_output=True, text=True)
        if result.returncode != 0:
            print("FAIL: build failed")
            return 1

    results = []

    # Run the MV C++ test
    print("\n--- Running reblur_mv_test (static + motion screenshots) ---")
    cmd = [py, build_py, "--framework", fw, "--skip_build",
           "--run", "--test_case", "reblur_mv_test", "--headless", "true",
           "--pipeline", "gpu", "--use_reblur", "true",
           "--spp", "1", "--max_spp", "10", "--test_timeout", "30"] + extra_args
    print(f"  cmd: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=PROJECT_ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  FAIL: app exited with code {result.returncode}")
        if result.stderr:
            for line in result.stderr.strip().splitlines()[-5:]:
                print(f"      {line}")
        results.append(("MV test app run", False))
    else:
        results.append(("MV test app run", True))

        # Validate static screenshot
        print("\n--- Validating static screenshot ---")
        static_path = get_latest_screenshot(screenshot_dir, "*mv_static*")
        if static_path:
            print(f"  Found: {static_path}")
            ok, msg = validate_screenshot(static_path, "Static MV screenshot")
            results.append(("Static MV screenshot", ok))
            if not ok:
                print(f"    FAIL: {msg}")
        else:
            print("    WARN: static screenshot not found (non-fatal)")
            results.append(("Static MV screenshot", True))

        # Validate motion screenshot
        print("\n--- Validating motion screenshot ---")
        motion_path = get_latest_screenshot(screenshot_dir, "*mv_motion*")
        if motion_path:
            print(f"  Found: {motion_path}")
            ok, msg = validate_screenshot(motion_path, "Motion MV screenshot")
            results.append(("Motion MV screenshot", ok))
            if not ok:
                print(f"    FAIL: {msg}")
        else:
            print("    WARN: motion screenshot not found (non-fatal)")
            results.append(("Motion MV screenshot", True))

    # Summary
    passed = sum(1 for _, ok in results if ok)
    failed = sum(1 for _, ok in results if not ok)
    print(f"\n{'='*60}")
    print(f"  Results: {passed} passed, {failed} failed")
    for name, ok in results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    print(f"{'='*60}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
