"""REBLUR temporal pipeline validation: validates TemporalAccumulation and HistoryFix passes.

Tests:
  1. No NaN or Inf in any output
  2. No output is all black
  3. Temporal convergence: 64-frame std <= 4-frame std * 1.1
  4. HistoryFix does not amplify noise: HistoryFix std <= TemporalAccum std * 1.5
  5. Mean luminance conserved within 50% across temporal passes

Usage:
  python tests/reblur/reblur_temporal_validation.py --framework glfw [--skip_build]
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
sys.path.insert(0, PROJECT_ROOT)

from dev.utils import extract_log_path

PASS_NAMES = {
    3: "TemporalAccum",
    4: "HistoryFix",
    99: "FullPipeline",
}


def parse_args():
    parser = argparse.ArgumentParser(description="REBLUR temporal validation")
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


def run_capture(framework, debug_pass, max_spp, output_dir, label):
    """Run app with given params and capture screenshot to output_dir."""
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    cmd = [
        sys.executable, build_py, "--framework", framework, "--skip_build",
        "--run", "--test_case", "reblur_temporal_convergence",
        "--clear_screenshots", "true",
        "--headless", "true",
        "--pipeline", "gpu",
        "--use_reblur", "true",
        "--spp", "1",
        "--max_spp", str(max_spp),
        "--reblur_debug_pass", str(debug_pass),
        "--test_timeout", "120",
    ]

    print(f"\n{'='*60}", flush=True)
    print(f"  {label}", flush=True)
    print(f"  debug_pass={debug_pass}, max_spp={max_spp}", flush=True)
    print(f"{'='*60}", flush=True)

    result = subprocess.run(cmd, cwd=PROJECT_ROOT, capture_output=True, text=True)
    log_path = extract_log_path(result.stdout)
    log_info = f" — log: {log_path}" if log_path else ""
    if result.returncode != 0:
        print(f"FAIL: App crashed — {label}{log_info}", flush=True)
        return None
    print(f"  OK{log_info}", flush=True)

    # Find and copy screenshot
    screenshot_dir = get_screenshot_dir(framework)
    pattern = os.path.join(screenshot_dir, "TestScene_gpu_*.png")
    matches = glob.glob(pattern)
    if not matches:
        print(f"FAIL: No screenshot found — {label}", flush=True)
        return None
    matches.sort(key=os.path.getmtime, reverse=True)
    dst = os.path.join(output_dir, f"temporal_{debug_pass}_spp{max_spp}.png")
    shutil.copy2(matches[0], dst)
    print(f"  Screenshot: {dst}", flush=True)
    return dst


def load_screenshot(path):
    """Load screenshot as float32 RGB array."""
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def compute_luminance(img):
    """Per-pixel luminance (Rec.709)."""
    return img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722


def validate_image(name, img):
    """Validate basic properties. Returns metrics dict."""
    luma = compute_luminance(img)
    metrics = {
        "mean_luminance": float(np.mean(luma)),
        "std_luminance": float(np.std(luma)),
        "max_luminance": float(np.max(luma)),
        "black_pixel_ratio": float(np.mean(luma < 1e-4)),
        "has_nan": bool(np.any(np.isnan(img))),
        "has_inf": bool(np.any(np.isinf(img))),
    }

    print(f"\n--- {name} ---", flush=True)
    print(f"  Mean luminance:    {metrics['mean_luminance']:.6f}", flush=True)
    print(f"  Std luminance:     {metrics['std_luminance']:.6f}", flush=True)
    print(f"  Black pixel ratio: {metrics['black_pixel_ratio']:.4f}", flush=True)
    print(f"  Has NaN:           {metrics['has_nan']}", flush=True)
    print(f"  Has Inf:           {metrics['has_inf']}", flush=True)
    return metrics


def main():
    args = parse_args()

    # Build once
    if not args.skip_build:
        print("Building...", flush=True)
        build_py = os.path.join(PROJECT_ROOT, "build.py")
        result = subprocess.run(
            [sys.executable, build_py, "--framework", args.framework],
            cwd=PROJECT_ROOT, capture_output=True, text=True,
        )
        if result.returncode != 0:
            print("FAIL: Build failed", flush=True)
            if result.stderr:
                for line in result.stderr.strip().splitlines()[-10:]:
                    print(f"  {line}", flush=True)
            return 1

    output_dir = tempfile.mkdtemp(prefix="reblur_temporal_")
    print(f"Output directory: {output_dir}", flush=True)

    # --- Capture screenshots ---
    captures = {}

    # 1. TemporalAccum at 64 frames (debug_pass=3)
    path = run_capture(args.framework, 3, 64, output_dir,
                       "TemporalAccum output (64 frames)")
    if not path:
        return 1
    captures["ta_64"] = path

    # 2. HistoryFix at 64 frames (debug_pass=4)
    path = run_capture(args.framework, 4, 64, output_dir,
                       "HistoryFix output (64 frames)")
    if not path:
        return 1
    captures["hf_64"] = path

    # 3. Full pipeline at 4 frames (low convergence baseline)
    path = run_capture(args.framework, 99, 4, output_dir,
                       "Full pipeline (4 frames, low convergence)")
    if not path:
        return 1
    captures["full_4"] = path

    # 4. Full pipeline at 64 frames (converged)
    path = run_capture(args.framework, 99, 64, output_dir,
                       "Full pipeline (64 frames, converged)")
    if not path:
        return 1
    captures["full_64"] = path

    # --- Load and validate ---
    images = {k: load_screenshot(v) for k, v in captures.items()}
    metrics = {}

    metrics["ta_64"] = validate_image("TemporalAccum (64 frames)", images["ta_64"])
    metrics["hf_64"] = validate_image("HistoryFix (64 frames)", images["hf_64"])
    metrics["full_4"] = validate_image("FullPipeline (4 frames)", images["full_4"])
    metrics["full_64"] = validate_image("FullPipeline (64 frames)", images["full_64"])

    # --- Assertions ---
    failures = []

    # 1. No NaN or Inf in any output
    for key, m in metrics.items():
        if m["has_nan"]:
            failures.append(f"{key}: contains NaN values")
        if m["has_inf"]:
            failures.append(f"{key}: contains Inf values")

    # 2. No output should be all black
    for key, m in metrics.items():
        if m["mean_luminance"] < 1e-4:
            failures.append(f"{key}: output is all black (mean_luma={m['mean_luminance']:.6f})")

    # 3. Temporal convergence: 64-frame full pipeline should have less noise than 4-frame
    std_4 = metrics["full_4"]["std_luminance"]
    std_64 = metrics["full_64"]["std_luminance"]
    print(f"\nTemporal convergence: 4-frame std={std_4:.6f}, 64-frame std={std_64:.6f}",
          flush=True)
    if std_4 > 0 and std_64 > std_4 * 1.1:
        failures.append(
            f"Temporal convergence failed: 64-frame std ({std_64:.6f}) > "
            f"4-frame std ({std_4:.6f}) * 1.1 — temporal accumulation is not reducing noise"
        )

    # 4. HistoryFix should not amplify noise vs TemporalAccum
    ta_std = metrics["ta_64"]["std_luminance"]
    hf_std = metrics["hf_64"]["std_luminance"]
    print(f"HistoryFix effect: TemporalAccum std={ta_std:.6f}, HistoryFix std={hf_std:.6f}",
          flush=True)
    if ta_std > 0 and hf_std > ta_std * 1.5:
        failures.append(
            f"HistoryFix amplified noise: HistoryFix std ({hf_std:.6f}) > "
            f"TemporalAccum std ({ta_std:.6f}) * 1.5"
        )

    # 5. Mean luminance conservation across temporal passes
    ta_mean = metrics["ta_64"]["mean_luminance"]
    full_mean = metrics["full_64"]["mean_luminance"]
    if ta_mean > 0:
        ratio = abs(full_mean - ta_mean) / ta_mean
        print(f"Luminance conservation: TemporalAccum mean={ta_mean:.6f}, "
              f"FullPipeline mean={full_mean:.6f} (ratio={ratio:.4f})", flush=True)
        if ratio > 0.5:
            failures.append(
                f"Mean luminance changed too much: TemporalAccum={ta_mean:.6f} vs "
                f"FullPipeline={full_mean:.6f} (ratio={ratio:.4f} > 0.5)"
            )

    # --- Report ---
    print(f"\n{'='*60}", flush=True)
    if failures:
        print("FAIL: Temporal validation found issues:", flush=True)
        for f in failures:
            print(f"  - {f}", flush=True)
        return 1

    print("PASS: All temporal validations passed", flush=True)
    print(f"  TemporalAccum std (64f): {ta_std:.6f}", flush=True)
    print(f"  HistoryFix std (64f):    {hf_std:.6f}", flush=True)
    print(f"  Full pipeline std (4f):  {std_4:.6f}", flush=True)
    print(f"  Full pipeline std (64f): {std_64:.6f}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
