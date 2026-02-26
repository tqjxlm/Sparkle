#!/usr/bin/env python3
"""
Test: ReBLUR Convergence Stability

Measures frame-to-frame instability at convergence (2048 spp). After 2048 frames,
the denoiser output should be nearly stable — consecutive frames should differ
by very little.

Metrics:
- Percentage of pixels changing by >1/255 per frame (captures subtle flicker)
- Percentage of pixels changing by >5/255 per frame (captures visible flicker)

The vanilla pipeline is used as a reference baseline. ReBLUR instability should
be within a small multiple of vanilla's instability.

Root causes of instability:
1. Temporal accumulation capped at 63 frames (1/64 ≈ 1.6% new sample weight)
2. Rotating Poisson disk in spatial blur causes frame-to-frame shifts
3. Antilag resets on specular surfaces due to per-frame variance
4. No firefly suppression in temporal accumulation
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

# Maximum acceptable percentage of pixels changing by >1/255 per frame
# Vanilla baseline: ~0.18%. Target: within 3x of vanilla.
MAX_INSTABILITY_PCT_1 = 0.5
# Maximum acceptable percentage of pixels changing by >5/255 per frame (visible flicker)
# Vanilla baseline: ~0.00%. Any visible flicker is a regression.
MAX_INSTABILITY_PCT_5 = 0.1
# Maximum acceptable ratio of denoiser instability to vanilla instability
MAX_INSTABILITY_RATIO = 3.0
# Maximum acceptable mean luminance gap vs vanilla (energy conservation)
MAX_LUMA_GAP_PCT = 3.0


def get_screenshot_dir(framework):
    if framework == "glfw":
        return SCRIPT_DIR / "build_system" / "glfw" / "output" / "build" / "generated" / "screenshots"
    if framework == "macos":
        return Path.home() / "Documents" / "sparkle" / "screenshots"
    raise ValueError(f"Unsupported framework: {framework}")


def run_pipeline(framework, use_reblur, max_spp=2048):
    """Run the app and return frame-to-frame instability metrics."""
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
    ]

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if result.returncode != 0:
        print(f"  App failed: {result.stderr[-500:]}")
        return None

    screenshot_dir = get_screenshot_dir(framework)
    files = sorted(screenshot_dir.glob("multi_frame_*.png"))
    if len(files) < 2:
        print(f"  Need at least 2 screenshots, found {len(files)}")
        return None

    # Measure frame-to-frame differences for all consecutive pairs
    frames = []
    for f in files:
        frames.append(np.array(Image.open(f), dtype=np.float32) / 255.0)

    pct_1_list = []
    pct_5_list = []
    mean_diff_list = []
    for i in range(len(frames) - 1):
        diff = np.abs(frames[i + 1] - frames[i])
        mean_diff_list.append(float(np.mean(diff)))
        pct_1_list.append(float(np.mean(np.any(diff > 1.0 / 255, axis=2)) * 100))
        pct_5_list.append(float(np.mean(np.any(diff > 5.0 / 255, axis=2)) * 100))

    return {
        "mean_diff": np.mean(mean_diff_list),
        "pct_1": np.mean(pct_1_list),
        "pct_5": np.mean(pct_5_list),
        "luma": float(np.mean(frames[-1][:, :, :3])),
    }


def main():
    parser = argparse.ArgumentParser(description="Test ReBLUR convergence stability")
    parser.add_argument("--framework", required=True, choices=SUPPORTED_FRAMEWORKS)
    args = parser.parse_args()

    print("=== Test: ReBLUR Convergence Stability ===\n")

    # Step 1: Run vanilla baseline
    print("Running vanilla pipeline (2048 spp)...")
    vanilla = run_pipeline(args.framework, use_reblur=False)
    if vanilla is None:
        print("FAIL: Vanilla pipeline failed to run")
        return 1
    print(f"  Instability: {vanilla['pct_1']:.2f}% pixels >1/255, "
          f"{vanilla['pct_5']:.2f}% >5/255, mean_diff={vanilla['mean_diff']:.6f}")

    # Step 2: Run ReBLUR
    print("\nRunning ReBLUR pipeline (2048 spp)...")
    reblur = run_pipeline(args.framework, use_reblur=True)
    if reblur is None:
        print("FAIL: ReBLUR pipeline failed to run")
        return 1
    print(f"  Instability: {reblur['pct_1']:.2f}% pixels >1/255, "
          f"{reblur['pct_5']:.2f}% >5/255, mean_diff={reblur['mean_diff']:.6f}")

    # Step 3: Evaluate
    ratio = reblur["pct_1"] / max(vanilla["pct_1"], 0.001)
    luma_gap_pct = abs(reblur["luma"] - vanilla["luma"]) / max(vanilla["luma"], 1e-6) * 100

    print(f"\n--- Results ---")
    print(f"  >1/255 instability: {reblur['pct_1']:.2f}% (threshold: {MAX_INSTABILITY_PCT_1}%)")
    print(f"  >5/255 instability: {reblur['pct_5']:.2f}% (threshold: {MAX_INSTABILITY_PCT_5}%)")
    print(f"  Instability ratio vs vanilla: {ratio:.1f}x (threshold: {MAX_INSTABILITY_RATIO}x)")
    print(f"  Luminance gap vs vanilla: {luma_gap_pct:.2f}% (threshold: {MAX_LUMA_GAP_PCT}%)")
    print(f"  Mean luminance — vanilla: {vanilla['luma']:.4f}, reblur: {reblur['luma']:.4f}")

    passed = True

    if reblur["pct_1"] > MAX_INSTABILITY_PCT_1:
        print(f"\n  FAIL: >1/255 instability ({reblur['pct_1']:.2f}%) exceeds {MAX_INSTABILITY_PCT_1}%")
        passed = False

    if reblur["pct_5"] > MAX_INSTABILITY_PCT_5:
        print(f"  FAIL: >5/255 instability ({reblur['pct_5']:.2f}%) exceeds {MAX_INSTABILITY_PCT_5}%")
        passed = False

    if ratio > MAX_INSTABILITY_RATIO:
        print(f"  FAIL: ReBLUR is {ratio:.1f}x more unstable than vanilla (threshold: {MAX_INSTABILITY_RATIO}x)")
        passed = False

    if luma_gap_pct > MAX_LUMA_GAP_PCT:
        print(f"  FAIL: Luminance gap ({luma_gap_pct:.2f}%) exceeds {MAX_LUMA_GAP_PCT}%")
        passed = False

    if passed:
        print(f"\nPASS: ReBLUR convergence is acceptable")
        return 0
    else:
        print(f"\nFAIL: ReBLUR has excessive frame-to-frame instability at convergence")
        return 1


if __name__ == "__main__":
    sys.exit(main())
