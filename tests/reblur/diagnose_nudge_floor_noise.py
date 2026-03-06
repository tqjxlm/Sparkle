"""Post-nudge floor noise diagnostic for REBLUR denoiser.

Investigates WHY reprojected floor pixels have more noise after a 2-degree
camera yaw nudge, despite >99% valid reprojection.

Methodology:
  For each denoiser stage (TA-only, PostBlur, Full, Denoised-only),
  run the reblur_converged_history test and compare floor noise in the
  "before" (fully converged) vs "after" (5 frames post-nudge) screenshots.

  Also captures the TADisocclusion mask to segment history vs disoccluded
  pixels, and measures noise ONLY in reprojected floor pixels.

Stages captured:
  1. Denoised-only (Full pipeline, --reblur_no_pt_blend)
  2. TemporalAccum only (--reblur_debug_pass TemporalAccum)
  3. PostBlur only (--reblur_debug_pass PostBlur)
  4. TADisocclusion mask (--reblur_debug_pass TADisocclusion)

Floor region: lower 40% of image, moderate luminance (0.05 < luma < 0.85).

Usage:
  python tests/reblur/diagnose_nudge_floor_noise.py --framework glfw [--skip_build]
"""

import argparse
import glob
import os
import subprocess
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import convolve, uniform_filter

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, PROJECT_ROOT)


def parse_args():
    parser = argparse.ArgumentParser(description="Post-nudge floor noise diagnostic")
    parser.add_argument("--framework", default="glfw", choices=("glfw", "macos"))
    parser.add_argument("--skip_build", action="store_true")
    return parser.parse_known_args()


def get_screenshot_dir(framework):
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    return os.path.join(PROJECT_ROOT, "build_system", "glfw", "output",
                        "build", "generated", "screenshots")


def find_screenshot(screenshot_dir, pattern):
    matches = glob.glob(os.path.join(screenshot_dir, pattern))
    if not matches:
        return None
    matches.sort(key=os.path.getmtime, reverse=True)
    return matches[0]


def load_luminance(path):
    img = np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
    luma = img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722
    return img, luma


def load_image(path):
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def get_floor_mask(luma):
    """Floor = lower 40% of image, moderate luminance."""
    h, w = luma.shape
    mask = np.zeros_like(luma, dtype=bool)
    mask[int(h * 0.6):, :] = True
    mask &= (luma > 0.05) & (luma < 0.85)
    return mask


def floor_laplacian_var(luma, mask):
    kernel = np.array([[0, 1, 0], [1, -4, 1], [0, 1, 0]], dtype=np.float32)
    lap = convolve(luma, kernel, mode="reflect")
    return float(np.var(lap[mask]))


def floor_local_std_mean(luma, mask, window=5):
    mean = uniform_filter(luma, size=window, mode='reflect')
    sqmean = uniform_filter(luma * luma, size=window, mode='reflect')
    local_var = np.maximum(sqmean - mean * mean, 0)
    return float(np.mean(np.sqrt(local_var)[mask]))


def run_app(py, build_py, framework, test_case, extra_args, label,
            use_reblur=True, clear_screenshots=False):
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
                            capture_output=True, text=True, timeout=300)
    if result.returncode != 0:
        print(f"  FAIL: {label} exited with code {result.returncode}")
        if result.stderr:
            for line in result.stderr.strip().splitlines()[-5:]:
                print(f"    {line}")
        return False
    return True


def rename_screenshots(sdir, prefix):
    """Rename converged_history_before/after -> {prefix}_before/after."""
    for suffix in ("before", "after"):
        path = find_screenshot(sdir, f"*converged_history_{suffix}*")
        if path:
            new_path = os.path.join(sdir, f"nudge_{prefix}_{suffix}.png")
            os.rename(path, new_path)


def measure_floor(path, floor_mask, label):
    """Measure floor noise metrics for a screenshot."""
    _, luma = load_luminance(path)
    lap = floor_laplacian_var(luma, floor_mask)
    lstd = floor_local_std_mean(luma, floor_mask)
    fluma = float(np.mean(luma[floor_mask]))
    print(f"    {label:40s}: luma={fluma:.4f} lap_var={lap:.6f} local_std={lstd:.5f}")
    return {"lap_var": lap, "local_std": lstd, "luma": fluma, "path": path}


