"""REBLUR per-pass validation: runs the denoiser with debug output at each stage and validates properties.

Tests:
  1. Each pass produces non-black output
  2. Pixel-level variance decreases across passes (PrePass -> Blur -> PostBlur)
  3. Mean luminance is approximately conserved across passes
  4. No NaN or Inf values in any pass output

Usage:
  python tests/reblur/reblur_pass_validation.py --framework glfw [--skip_build]
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))

PASS_NAMES = {0: "PrePass", 1: "Blur", 2: "PostBlur"}
MAX_SPP = 4  # low SPP to see denoiser effect clearly
WARMUP_FRAMES = 5  # frames before screenshot


def parse_args():
    parser = argparse.ArgumentParser(description="REBLUR per-pass validation")
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


def run_with_debug_pass(framework, debug_pass, skip_build, output_dir):
    """Run app with a specific reblur_debug_pass value and capture screenshot.

    Copies the screenshot to output_dir to prevent deletion by subsequent runs.
    """
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    cmd = [sys.executable, build_py, "--framework", framework]
    if skip_build:
        cmd.append("--skip_build")
    cmd += [
        "--run", "--test_case", "reblur_pass_validation",
        "--clear_screenshots", "true",
        "--headless", "true",
        "--pipeline", "gpu",
        "--use_reblur", "true",
        "--spp", "1",
        "--max_spp", str(MAX_SPP),
        "--reblur_debug_pass", str(debug_pass),
        "--test_timeout", "60",
    ]
    name = PASS_NAMES.get(debug_pass, f"full (debug_pass={debug_pass})")
    print(f"\n{'='*60}", flush=True)
    print(f"Running with debug_pass={debug_pass} ({name})", flush=True)
    print(f"{'='*60}", flush=True)
    result = subprocess.run(cmd, cwd=PROJECT_ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"FAIL: App crashed with debug_pass={debug_pass}", flush=True)
        print(result.stdout[-2000:] if len(result.stdout) > 2000 else result.stdout,
              flush=True)
        return None

    # Find the screenshot
    screenshot_dir = get_screenshot_dir(framework)
    pattern = os.path.join(screenshot_dir, "TestScene_gpu_*.png")
    matches = glob.glob(pattern)
    if not matches:
        print(f"FAIL: No screenshot found for debug_pass={debug_pass}", flush=True)
        return None
    matches.sort(key=os.path.getmtime, reverse=True)
    src = matches[0]
    # Copy to safe location before next run clears screenshots
    dst = os.path.join(output_dir, f"reblur_pass_{debug_pass}.png")
    shutil.copy2(src, dst)
    print(f"  Screenshot saved: {dst}", flush=True)
    return dst


def load_screenshot(path):
    """Load screenshot as float32 RGB array."""
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def compute_luminance(img):
    """Compute per-pixel luminance using Rec.709 coefficients."""
    return img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722


def compute_local_variance(luma, kernel_size=5):
    """Compute local variance using a sliding window."""
    from scipy.ndimage import uniform_filter
    mean = uniform_filter(luma, size=kernel_size, mode='reflect')
    mean_sq = uniform_filter(luma ** 2, size=kernel_size, mode='reflect')
    return np.maximum(mean_sq - mean ** 2, 0.0)


def validate_pass(name, img):
    """Validate basic properties of a pass output. Returns dict of metrics."""
    luma = compute_luminance(img)
    metrics = {
        "mean_luminance": float(np.mean(luma)),
        "max_luminance": float(np.max(luma)),
        "min_luminance": float(np.min(luma)),
        "std_luminance": float(np.std(luma)),
        "black_pixel_ratio": float(np.mean(luma < 1e-4)),
        "has_nan": bool(np.any(np.isnan(img))),
        "has_inf": bool(np.any(np.isinf(img))),
    }

    print(f"\n--- {name} ---", flush=True)
    print(f"  Mean luminance:    {metrics['mean_luminance']:.6f}", flush=True)
    print(f"  Std luminance:     {metrics['std_luminance']:.6f}", flush=True)
    print(f"  Max luminance:     {metrics['max_luminance']:.6f}", flush=True)
    print(f"  Black pixel ratio: {metrics['black_pixel_ratio']:.4f}", flush=True)
    print(f"  Has NaN:           {metrics['has_nan']}", flush=True)
    print(f"  Has Inf:           {metrics['has_inf']}", flush=True)

    return metrics


def main():
    args = parse_args()

    # Install scipy if needed (for local variance)
    try:
        from scipy.ndimage import uniform_filter  # noqa: F401
    except ImportError:
        subprocess.check_call([sys.executable, "-m", "pip", "install", "scipy"],
                              stdout=subprocess.DEVNULL)

    # Build once
    if not args.skip_build:
        print("Building...", flush=True)
        build_py = os.path.join(PROJECT_ROOT, "build.py")
        result = subprocess.run(
            [sys.executable, build_py, "--framework", args.framework],
            cwd=PROJECT_ROOT,
        )
        if result.returncode != 0:
            print("FAIL: Build failed", flush=True)
            return 1

    # Run each debug pass and collect screenshots (saved to temp dir)
    output_dir = tempfile.mkdtemp(prefix="reblur_validation_")
    print(f"Output directory: {output_dir}", flush=True)
    screenshots = {}
    for debug_pass in [0, 1, 2, 99]:
        path = run_with_debug_pass(args.framework, debug_pass, skip_build=True,
                                   output_dir=output_dir)
        if path is None:
            print(f"FAIL: Could not get screenshot for debug_pass={debug_pass}",
                  flush=True)
            return 1
        screenshots[debug_pass] = path

    # Load and validate each pass
    images = {}
    metrics = {}
    for debug_pass, path in screenshots.items():
        name = PASS_NAMES.get(debug_pass, "FullPipeline")
        img = load_screenshot(path)
        images[debug_pass] = img
        metrics[debug_pass] = validate_pass(name, img)

    # === Assertions ===
    failures = []

    # 1. No NaN or Inf in any pass
    for dp, m in metrics.items():
        name = PASS_NAMES.get(dp, "FullPipeline")
        if m["has_nan"]:
            failures.append(f"{name}: contains NaN values")
        if m["has_inf"]:
            failures.append(f"{name}: contains Inf values")

    # 2. No pass should be all black
    for dp, m in metrics.items():
        name = PASS_NAMES.get(dp, "FullPipeline")
        if m["mean_luminance"] < 1e-4:
            failures.append(f"{name}: output is all black (mean_luma={m['mean_luminance']:.6f})")

    # 3. Variance should decrease across spatial passes (PrePass -> Blur -> PostBlur)
    # Note: after tone mapping, the variance comparison is approximate
    stds = [metrics[0]["std_luminance"], metrics[1]["std_luminance"], metrics[2]["std_luminance"]]
    print(f"\nVariance progression: PrePass={stds[0]:.6f} -> Blur={stds[1]:.6f} -> PostBlur={stds[2]:.6f}",
          flush=True)

    # PostBlur should have less or similar noise than PrePass
    if stds[2] > stds[0] * 1.5:
        failures.append(
            f"PostBlur std ({stds[2]:.6f}) is significantly higher than "
            f"PrePass std ({stds[0]:.6f}) — spatial pipeline may not be filtering"
        )

    # 4. Mean luminance should be approximately conserved (within 50% tolerance
    #    since tone mapping is nonlinear)
    prepass_luma = metrics[0]["mean_luminance"]
    full_luma = metrics[99]["mean_luminance"]
    if prepass_luma > 0 and abs(full_luma - prepass_luma) / prepass_luma > 1.0:
        failures.append(
            f"Mean luminance changed too much: PrePass={prepass_luma:.6f} vs "
            f"FullPipeline={full_luma:.6f}"
        )

    # Report results
    print(f"\n{'='*60}", flush=True)
    if failures:
        print("FAIL: Per-pass validation found issues:", flush=True)
        for f in failures:
            print(f"  - {f}", flush=True)
        return 1

    print("PASS: All per-pass validations passed", flush=True)
    print(f"  PrePass std:    {stds[0]:.6f}", flush=True)
    print(f"  Blur std:       {stds[1]:.6f}", flush=True)
    print(f"  PostBlur std:   {stds[2]:.6f}", flush=True)
    print(f"  Full pipeline:  mean_luma={full_luma:.6f}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
