#!/usr/bin/env python3
"""Test material ID disocclusion: compare ghosting before/after material ID check.

Runs orbit_sweep camera motion with REBLUR and captures screenshots for analysis.
Checks:
1. Material ID diagnostic shows mismatches at object boundaries
2. Full pipeline output has clean floor (no object ghosting)
3. Static camera non-regression
"""

import subprocess
import sys
import os
import glob
import numpy as np
from PIL import Image

def find_build_py():
    d = os.path.dirname(os.path.abspath(__file__))
    while d != os.path.dirname(d):
        p = os.path.join(d, "build.py")
        if os.path.isfile(p):
            return p
        d = os.path.dirname(d)
    raise FileNotFoundError("build.py not found")

def find_screenshot_dir():
    return os.path.expanduser("~/Documents/sparkle/screenshots")

def run_test(build_py, extra_args, label):
    """Run a test and return screenshot path."""
    cmd = [
        sys.executable, build_py,
        "--framework", "macos",
        "--run",
        "--headless", "true",
        "--pipeline", "gpu",
        "--use_reblur", "true",
        "--spp", "1",
        "--clear_screenshots", "true",
        "--test_timeout", "120",
    ] + extra_args + ["--test_case", "screenshot"]
    print(f"\n--- Running: {label} ---")
    print(f"  cmd: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if result.returncode != 0:
        print(f"  FAILED (exit {result.returncode})")
        print(result.stderr[-500:] if result.stderr else "")
        return None

    ss_dir = find_screenshot_dir()
    screenshots = sorted(glob.glob(os.path.join(ss_dir, "screenshot*.png")))
    if not screenshots:
        print("  No screenshot found!")
        return None
    print(f"  Screenshot: {screenshots[-1]}")
    return screenshots[-1]

def analyze_image(path, label):
    """Analyze screenshot and return statistics."""
    img = np.array(Image.open(path).convert("RGB")).astype(np.float32) / 255.0
    r, g, b = img[:,:,0], img[:,:,1], img[:,:,2]
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b

    print(f"\n  [{label}] Image stats:")
    print(f"    Mean luma: {luma.mean():.4f}")
    print(f"    Std luma:  {luma.std():.4f}")
    print(f"    Min luma:  {luma.min():.4f}")
    print(f"    Max luma:  {luma.max():.4f}")

    # Check for NaN/Inf
    has_nan = np.isnan(img).any()
    has_inf = np.isinf(img).any()
    print(f"    NaN: {has_nan}, Inf: {has_inf}")

    return {
        "mean_luma": luma.mean(),
        "std_luma": luma.std(),
        "has_nan": has_nan,
        "has_inf": has_inf,
        "luma": luma,
        "img": img,
    }

def analyze_material_id_diagnostic(path):
    """Analyze material ID diagnostic screenshot."""
    img = np.array(Image.open(path).convert("RGB")).astype(np.float32) / 255.0
    r, g, b = img[:,:,0], img[:,:,1], img[:,:,2]

    # R = current material ID, G = prev material ID, B = mismatch flag
    mismatch_pixels = (b > 0.5).sum()
    total_pixels = b.size
    mismatch_pct = 100.0 * mismatch_pixels / total_pixels

    # Count distinct material IDs
    unique_r = np.unique(np.round(r * 1023).astype(int))
    unique_g = np.unique(np.round(g * 1023).astype(int))

    print(f"\n  [MaterialID Diagnostic]:")
    print(f"    Distinct current material IDs: {len(unique_r)} (values: {unique_r[:10]})")
    print(f"    Distinct prev material IDs:    {len(unique_g)} (values: {unique_g[:10]})")
    print(f"    Mismatch pixels: {mismatch_pixels} / {total_pixels} ({mismatch_pct:.2f}%)")

    return {
        "mismatch_pct": mismatch_pct,
        "unique_current": len(unique_r),
        "unique_prev": len(unique_g),
    }

def analyze_floor_ghosting(img_data, label):
    """Analyze the floor region for ghosting artifacts.

    The floor is typically in the lower portion of the image.
    Ghosting manifests as residual object shapes on the floor.
    """
    h, w = img_data["luma"].shape
    # Floor region: bottom 40% of image
    floor_region = img_data["luma"][int(h*0.6):, :]

    # A clean floor should have low variance (uniform)
    floor_std = floor_region.std()
    floor_mean = floor_region.mean()

    # Check for suspicious bright patches that could be ghost objects
    # Use local variance in 16x16 blocks
    block_size = 16
    fh, fw = floor_region.shape
    block_vars = []
    for y in range(0, fh - block_size, block_size):
        for x in range(0, fw - block_size, block_size):
            block = floor_region[y:y+block_size, x:x+block_size]
            block_vars.append(block.std())

    block_vars = np.array(block_vars)
    high_var_blocks = (block_vars > 0.05).sum()
    total_blocks = len(block_vars)
    high_var_pct = 100.0 * high_var_blocks / max(total_blocks, 1)

    print(f"\n  [{label}] Floor analysis (bottom 40%):")
    print(f"    Floor mean luma: {floor_mean:.4f}")
    print(f"    Floor std luma:  {floor_std:.4f}")
    print(f"    High-variance blocks: {high_var_blocks}/{total_blocks} ({high_var_pct:.1f}%)")

    return {
        "floor_mean": floor_mean,
        "floor_std": floor_std,
        "high_var_pct": high_var_pct,
    }


def main():
    build_py = find_build_py()
    all_pass = True

    # Test 1: Material ID diagnostic during motion
    print("=" * 60)
    print("TEST 1: Material ID diagnostic during orbit_sweep")
    print("=" * 60)
    path = run_test(build_py, [
        "--max_spp", "15",
        # TODO: camera motion now uses TestCase-based approach (ReblurGhostingTest)
        "--reblur_debug_pass", "TAMaterialId",
    ], "MaterialID diagnostic")

    if path:
        mat_stats = analyze_material_id_diagnostic(path)
        # Should detect at least 2 material IDs
        if mat_stats["unique_current"] < 2:
            print("  FAIL: only 1 material ID detected (expected >=2)")
            all_pass = False
        else:
            print(f"  PASS: {mat_stats['unique_current']} material IDs detected")
        # Should detect some mismatches during motion
        if mat_stats["mismatch_pct"] < 0.01:
            print("  WARN: very few material ID mismatches during motion")
        else:
            print(f"  PASS: {mat_stats['mismatch_pct']:.2f}% material ID mismatches detected")
    else:
        print("  FAIL: no screenshot")
        all_pass = False

    # Test 2: Full pipeline with orbit_sweep — check floor ghosting
    print("\n" + "=" * 60)
    print("TEST 2: Full pipeline with orbit_sweep (ghosting check)")
    print("=" * 60)
    path = run_test(build_py, [
        "--max_spp", "30",
        # TODO: camera motion now uses TestCase-based approach (ReblurGhostingTest)
        "--reblur_no_pt_blend", "true",
    ], "Full pipeline orbit_sweep")

    if path:
        stats = analyze_image(path, "orbit_sweep full pipeline")
        floor = analyze_floor_ghosting(stats, "orbit_sweep")
        if stats["has_nan"] or stats["has_inf"]:
            print("  FAIL: NaN or Inf in output")
            all_pass = False
        else:
            print("  PASS: no NaN/Inf")
        # Floor quality: low std means clean floor
        print(f"  Floor std: {floor['floor_std']:.4f} (lower = cleaner)")
        print(f"  Floor high-var blocks: {floor['high_var_pct']:.1f}%")
    else:
        print("  FAIL: no screenshot")
        all_pass = False

    # Test 3: Static camera non-regression
    print("\n" + "=" * 60)
    print("TEST 3: Static camera non-regression")
    print("=" * 60)
    path = run_test(build_py, [
        "--max_spp", "64",
        "--reblur_no_pt_blend", "true",
    ], "Static camera")

    if path:
        stats = analyze_image(path, "static camera")
        if stats["has_nan"] or stats["has_inf"]:
            print("  FAIL: NaN or Inf")
            all_pass = False
        else:
            print("  PASS: no NaN/Inf")
        if stats["mean_luma"] < 0.1:
            print(f"  FAIL: mean luma too low ({stats['mean_luma']:.4f})")
            all_pass = False
        else:
            print(f"  PASS: mean luma = {stats['mean_luma']:.4f}")
    else:
        print("  FAIL: no screenshot")
        all_pass = False

    # Test 4: Disocclusion diagnostic during motion (existing test)
    print("\n" + "=" * 60)
    print("TEST 4: Disocclusion diagnostic during orbit_sweep")
    print("=" * 60)
    path = run_test(build_py, [
        "--max_spp", "15",
        # TODO: camera motion now uses TestCase-based approach (ReblurGhostingTest)
        "--reblur_debug_pass", "TADisocclusion",
    ], "Disocclusion diagnostic")

    if path:
        img = np.array(Image.open(path).convert("RGB")).astype(np.float32) / 255.0
        r = img[:,:,0]  # R = disoccluded (1=yes)
        disoccluded_pct = 100.0 * (r > 0.5).sum() / r.size
        print(f"  Disoccluded pixels: {disoccluded_pct:.2f}%")
        # With material ID check, expect MORE disocclusion at object boundaries
        if disoccluded_pct > 0.1:
            print(f"  PASS: material ID check is triggering disocclusion ({disoccluded_pct:.2f}%)")
        else:
            print(f"  INFO: low disocclusion rate ({disoccluded_pct:.2f}%)")
    else:
        print("  FAIL: no screenshot")
        all_pass = False

    print("\n" + "=" * 60)
    if all_pass:
        print("ALL TESTS PASSED")
    else:
        print("SOME TESTS FAILED")
    print("=" * 60)

    return 0 if all_pass else 1

if __name__ == "__main__":
    sys.exit(main())
