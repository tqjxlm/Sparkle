"""Diagnose REBLUR energy loss by comparing denoiser stages vs vanilla.

Captures screenshots at 64 spp for:
  - Vanilla (ground truth at this SPP)
  - REBLUR TemporalAccum (first stage with temporal history)
  - REBLUR Full pipeline (denoiser-only, no PT blend)

Produces diagnostic images:
  - Per-pixel luminance difference maps
  - Per-channel (R/G/B) energy comparison
  - Regional analysis (floor, spheres, background)
  - Statistical breakdown by luminance band
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

SPP = 64


def parse_args():
    parser = argparse.ArgumentParser(description="REBLUR energy loss diagnostic")
    parser.add_argument("--framework", default="glfw", choices=("glfw", "macos"))
    parser.add_argument("--skip_build", action="store_true")
    parser.add_argument("--spp", type=int, default=SPP)
    return parser.parse_args()


def get_screenshot_dir(framework):
    if framework == "glfw":
        return os.path.join(PROJECT_ROOT, "build_system", "glfw", "output",
                            "build", "generated", "screenshots")
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    raise ValueError(f"Unsupported framework: {framework}")


def run_capture(framework, label, extra_args, spp, clear=False):
    """Run app and return screenshot path."""
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    cmd = [sys.executable, build_py, "--framework", framework, "--skip_build",
           "--run", "--test_case", "screenshot",
           "--pipeline", "gpu", "--headless", "true"]
    if clear:
        cmd += ["--clear_screenshots", "true"]
    cmd += ["--spp", "1", "--max_spp", str(spp)] + extra_args
    print(f"  Capturing: {label}")
    print(f"    cmd: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=PROJECT_ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  FAIL: {label} exited {result.returncode}")
        if result.stderr:
            for line in result.stderr.strip().splitlines()[-5:]:
                print(f"    {line}")
        return None

    ss_dir = get_screenshot_dir(framework)
    ss_path = os.path.join(ss_dir, "screenshot.png")
    if not os.path.exists(ss_path):
        print(f"  FAIL: no screenshot at {ss_path}")
        return None

    # Save with descriptive name
    dest = os.path.join(ss_dir, f"diag_{label}.png")
    import shutil
    shutil.copy2(ss_path, dest)
    return dest


def load_image(path):
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def luminance(img):
    return img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722


def save_heatmap(arr, path, vmin=0, vmax=0.3, label=""):
    """Save array as blue-to-red heatmap."""
    normalized = np.clip((arr - vmin) / max(vmax - vmin, 1e-9), 0, 1)
    rgb = np.zeros((*arr.shape, 3), dtype=np.uint8)
    rgb[:, :, 0] = (normalized * 255).astype(np.uint8)  # Red = high
    rgb[:, :, 2] = ((1 - normalized) * 255).astype(np.uint8)  # Blue = low
    Image.fromarray(rgb).save(path)


def save_signed_heatmap(arr, path, vmax=0.15):
    """Save signed difference: blue=negative (darker), red=positive (brighter), green=neutral."""
    normalized = np.clip(arr / vmax, -1, 1)
    rgb = np.zeros((*arr.shape, 3), dtype=np.uint8)
    # Positive (brighter than vanilla): red
    rgb[:, :, 0] = (np.clip(normalized, 0, 1) * 255).astype(np.uint8)
    # Negative (darker than vanilla): blue
    rgb[:, :, 2] = (np.clip(-normalized, 0, 1) * 255).astype(np.uint8)
    # Neutral zone
    neutral = np.abs(normalized) < 0.1
    rgb[neutral] = [0, 100, 0]
    Image.fromarray(rgb).save(path)


def analyze_regions(vanilla_luma, reblur_luma):
    """Analyze energy loss by luminance band of vanilla image."""
    print("\n  Energy analysis by vanilla luminance band:")
    print(f"    {'Band':<20s} {'Vanilla mean':<15s} {'Reblur mean':<15s} {'Ratio':<10s} {'Pixel %':<10s}")
    bands = [
        ("Dark (< 0.1)", vanilla_luma < 0.1),
        ("Mid-dark (0.1-0.3)", (vanilla_luma >= 0.1) & (vanilla_luma < 0.3)),
        ("Mid (0.3-0.5)", (vanilla_luma >= 0.3) & (vanilla_luma < 0.5)),
        ("Mid-bright (0.5-0.7)", (vanilla_luma >= 0.5) & (vanilla_luma < 0.7)),
        ("Bright (> 0.7)", vanilla_luma >= 0.7),
    ]
    for name, mask in bands:
        if np.sum(mask) == 0:
            continue
        v_mean = float(np.mean(vanilla_luma[mask]))
        r_mean = float(np.mean(reblur_luma[mask]))
        ratio = r_mean / max(v_mean, 1e-9)
        pct = np.sum(mask) / mask.size * 100
        print(f"    {name:<20s} {v_mean:<15.4f} {r_mean:<15.4f} {ratio:<10.3f} {pct:<10.1f}")


def analyze_channels(vanilla_img, reblur_img):
    """Per-channel energy comparison."""
    print("\n  Per-channel energy comparison:")
    print(f"    {'Channel':<10s} {'Vanilla mean':<15s} {'Reblur mean':<15s} {'Ratio':<10s}")
    for ch, name in enumerate(["Red", "Green", "Blue"]):
        v_mean = float(np.mean(vanilla_img[:, :, ch]))
        r_mean = float(np.mean(reblur_img[:, :, ch]))
        ratio = r_mean / max(v_mean, 1e-9)
        print(f"    {name:<10s} {v_mean:<15.4f} {r_mean:<15.4f} {ratio:<10.3f}")


def analyze_spatial(vanilla_luma, reblur_luma):
    """Spatial analysis: top/bottom/left/right halves."""
    h, w = vanilla_luma.shape
    regions = {
        "Top half": (slice(0, h//2), slice(0, w)),
        "Bottom half": (slice(h//2, h), slice(0, w)),
        "Left half": (slice(0, h), slice(0, w//2)),
        "Right half": (slice(0, h), slice(w//2, w)),
        "Center": (slice(h//4, 3*h//4), slice(w//4, 3*w//4)),
    }
    print("\n  Spatial energy analysis:")
    print(f"    {'Region':<15s} {'Vanilla mean':<15s} {'Reblur mean':<15s} {'Ratio':<10s}")
    for name, (rs, cs) in regions.items():
        v = float(np.mean(vanilla_luma[rs, cs]))
        r = float(np.mean(reblur_luma[rs, cs]))
        ratio = r / max(v, 1e-9)
        print(f"    {name:<15s} {v:<15.4f} {r:<15.4f} {ratio:<10.3f}")


def main():
    args = parse_args()
    fw = args.framework
    spp = args.spp
    ss_dir = get_screenshot_dir(fw)

    print("=" * 60)
    print(f"  REBLUR Energy Loss Diagnostic (SPP={spp})")
    print("=" * 60)

    # Build
    if not args.skip_build:
        build_py = os.path.join(PROJECT_ROOT, "build.py")
        print("\nBuilding...")
        result = subprocess.run([sys.executable, build_py, "--framework", fw],
                                cwd=PROJECT_ROOT, capture_output=True, text=True)
        if result.returncode != 0:
            print("FAIL: build failed")
            return 1

    # Capture screenshots
    print("\nCapturing screenshots...")

    vanilla_path = run_capture(fw, "vanilla",
                               ["--use_reblur", "false"], spp, clear=True)
    ta_path = run_capture(fw, "temporal_accum",
                          ["--use_reblur", "true", "--reblur_debug_pass",
                           "TemporalAccum", "--reblur_no_pt_blend", "true"], spp)
    postblur_path = run_capture(fw, "postblur",
                                ["--use_reblur", "true", "--reblur_debug_pass",
                                 "PostBlur", "--reblur_no_pt_blend", "true"], spp)
    full_path = run_capture(fw, "full_denoiser",
                            ["--use_reblur", "true", "--reblur_debug_pass",
                             "Full", "--reblur_no_pt_blend", "true"], spp)

    if not all([vanilla_path, ta_path, postblur_path, full_path]):
        print("\nFAIL: could not capture all screenshots")
        return 1

    # Load images
    vanilla_img = load_image(vanilla_path)
    ta_img = load_image(ta_path)
    postblur_img = load_image(postblur_path)
    full_img = load_image(full_path)

    vanilla_luma = luminance(vanilla_img)
    ta_luma = luminance(ta_img)
    postblur_luma = luminance(postblur_img)
    full_luma = luminance(full_img)

    # === Visual Diagnostics ===
    print("\n" + "=" * 60)
    print("  Visual Diagnostics")
    print("=" * 60)

    # 1. Luminance difference: reblur_full - vanilla (signed)
    diff_full = full_luma - vanilla_luma
    save_signed_heatmap(diff_full,
                        os.path.join(ss_dir, "diag_full_minus_vanilla.png"), vmax=0.15)
    print(f"  Saved: diag_full_minus_vanilla.png (blue=darker, red=brighter)")

    # 2. Luminance difference: temporal_accum - vanilla
    diff_ta = ta_luma - vanilla_luma
    save_signed_heatmap(diff_ta,
                        os.path.join(ss_dir, "diag_ta_minus_vanilla.png"), vmax=0.15)
    print(f"  Saved: diag_ta_minus_vanilla.png")

    # 3. Absolute error heatmap
    abs_err = np.abs(full_luma - vanilla_luma)
    save_heatmap(abs_err, os.path.join(ss_dir, "diag_full_abs_error.png"),
                 vmin=0, vmax=0.2)
    print(f"  Saved: diag_full_abs_error.png (blue=low error, red=high error)")

    # 4. Side-by-side: vanilla left, full right
    h, w = vanilla_img.shape[:2]
    sidebyside = np.zeros((h, w * 2, 3), dtype=np.uint8)
    sidebyside[:, :w] = (np.clip(vanilla_img, 0, 1) * 255).astype(np.uint8)
    sidebyside[:, w:] = (np.clip(full_img, 0, 1) * 255).astype(np.uint8)
    Image.fromarray(sidebyside).save(
        os.path.join(ss_dir, "diag_sidebyside_vanilla_full.png"))
    print(f"  Saved: diag_sidebyside_vanilla_full.png")

    # === Statistical Analysis ===
    print("\n" + "=" * 60)
    print("  Statistical Analysis")
    print("=" * 60)

    print(f"\n  Overall luminance:")
    print(f"    Vanilla:       {np.mean(vanilla_luma):.4f}")
    print(f"    TemporalAccum: {np.mean(ta_luma):.4f} (ratio: {np.mean(ta_luma)/np.mean(vanilla_luma):.3f})")
    print(f"    PostBlur:      {np.mean(postblur_luma):.4f} (ratio: {np.mean(postblur_luma)/np.mean(vanilla_luma):.3f})")
    print(f"    Full denoiser: {np.mean(full_luma):.4f} (ratio: {np.mean(full_luma)/np.mean(vanilla_luma):.3f})")
    print(f"    TS energy loss: {(1 - np.mean(full_luma)/np.mean(postblur_luma))*100:.1f}%")

    print(f"\n  Luminance loss (full - vanilla):")
    print(f"    Mean:   {np.mean(diff_full):.4f}")
    print(f"    Median: {np.median(diff_full):.4f}")
    print(f"    Std:    {np.std(diff_full):.4f}")
    print(f"    Min:    {np.min(diff_full):.4f}")
    print(f"    Max:    {np.max(diff_full):.4f}")

    # Percentage of pixels that are darker vs brighter
    darker = np.sum(diff_full < -0.01)
    brighter = np.sum(diff_full > 0.01)
    neutral = diff_full.size - darker - brighter
    print(f"\n  Pixel classification (full vs vanilla):")
    print(f"    Darker (>1%):   {darker} ({darker/diff_full.size*100:.1f}%)")
    print(f"    Neutral (<1%):  {neutral} ({neutral/diff_full.size*100:.1f}%)")
    print(f"    Brighter (>1%): {brighter} ({brighter/diff_full.size*100:.1f}%)")

    # Regional analysis
    analyze_regions(vanilla_luma, full_luma)
    analyze_channels(vanilla_img, full_img)
    analyze_spatial(vanilla_luma, full_luma)

    # === FLIP if available ===
    try:
        from flip_evaluator import nbflip
        print("\n  FLIP comparison:")
        _, flip_ta, _ = nbflip.evaluate(vanilla_img, ta_img, False, True, False, True, {})
        _, flip_pb, _ = nbflip.evaluate(vanilla_img, postblur_img, False, True, False, True, {})
        _, flip_full, _ = nbflip.evaluate(vanilla_img, full_img, False, True, False, True, {})
        print(f"    TemporalAccum vs vanilla: {flip_ta:.4f}")
        print(f"    PostBlur vs vanilla:      {flip_pb:.4f}")
        print(f"    Full vs vanilla:          {flip_full:.4f}")
    except ImportError:
        print("\n  FLIP: flip_evaluator not installed, skipping")

    print(f"\n  All diagnostic images saved to: {ss_dir}")
    print("=" * 60)
    return 0


if __name__ == "__main__":
    sys.exit(main())
