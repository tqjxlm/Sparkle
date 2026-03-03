"""Diagnostic: Compare PostBlur vs Full pipeline noise levels.

Measures the noise reduction achieved by Temporal Stabilization (TS)
by comparing PostBlur-only output with full pipeline output.
"""
import numpy as np
from PIL import Image
from scipy.ndimage import convolve, uniform_filter
import sys
import os

SCREENSHOT_DIR = "/tmp/sparkle_debug"


def load_luma(path):
    img = np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
    luma = img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722
    return img, luma


def get_floor_mask(luma):
    h, w = luma.shape
    mask = np.zeros_like(luma, dtype=bool)
    mask[int(h * 0.6):, :] = True
    mask &= (luma > 0.05) & (luma < 0.9)
    return mask


def laplacian_var(luma, mask):
    kernel = np.array([[0, 1, 0], [1, -4, 1], [0, 1, 0]], dtype=np.float32)
    lap = convolve(luma, kernel, mode="reflect")
    return float(np.var(lap[mask]))


def local_std(luma, mask):
    mean = uniform_filter(luma, size=5, mode='reflect')
    sqmean = uniform_filter(luma * luma, size=5, mode='reflect')
    local_var = np.maximum(sqmean - mean * mean, 0)
    lstd = np.sqrt(local_var)
    return float(np.mean(lstd[mask]))


def per_channel_noise(img, mask):
    kernel = np.array([[0, 1, 0], [1, -4, 1], [0, 1, 0]], dtype=np.float32)
    results = {}
    for i, ch in enumerate(["R", "G", "B"]):
        lap = convolve(img[:, :, i], kernel, mode="reflect")
        results[ch] = float(np.var(lap[mask]))
    return results


def analyze(name, path, floor_mask=None):
    img, luma = load_luma(path)
    if floor_mask is None:
        floor_mask = get_floor_mask(luma)
    lv = laplacian_var(luma, floor_mask)
    ls = local_std(luma, floor_mask)
    mean_luma = float(np.mean(luma[floor_mask]))
    ch = per_channel_noise(img, floor_mask)
    print(f"\n  {name}:")
    print(f"    mean luma:    {mean_luma:.4f}")
    print(f"    lap_var:      {lv:.6f}")
    print(f"    local_std:    {ls:.6f}")
    print(f"    per-ch lap_var: R={ch['R']:.6f} G={ch['G']:.6f} B={ch['B']:.6f}")
    return lv, ls, mean_luma, floor_mask


def main():
    print("=" * 60)
    print("  TS Noise Reduction Diagnostic")
    print("=" * 60)

    vanilla_path = os.path.join(SCREENSHOT_DIR, "vanilla.png")
    postblur_path = os.path.join(SCREENSHOT_DIR, "postblur.png")
    full_path = os.path.join(SCREENSHOT_DIR, "full.png")

    files = {
        "Vanilla (2048spp)": vanilla_path,
        "PostBlur (no TS)": postblur_path,
        "Full pipeline (with TS)": full_path,
    }

    for name, path in files.items():
        if not os.path.exists(path):
            print(f"  MISSING: {path}")
            return 1

    # Use vanilla's floor mask for all
    _, vanilla_luma = load_luma(vanilla_path)
    floor_mask = get_floor_mask(vanilla_luma)
    pixel_count = int(np.sum(floor_mask))
    print(f"\n  Floor mask: {pixel_count} pixels")

    results = {}
    for name, path in files.items():
        lv, ls, ml, _ = analyze(name, path, floor_mask)
        results[name] = (lv, ls, ml)

    v_lv, v_ls, _ = results["Vanilla (2048spp)"]
    pb_lv, pb_ls, _ = results["PostBlur (no TS)"]
    f_lv, f_ls, _ = results["Full pipeline (with TS)"]

    print(f"\n  --- Noise Reduction Analysis ---")
    print(f"  PostBlur/Vanilla lap_var ratio: {pb_lv/max(v_lv,1e-10):.2f}x")
    print(f"  Full/Vanilla lap_var ratio:     {f_lv/max(v_lv,1e-10):.2f}x")
    print(f"  TS noise reduction (PB→Full):   {pb_lv/max(f_lv,1e-10):.2f}x (lap_var)")
    print(f"  TS noise reduction (PB→Full):   {pb_ls/max(f_ls,1e-10):.2f}x (local_std)")
    print(f"  Theoretical TS reduction:       ~{255**0.5:.1f}x (sqrt(255), amplitude)")
    print(f"  Theoretical TS reduction:       ~{255:.0f}x (variance)")

    # Compute what noise level TS *would* need to achieve to match vanilla
    print(f"\n  --- Gap Analysis ---")
    remaining_noise = f_lv - v_lv
    if remaining_noise > 0:
        print(f"  Excess lap_var above vanilla: {remaining_noise:.6f}")
        print(f"  PostBlur excess above vanilla: {pb_lv - v_lv:.6f}")
        print(f"  TS removed {(1 - (f_lv - v_lv)/(pb_lv - v_lv))*100:.1f}% of PostBlur excess noise")
    else:
        print(f"  Full pipeline is cleaner than vanilla!")


if __name__ == "__main__":
    main()
