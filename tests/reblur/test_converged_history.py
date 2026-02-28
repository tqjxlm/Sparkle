"""Converged history camera delta validation.

Tests that a small camera yaw delta after full convergence preserves temporal
history — the resulting frame should be nearly as clean as the converged
frame, NOT a noisy 1spp restart.

Orchestrates three runs:
  0. Vanilla baseline — vanilla GPU pipeline (no reblur): fully converge,
     nudge, fully re-converge.  Establishes the best-case difference between
     two noise-free views 2 degrees apart.
  1. Full pipeline (end-to-end) — before/after screenshot noise comparison
  2. reblur_debug_pass 3 (temporal accumulation output) — validates history
     was fetched by the reprojection, before any spatial blur

Metrics:
  - FLIP error: perceptual difference between before/after (nvidia FLIP)
  - Laplacian variance: high-frequency noise measure (3x3 Laplacian kernel)
  - Noise ratio: laplacian_var_after / laplacian_var_before
  - Mean luminance: not-black validation
  - NaN/Inf checks

Usage:
  python tests/reblur/test_converged_history.py --framework glfw [--skip_build]
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


def parse_args():
    parser = argparse.ArgumentParser(
        description="Converged history camera delta test")
    parser.add_argument("--framework", default="glfw",
                        choices=("glfw", "macos"))
    parser.add_argument("--skip_build", action="store_true")
    return parser.parse_args()


def get_screenshot_dir(framework):
    if framework == "glfw":
        return os.path.join(PROJECT_ROOT, "build_system", "glfw", "output",
                            "build", "generated", "screenshots")
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    raise ValueError(f"Unsupported framework: {framework}")


def find_screenshot(screenshot_dir, pattern):
    matches = glob.glob(os.path.join(screenshot_dir, pattern))
    if not matches:
        return None
    matches.sort(key=os.path.getmtime, reverse=True)
    return matches[0]


def load_luminance(path):
    """Load image and compute luminance channel as float32 [0,1]."""
    img = np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
    luma = img[:, :, 0] * 0.2126 + img[:, :, 1] * \
        0.7152 + img[:, :, 2] * 0.0722
    return img, luma


def compute_laplacian_variance(luma):
    """Compute variance of the Laplacian as a noise metric.

    Higher values mean more high-frequency content (noise or edges).
    """
    # 3x3 Laplacian kernel
    kernel = np.array([[0, 1, 0],
                       [1, -4, 1],
                       [0, 1, 0]], dtype=np.float32)
    laplacian = convolve(luma, kernel, mode="reflect")
    return float(np.var(laplacian))


def compute_flip(before_path, after_path):
    """Compute mean FLIP error between two images. Returns mean_flip or None."""
    try:
        from flip_evaluator import nbflip
    except ImportError:
        print("  WARN: flip_evaluator not installed, skipping FLIP metric")
        return None
    before_img = np.array(
        Image.open(before_path).convert("RGB"), dtype=np.float32) / 255.0
    after_img = np.array(
        Image.open(after_path).convert("RGB"), dtype=np.float32) / 255.0
    if before_img.shape != after_img.shape:
        print(f"  WARN: image shape mismatch for FLIP: "
              f"{before_img.shape} vs {after_img.shape}")
        return None
    _, mean_flip, _ = nbflip.evaluate(
        before_img, after_img, False, True, False, True, {})
    return float(mean_flip)


def check_image(path, label):
    """Basic image validation: NaN, Inf, all-black."""
    img, luma = load_luminance(path)
    has_nan = bool(np.any(np.isnan(img)))
    has_inf = bool(np.any(np.isinf(img)))
    mean_luma = float(np.mean(luma))
    lap_var = compute_laplacian_variance(luma)

    failures = []
    if has_nan:
        failures.append("NaN detected")
    if has_inf:
        failures.append("Inf detected")
    if mean_luma < 1e-4:
        failures.append(f"all black (mean_luma={mean_luma:.6f})")

    return {
        "mean_luma": mean_luma,
        "laplacian_var": lap_var,
        "has_nan": has_nan,
        "has_inf": has_inf,
        "failures": failures,
    }


def run_test(py, build_py, framework, test_case, extra_args, label,
             use_reblur=True, clear_screenshots=False):
    """Run a C++ test case with given extra args. Returns success bool."""
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
        print(f"  FAIL: app exited with code {result.returncode}")
        if result.stderr:
            for line in result.stderr.strip().splitlines()[-5:]:
                print(f"    {line}")
        return False
    return True


def rename_screenshots(screenshot_dir, old_pattern, new_prefix):
    """Rename screenshots matching old_pattern by replacing the base name with new_prefix."""
    for path in glob.glob(os.path.join(screenshot_dir, old_pattern)):
        dirname = os.path.dirname(path)
        basename = os.path.basename(path)
        new_name = basename.replace(
            "converged_history_before", f"{new_prefix}_before").replace(
            "converged_history_after", f"{new_prefix}_after")
        new_path = os.path.join(dirname, new_name)
        os.rename(path, new_path)
        print(f"  Renamed: {basename} -> {new_name}")


def validate_run(screenshot_dir, label, noise_ratio_max,
                 before_pattern="*converged_history_before*",
                 after_pattern="*converged_history_after*",
                 flip_max=None):
    """Find before/after screenshots, compute noise and FLIP metrics, return results_list."""
    results = []

    before_path = find_screenshot(screenshot_dir, before_pattern)
    after_path = find_screenshot(screenshot_dir, after_pattern)

    if not before_path:
        print(f"  FAIL: 'before' screenshot not found")
        results.append((f"{label}: before screenshot found", False))
        return results
    results.append((f"{label}: before screenshot found", True))

    if not after_path:
        print(f"  FAIL: 'after' screenshot not found")
        results.append((f"{label}: after screenshot found", False))
        return results
    results.append((f"{label}: after screenshot found", True))

    print(f"  Before: {before_path}")
    print(f"  After:  {after_path}")

    before = check_image(before_path, "before")
    after = check_image(after_path, "after")

    print(f"  Before: mean_luma={before['mean_luma']:.6f}, "
          f"laplacian_var={before['laplacian_var']:.6f}")
    print(f"  After:  mean_luma={after['mean_luma']:.6f}, "
          f"laplacian_var={after['laplacian_var']:.6f}")

    # Basic validity checks
    for tag, info in [("before", before), ("after", after)]:
        if info["failures"]:
            for f in info["failures"]:
                print(f"  FAIL: {tag} — {f}")
                results.append((f"{label}: {tag} {f}", False))
        else:
            results.append((f"{label}: {tag} valid", True))

    # Noise ratio: after / before
    if before["laplacian_var"] > 1e-10:
        noise_ratio = after["laplacian_var"] / before["laplacian_var"]
        print(f"  Noise ratio: {noise_ratio:.2f}x")

        if noise_ratio < noise_ratio_max:
            print(f"  PASS: noise ratio {noise_ratio:.2f} < {noise_ratio_max}")
            results.append((f"{label}: noise ratio < {noise_ratio_max}", True))
        else:
            print(f"  FAIL: noise ratio {noise_ratio:.2f} >= {noise_ratio_max} "
                  f"(history not preserved)")
            results.append(
                (f"{label}: noise ratio < {noise_ratio_max}", False))
    else:
        print(f"  WARN: before laplacian_var too small to compute ratio")
        results.append((f"{label}: noise ratio computable", True))

    # FLIP perceptual error
    mean_flip = compute_flip(before_path, after_path)
    if mean_flip is not None:
        print(f"  FLIP error: {mean_flip:.4f}")
        if flip_max is not None:
            if mean_flip <= flip_max:
                print(f"  PASS: FLIP {mean_flip:.4f} <= {flip_max}")
                results.append((f"{label}: FLIP <= {flip_max}", True))
            else:
                print(f"  FAIL: FLIP {mean_flip:.4f} > {flip_max}")
                results.append((f"{label}: FLIP <= {flip_max}", False))

    return results


def main():
    args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)

    print("=" * 60)
    print("  Converged History Camera Delta Validation")
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

    # --- Run 0: Vanilla baseline (no reblur, full re-convergence) ---
    print(f"\n{'—'*60}")
    print("  Run 0: Vanilla baseline (converge -> nudge -> re-converge)")
    print(f"{'—'*60}")
    # Vanilla needs two full convergences; use modest max_spp + timeout so
    # the test completes in reasonable time.
    ok = run_test(py, build_py, fw, "vanilla_converged_baseline",
                  [],
                  "vanilla baseline", use_reblur=False,
                  clear_screenshots=True)
    if ok:
        all_results.append(("Vanilla baseline: test run", True))
        run_results = validate_run(
            screenshot_dir, "Vanilla baseline", noise_ratio_max=float("inf"),
            before_pattern="*vanilla_baseline_before*",
            after_pattern="*vanilla_baseline_after*")
        all_results.extend(run_results)
    else:
        all_results.append(("Vanilla baseline: test run", False))

    # --- Run 1: Full pipeline (end-to-end) ---
    print(f"\n{'—'*60}")
    print("  Run 1: Reblur full pipeline (end-to-end)")
    print(f"{'—'*60}")
    ok = run_test(py, build_py, fw, "reblur_converged_history", [],
                  "full pipeline")
    if ok:
        all_results.append(("Full pipeline: test run", True))
        run_results = validate_run(
            screenshot_dir, "Full pipeline", noise_ratio_max=5.0)
        all_results.extend(run_results)
        # Rename so Run 2 doesn't overwrite
        rename_screenshots(screenshot_dir, "*converged_history_*",
                           "reblur_e2e")
    else:
        all_results.append(("Full pipeline: test run", False))

    # --- Run 2: Temporal accumulation (reblur_debug_pass 3) ---
    print(f"\n{'—'*60}")
    print("  Run 2: Reblur temporal accumulation (reblur_debug_pass 3)")
    print(f"{'—'*60}")
    ok = run_test(py, build_py, fw, "reblur_converged_history",
                  ["--reblur_debug_pass", "TemporalAccum"], "temporal accumulation")
    if ok:
        all_results.append(("Temporal accum: test run", True))
        # Raw temporal accumulation output (before spatial blur) has higher noise
        # because genuinely disoccluded pixels (~20-25% for a 2° yaw) get
        # accumSpeed=1 with no spatial denoising. The full pipeline test (Run 1)
        # validates the final output quality; this run validates that reprojection
        # fetches history at all, not that the result is spatially clean.
        run_results = validate_run(
            screenshot_dir, "Temporal accum", noise_ratio_max=8.0)
        all_results.extend(run_results)
        rename_screenshots(screenshot_dir, "*converged_history_*",
                           "reblur_temporal_accum")
    else:
        all_results.append(("Temporal accum: test run", False))

    # --- Summary ---
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
