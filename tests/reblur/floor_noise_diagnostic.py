"""Floor noise diagnostic for REBLUR denoiser.

Analyzes the floor region specifically to understand why the denoised output
has visible noise even with converged history.
"""

import glob
import os
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import convolve

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
SCREENSHOT_DIR = os.path.expanduser("~/Documents/sparkle/screenshots")


def load_image(path):
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def load_luminance(path):
    img = load_image(path)
    luma = img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722
    return img, luma


def laplacian_variance(luma):
    kernel = np.array([[0, 1, 0], [1, -4, 1], [0, 1, 0]], dtype=np.float32)
    lap = convolve(luma, kernel, mode="reflect")
    return float(np.var(lap))


def per_pixel_std(luma):
    """Compute local noise as std in 5x5 neighborhoods."""
    from scipy.ndimage import uniform_filter
    mean = uniform_filter(luma, size=5, mode='reflect')
    sqmean = uniform_filter(luma * luma, size=5, mode='reflect')
    local_var = np.maximum(sqmean - mean * mean, 0)
    return np.sqrt(local_var)


def identify_floor_region(luma):
    """Identify the floor region: lower half, moderate luminance, smooth."""
    h, w = luma.shape
    # Floor is roughly the lower 40% of the image, excluding very dark and very bright
    floor_mask = np.zeros_like(luma, dtype=bool)
    lower_region = slice(int(h * 0.6), h)
    floor_mask[lower_region, :] = True
    # Floor has moderate luminance (not sky, not shadow)
    floor_mask &= (luma > 0.1) & (luma < 0.8)
    # Exclude object regions (local high variance in vanilla)
    return floor_mask


def analyze_region(luma, mask, label):
    """Compute noise statistics for a masked region."""
    pixels = luma[mask]
    if len(pixels) == 0:
        print(f"  {label}: no pixels in region")
        return {}

    local_std = per_pixel_std(luma)
    region_std = local_std[mask]

    stats = {
        "pixel_count": len(pixels),
        "mean_luma": float(np.mean(pixels)),
        "std_luma": float(np.std(pixels)),
        "mean_local_std": float(np.mean(region_std)),
        "median_local_std": float(np.median(region_std)),
        "p95_local_std": float(np.percentile(region_std, 95)),
        "p99_local_std": float(np.percentile(region_std, 99)),
        "laplacian_var": laplacian_variance(luma * mask.astype(np.float32)),
    }
    print(f"  {label}:")
    print(f"    pixels:         {stats['pixel_count']}")
    print(f"    mean_luma:      {stats['mean_luma']:.6f}")
    print(f"    std_luma:       {stats['std_luma']:.6f}")
    print(f"    mean_local_std: {stats['mean_local_std']:.6f}")
    print(f"    median_local_std: {stats['median_local_std']:.6f}")
    print(f"    p95_local_std:  {stats['p95_local_std']:.6f}")
    print(f"    p99_local_std:  {stats['p99_local_std']:.6f}")
    return stats