def measure_floor_masked(path, floor_mask, history_mask, label):
    """Measure floor noise only in reprojected (history-valid) pixels."""
    combined = floor_mask & history_mask
    n = int(np.sum(combined))
    if n < 100:
        print(f"    {label}: only {n} pixels in floor+history mask, skipping")
        return None
    _, luma = load_luminance(path)
    lap = floor_laplacian_var(luma, combined)
    lstd = floor_local_std_mean(luma, combined)
    fluma = float(np.mean(luma[combined]))
    print(f"    {label:40s}: luma={fluma:.4f} lap_var={lap:.6f} local_std={lstd:.5f} ({n} px)")
    return {"lap_var": lap, "local_std": lstd, "luma": fluma, "n_pixels": n}


def main():
    args, extra_args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    sdir = get_screenshot_dir(fw)

    print("=" * 70)
    print("  Post-Nudge Floor Noise Diagnostic")
    print("=" * 70)

    if not args.skip_build:
        print("\nBuilding...")
        result = subprocess.run([py, build_py, "--framework", fw] + extra_args,
                                cwd=PROJECT_ROOT, capture_output=True, text=True,
                                timeout=600)
        if result.returncode != 0:
            print("FAIL: build failed")
            if result.stderr:
                for line in result.stderr.strip().splitlines()[-10:]:
                    print(f"  {line}")
            return 1

    # ====================================================================
    # Step 1: Capture vanilla reference (for floor mask)
    # ====================================================================
    print(f"\n{'=' * 70}")
    print("  Step 1: Vanilla reference for floor mask")
    print(f"{'=' * 70}")
    ok = run_app(py, build_py, fw, "screenshot",
                 ["--max_spp", "2048"] + extra_args,
                 "vanilla ref", use_reblur=False, clear_screenshots=True)
    if not ok:
        print("FAIL: vanilla reference failed")
        return 1
    ref_path = find_screenshot(sdir, "*screenshot*")
    if not ref_path:
        print("FAIL: vanilla reference screenshot not found")
        return 1
    _, ref_luma = load_luminance(ref_path)
    floor_mask = get_floor_mask(ref_luma)
    n_floor = int(np.sum(floor_mask))
    print(f"  Floor mask: {n_floor} pixels ({n_floor/floor_mask.size*100:.1f}%)")

    # ====================================================================
    # Step 2: Capture per-stage debug passes (ALL with --reblur_no_pt_blend)
    # ====================================================================
    # CRITICAL: Must use --reblur_no_pt_blend for ALL passes. Without it,
    # the Composite shader applies PT blend (pt_weight = frame_index/256).
    # At convergence pt_weight=1.0 → output = vanilla PT result, NOT denoiser.
    # After nudge, pt_weight resets to ~0 → output = denoiser. This creates a
    # fake 17x "noise increase" that's really just the PT blend transition.
    stages = [
        ("ta_only", "TemporalAccum", ["--reblur_debug_pass", "TemporalAccum",
                                       "--reblur_no_pt_blend", "true"]),
        ("postblur", "PostBlur", ["--reblur_debug_pass", "PostBlur",
                                   "--reblur_no_pt_blend", "true"]),
        ("denoised", "Full (denoised-only)", ["--reblur_no_pt_blend", "true"]),
    ]

    stage_results = {}
    for tag, label, flags in stages:
        print(f"\n{'=' * 70}")
        print(f"  Step 2: {label}")
        print(f"{'=' * 70}")
        ok = run_app(py, build_py, fw, "reblur_converged_history",
                     flags + extra_args, tag, clear_screenshots=True)
        if not ok:
            print(f"  SKIP: {label} failed")
            continue
        rename_screenshots(sdir, tag)

        bp = os.path.join(sdir, f"nudge_{tag}_before.png")
        ap = os.path.join(sdir, f"nudge_{tag}_after.png")
        if not os.path.exists(bp) or not os.path.exists(ap):
            print(f"  SKIP: {tag} screenshots not found")
            continue

        print(f"\n  Floor noise ({label}):")
        s_before = measure_floor(bp, floor_mask, f"{label} BEFORE")
        s_after = measure_floor(ap, floor_mask, f"{label} AFTER")
        if s_before and s_after:
            ratio = s_after["lap_var"] / max(s_before["lap_var"], 1e-10)
            print(f"    Noise increase (after/before): {ratio:.2f}x")
        stage_results[tag] = (s_before, s_after)

    # ====================================================================
    # Step 3: TADisocclusion mask
    # ====================================================================
    print(f"\n{'=' * 70}")
    print("  Step 3: TADisocclusion mask (pixel classification)")
    print(f"{'=' * 70}")
    ok = run_app(py, build_py, fw, "reblur_converged_history",
                 ["--reblur_debug_pass", "TADisocclusion"] + extra_args,
                 "disocclusion", clear_screenshots=True)
    if not ok:
        print("  SKIP: TADisocclusion failed")
        history_mask = np.ones_like(floor_mask)
    else:
        rename_screenshots(sdir, "disocclusion")
        mask_after_path = os.path.join(sdir, "nudge_disocclusion_after.png")
        if os.path.exists(mask_after_path):
            mask_img = load_image(mask_after_path)
            r_ch, b_ch = mask_img[:, :, 0], mask_img[:, :, 2]
            in_screen = b_ch > 0.5
            history_mask = (r_ch < 0.5) & in_screen
            disoccluded_mask = (r_ch > 0.5) & in_screen

            floor_hist = floor_mask & history_mask
            floor_disoccl = floor_mask & disoccluded_mask
            print(f"\n  Floor pixel classification:")
            print(f"    Floor total:       {n_floor}")
            print(f"    Floor + history:   {int(np.sum(floor_hist))} "
                  f"({np.sum(floor_hist)/max(n_floor,1)*100:.1f}%)")
            print(f"    Floor + disoccl:   {int(np.sum(floor_disoccl))} "
                  f"({np.sum(floor_disoccl)/max(n_floor,1)*100:.1f}%)")
        else:
            history_mask = np.ones_like(floor_mask)

    # ====================================================================
    # Step 4: Floor noise for reprojected pixels only (using saved screenshots)
    # ====================================================================
    print(f"\n{'=' * 70}")
    print("  Step 4: Floor noise for REPROJECTED floor pixels only")
    print(f"{'=' * 70}")

    for tag, label, _ in stages:
        if tag not in stage_results:
            continue
        bp = os.path.join(sdir, f"nudge_{tag}_before.png")
        ap = os.path.join(sdir, f"nudge_{tag}_after.png")
        if not os.path.exists(bp) or not os.path.exists(ap):
            continue
        print(f"\n  {label}:")
        sm_before = measure_floor_masked(bp, floor_mask, history_mask,
                                          f"{label} BEFORE (floor+history)")
        sm_after = measure_floor_masked(ap, floor_mask, history_mask,
                                         f"{label} AFTER (floor+history)")
        if sm_before and sm_after:
            ratio = sm_after["lap_var"] / max(sm_before["lap_var"], 1e-10)
            print(f"    Noise increase (reprojected floor only): {ratio:.2f}x")

    # ====================================================================
    # Summary table
    # ====================================================================
    print(f"\n{'=' * 70}")
    print("  SUMMARY: Floor noise before vs after nudge (all with no_pt_blend)")
    print(f"{'=' * 70}")
    print(f"  {'Stage':25s} {'Before lap_var':>15s} {'After lap_var':>15s} {'Ratio':>8s}")
    print(f"  {'-'*25} {'-'*15} {'-'*15} {'-'*8}")

    for tag, label, _ in stages:
        if tag in stage_results:
            sb, sa = stage_results[tag]
            if sb and sa:
                ratio = sa["lap_var"] / max(sb["lap_var"], 1e-10)
                print(f"  {label:25s} {sb['lap_var']:15.6f} "
                      f"{sa['lap_var']:15.6f} {ratio:8.2f}x")

    print(f"\n  All diagnostic screenshots saved to: {sdir}/nudge_*.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
