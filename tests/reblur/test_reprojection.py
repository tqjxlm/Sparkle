"""M3 test gate: Reprojection statistical validation.

Runs the REBLUR reprojection C++ test (motion + settle + screenshot),
then validates the output screenshot for statistical properties:
  - No NaN/Inf
  - Not all-black
  - Reasonable pixel coverage (< 90% black)
  - Non-zero luminance variance (not a flat image)

Usage:
  python tests/reblur/test_reprojection.py --framework glfw [--skip_build]
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
    parser = argparse.ArgumentParser(
        description="Reprojection statistical test")
    parser.add_argument("--framework", default="glfw",
                        choices=("glfw", "macos"))
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


def main():
    args, extra_args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)

    print("=" * 60)
    print("  M3: Reprojection Statistical Validation")
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

    # Run the reprojection C++ test
    print("\n--- Running reprojection test (motion + settle + screenshot) ---")
    cmd = [py, build_py, "--framework", fw, "--skip_build",
           "--run", "--test_case", "reblur_reprojection", "--headless", "true",
           "--test_timeout", "60"] + extra_args
    print(f"  cmd: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=PROJECT_ROOT,
                            capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  FAIL: app exited with code {result.returncode}")
        if result.stderr:
            for line in result.stderr.strip().splitlines()[-5:]:
                print(f"    {line}")
        results.append(("Reprojection test run", False))
        # Print summary even on early failure
        _print_summary(results)
        return 1
    results.append(("Reprojection test run", True))

    # Validate the reprojection screenshot
    print("\n--- Validating reprojection screenshot ---")
    path = get_latest_screenshot(screenshot_dir, "*reprojection*")
    if not path:
        path = get_latest_screenshot(screenshot_dir)
    if not path:
        print("  FAIL: no screenshot found")
        results.append(("Screenshot found", False))
    else:
        results.append(("Screenshot found", True))
        print(f"  Screenshot: {path}")

        img = np.array(Image.open(path).convert(
            "RGB"), dtype=np.float32) / 255.0
        h, w, _ = img.shape
        luma = (img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152
                + img[:, :, 2] * 0.0722)

        has_nan = bool(np.any(np.isnan(img)))
        has_inf = bool(np.any(np.isinf(img)))
        mean_luma = float(np.mean(luma))
        max_luma = float(np.max(luma))
        std_luma = float(np.std(luma))
        black_ratio = float(np.mean(luma < 1e-4))

        print(f"  Dimensions:       {w}x{h}")
        print(f"  Mean luminance:   {mean_luma:.6f}")
        print(f"  Max luminance:    {max_luma:.6f}")
        print(f"  Std luminance:    {std_luma:.6f}")
        print(f"  Black pixel ratio: {black_ratio:.4f}")
        print(f"  Has NaN: {has_nan}")
        print(f"  Has Inf: {has_inf}")

        # Statistical checks
        if has_nan:
            print("  FAIL: NaN values detected")
            results.append(("No NaN", False))
        else:
            results.append(("No NaN", True))

        if has_inf:
            print("  FAIL: Inf values detected")
            results.append(("No Inf", False))
        else:
            results.append(("No Inf", True))

        if mean_luma < 1e-4:
            print("  FAIL: output is all black")
            results.append(("Not all black", False))
        else:
            results.append(("Not all black", True))

        # After motion + settle, should have some convergence
        if black_ratio > 0.9:
            print(f"  FAIL: too many black pixels ({black_ratio:.2%})")
            results.append(("Reasonable coverage", False))
        else:
            results.append(("Reasonable coverage", True))

        # Luminance variance should exist (not a flat image)
        if std_luma < 1e-5:
            print("  FAIL: no luminance variance (flat image)")
            results.append(("Has variance", False))
        else:
            results.append(("Has variance", True))

    _print_summary(results)
    failed = sum(1 for _, ok in results if not ok)
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