def main():
    screenshots = {
        "vanilla_before": "vanilla_baseline_before.png",
        "vanilla_after": "vanilla_baseline_after.png",
        "denoised_before": "reblur_denoised_only_before.png",
        "denoised_after": "reblur_denoised_only_after.png",
        "e2e_before": "reblur_e2e_before.png",
        "e2e_after": "reblur_e2e_after.png",
        "ta_after": "reblur_temporal_accum_after.png",
    }

    # Load all available images
    images = {}
    for key, filename in screenshots.items():
        path = os.path.join(SCREENSHOT_DIR, filename)
        if os.path.exists(path):
            img, luma = load_luminance(path)
            images[key] = {"img": img, "luma": luma, "path": path}
        else:
            print(f"  SKIP: {filename} not found")

    if "vanilla_after" not in images:
        print("ERROR: vanilla_baseline_after.png required for floor mask")
        return 1

    # Use vanilla image to identify floor
    vanilla_luma = images["vanilla_after"]["luma"]
    floor_mask = identify_floor_region(vanilla_luma)
    print(f"Floor mask: {np.sum(floor_mask)} pixels "
          f"({np.sum(floor_mask)/floor_mask.size*100:.1f}% of image)")

    # Non-floor geometry: upper part, moderate luminance
    h, w = vanilla_luma.shape
    obj_mask = np.zeros_like(vanilla_luma, dtype=bool)
    obj_mask[:int(h * 0.6), :] = True
    obj_mask &= (vanilla_luma > 0.05)

    print("\n" + "=" * 60)
    print("  FLOOR REGION ANALYSIS")
    print("=" * 60)
    for key in ["vanilla_before", "vanilla_after", "denoised_before",
                "denoised_after", "e2e_before", "e2e_after", "ta_after"]:
        if key in images:
            print(f"\n--- {key} ---")
            analyze_region(images[key]["luma"], floor_mask, "floor")

    # Compute noise ratios for floor specifically
    print("\n" + "=" * 60)
    print("  FLOOR NOISE RATIOS")
    print("=" * 60)

    def floor_lap_var(key):
        if key not in images:
            return None
        luma = images[key]["luma"]
        kernel = np.array([[0, 1, 0], [1, -4, 1], [0, 1, 0]], dtype=np.float32)
        lap = convolve(luma, kernel, mode="reflect")
        return float(np.var(lap[floor_mask]))

    vanilla_flv = floor_lap_var("vanilla_after")
    denoised_before_flv = floor_lap_var("denoised_before")
    denoised_after_flv = floor_lap_var("denoised_after")
    e2e_before_flv = floor_lap_var("e2e_before")
    e2e_after_flv = floor_lap_var("e2e_after")
    ta_after_flv = floor_lap_var("ta_after")

    print(f"  vanilla_after floor lap_var:    {vanilla_flv:.6f}")
    if denoised_before_flv is not None:
        print(f"  denoised_before floor lap_var: {denoised_before_flv:.6f}")
    if denoised_after_flv is not None:
        print(f"  denoised_after floor lap_var:  {denoised_after_flv:.6f}")
    if e2e_before_flv is not None:
        print(f"  e2e_before floor lap_var:      {e2e_before_flv:.6f}")
    if e2e_after_flv is not None:
        print(f"  e2e_after floor lap_var:       {e2e_after_flv:.6f}")
    if ta_after_flv is not None:
        print(f"  ta_after floor lap_var:        {ta_after_flv:.6f}")

    if vanilla_flv and vanilla_flv > 1e-10:
        print(f"\n  Noise ratio (denoised_before / vanilla): "
              f"{denoised_before_flv / vanilla_flv:.2f}x" if denoised_before_flv else "")
        if denoised_after_flv:
            print(f"  Noise ratio (denoised_after / vanilla):  "
                  f"{denoised_after_flv / vanilla_flv:.2f}x")
        if e2e_after_flv:
            print(f"  Noise ratio (e2e_after / vanilla):       "
                  f"{e2e_after_flv / vanilla_flv:.2f}x")
        if ta_after_flv:
            print(f"  Noise ratio (ta_after / vanilla):        "
                  f"{ta_after_flv / vanilla_flv:.2f}x")
    if denoised_before_flv and denoised_before_flv > 1e-10 and denoised_after_flv:
        print(f"\n  Noise ratio (denoised_after / denoised_before): "
              f"{denoised_after_flv / denoised_before_flv:.2f}x")

    # Per-pixel noise comparison: floor region
    print("\n" + "=" * 60)
    print("  PER-PIXEL NOISE DISTRIBUTION (FLOOR)")
    print("=" * 60)
    for key in ["vanilla_after", "denoised_before", "denoised_after",
                "e2e_after", "ta_after"]:
        if key in images:
            local_std = per_pixel_std(images[key]["luma"])
            floor_std = local_std[floor_mask]
            print(f"  {key:25s}: mean_std={np.mean(floor_std):.6f} "
                  f"median={np.median(floor_std):.6f} "
                  f"p95={np.percentile(floor_std, 95):.6f}")

    # Color channel analysis for floor
    print("\n" + "=" * 60)
    print("  FLOOR COLOR CHANNEL ANALYSIS")
    print("=" * 60)
    for key in ["vanilla_after", "denoised_before", "denoised_after"]:
        if key in images:
            img = images[key]["img"]
            print(f"\n  {key}:")
            for ch, name in enumerate(["R", "G", "B"]):
                ch_data = img[:, :, ch][floor_mask]
                print(f"    {name}: mean={np.mean(ch_data):.4f} "
                      f"std={np.std(ch_data):.4f}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
