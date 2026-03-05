#!/usr/bin/env python3
"""
Test: Demodulated Clamping Energy Loss

Root cause: ray_trace_split.cs.slang applies output_limit=6 clamping AFTER
demodulating diffuse radiance by dividing by albedo. For dark surfaces
(albedo=0.1), a diffuse radiance of 1.0 → demodulated 10.0 → clamped to 6.0
→ remodulated 0.6 instead of 1.0 (40% energy loss).

With default max_accumulated_frame_num=63, the spatial blur compensates and
the luminance gap is small (~1-3%). With higher max_accum (e.g. 2048), the
gap widens to ~17% because spatial blur turns off at high accumSpeed.

This test verifies that the ReBLUR denoiser output at convergence (2048 spp)
has luminance within an acceptable range of the vanilla pipeline.

The fix should either:
  A) Clamp before demodulation
  B) Scale the limit by inverse albedo for demodulated channels
"""

import argparse
import subprocess
import sys
from pathlib import Path

try:
    from PIL import Image
    import numpy as np
except ImportError:
    print("ERROR: pip install pillow numpy")
    sys.exit(1)

SCRIPT_DIR = Path(__file__).parent.parent.parent
BUILD_PY = SCRIPT_DIR / "build.py"
SUPPORTED_FRAMEWORKS = ("glfw", "macos")

# Maximum acceptable luminance gap between denoiser and vanilla (%)
MAX_LUMA_GAP_PERCENT = 5.0


def get_screenshot_dir(framework):
    if framework == "glfw":
        return SCRIPT_DIR / "build_system" / "glfw" / "output" / "build" / "generated" / "screenshots"
    if framework == "macos":
        return Path.home() / "Documents" / "sparkle" / "screenshots"
    raise ValueError(f"Unsupported framework: {framework}")


def run_pipeline(framework, use_reblur, max_spp=2048, passthrough_args=()):
    """Run the app and return the mean luminance of the final screenshot."""
    cmd = [
        sys.executable, str(BUILD_PY),
        "--framework", framework,
        "--pipeline", "gpu",
        "--use_reblur", "true" if use_reblur else "false",
        "--max_spp", str(max_spp),
        "--run", "--headless", "true",
        "--test_case", "multi_frame_screenshot",
        "--clear_screenshots", "true",
        "--skip_build",
    ] + list(passthrough_args)

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if result.returncode != 0:
        print(f"  App failed: {result.stderr[-500:]}")
        return None

    screenshot_dir = get_screenshot_dir(framework)
    files = sorted(screenshot_dir.glob("multi_frame_*.png"))
    if not files:
        print("  No screenshots found")
        return None

    # Use the last screenshot (most converged)
    img = np.array(Image.open(files[-1]), dtype=np.float32) / 255.0
    luma = float(np.mean(img[:, :, :3]))
    return luma


def main():
    parser = argparse.ArgumentParser(description="Test demodulated clamping energy loss")
    parser.add_argument("--framework", required=True, choices=SUPPORTED_FRAMEWORKS)
    args, extra_args = parser.parse_known_args()

    print("=== Test: Demodulated Clamping Energy Loss ===\n")

    # Step 1: Run vanilla baseline
    print("Running vanilla pipeline (2048 spp)...")
    vanilla_luma = run_pipeline(args.framework, use_reblur=False, passthrough_args=extra_args)
    if vanilla_luma is None:
        print("FAIL: Vanilla pipeline failed to run")
        return 1
    print(f"  Vanilla luminance: {vanilla_luma:.4f}")

    # Step 2: Run ReBLUR
    print("Running ReBLUR pipeline (2048 spp)...")
    reblur_luma = run_pipeline(args.framework, use_reblur=True, passthrough_args=extra_args)
    if reblur_luma is None:
        print("FAIL: ReBLUR pipeline failed to run")
        return 1
    print(f"  ReBLUR luminance: {reblur_luma:.4f}")

    # Step 3: Compare
    gap_pct = abs(vanilla_luma - reblur_luma) / vanilla_luma * 100
    print(f"\n  Luminance gap: {gap_pct:.1f}% (threshold: {MAX_LUMA_GAP_PERCENT}%)")

    if gap_pct <= MAX_LUMA_GAP_PERCENT:
        print(f"\nPASS: ReBLUR luminance is within {MAX_LUMA_GAP_PERCENT}% of vanilla")
        return 0
    else:
        print(f"\nFAIL: ReBLUR luminance gap ({gap_pct:.1f}%) exceeds {MAX_LUMA_GAP_PERCENT}%")
        print(f"  This indicates output_limit clamping on demodulated values")
        print(f"  is causing energy loss. See ray_trace_split.cs.slang:512-513")
        return 1


if __name__ == "__main__":
    sys.exit(main())
