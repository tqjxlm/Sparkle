"""Semantic converged-history camera-nudge validation.

Tests that after convergence + small camera yaw, ONLY disoccluded/new pixels
have noise.  History-retrieved pixels must be clean — indistinguishable from
the fully converged frame.

Uses the TADisocclusion mask to classify every pixel as:
  - history (valid reprojection)  -> MUST be clean
  - disoccluded (newly revealed)  -> expected to be noisy
  - sky (background)              -> ignored

Three runs:
  0. Vanilla baseline (no reblur): converge -> nudge -> re-converge.
     Provides noise-free ground-truth for the nudged viewpoint.
  1. Reblur denoised-only (--reblur_no_pt_blend): converge -> nudge -> settle.
     Isolates the denoiser from the PT blend ramp.
  2. TADisocclusion mask: same sequence as Run 1.
     Provides per-pixel history/disoccluded classification.

Semantic checks:
  A. History cleanness: local noise in history pixels of reblur_after must
     be comparable to the vanilla reference (ratio < 1.5).
  B. Noise concentration: disoccluded pixels must be measurably noisier than
     history pixels (ratio > 2.0).
  C. Reprojection validity: > 60% of geometry pixels have valid history.
  D. No NaN/Inf/all-black in any output.

Saves diagnostic images to screenshot dir for visual inspection.

Usage:
  python tests/reblur/test_converged_history.py --framework macos [--skip_build]
"""

import argparse
import glob
import os
import subprocess
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import uniform_filter, gaussian_filter

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, PROJECT_ROOT)

# --- Thresholds ---
# History cleanness: reblur history noise / vanilla noise (same region).
# 1.0 = identical to converged vanilla, higher = residual noise.
HISTORY_NOISE_RATIO_MAX = 1.5
# Noise concentration: disoccluded noise / history noise.
# Higher means noise is more concentrated in disoccluded regions (good).
NOISE_CONCENTRATION_MIN = 2.0
# Reprojection validity: % of geometry pixels with valid history.
MIN_VALID_REPROJECTION_PCT = 60.0
# Footprint quality: mean quality for valid-history pixels.
MIN_FOOTPRINT_QUALITY = 0.5
# Luma preservation: reblur before vs after (should be within 7%).
LUMA_RATIO_MIN = 0.93
LUMA_RATIO_MAX = 1.07


def parse_args():
    parser = argparse.ArgumentParser(
        description="Semantic converged history camera delta test")
    parser.add_argument("--framework", default="macos",
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


def load_image(path):
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def load_luminance(path):
    img = load_image(path)
    luma = img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722
    return img, luma


def compute_local_std(arr, window=7):
    """Per-pixel local standard deviation in WxW patches."""
    mean = uniform_filter(arr, size=window, mode="reflect")
    mean_sq = uniform_filter(arr ** 2, size=window, mode="reflect")
    return np.sqrt(np.maximum(mean_sq - mean ** 2, 0))


def compute_hf_residual(luma, sigma=2.0):
    """High-frequency residual: |image - gaussian_blur|.

    Isolates noise + fine texture from low-frequency scene content.
    """
    return np.abs(luma - gaussian_filter(luma, sigma=sigma))


def check_valid(path):
    """Basic image validation: NaN, Inf, all-black."""
    img, luma = load_luminance(path)
    failures = []
    if np.any(np.isnan(img)):
        failures.append("NaN detected")
    if np.any(np.isinf(img)):
        failures.append("Inf detected")
    if np.mean(luma) < 1e-4:
        failures.append("all black")
    return failures


def create_masks(disocclusion_path):
    """Create history/disoccluded/sky masks from TADisocclusion screenshot.

    TADisocclusion format: R=disoccluded, G=footprintQuality, B=inScreen.
    """
    img = load_image(disocclusion_path)
    r_ch, g_ch, b_ch = img[:, :, 0], img[:, :, 1], img[:, :, 2]
    in_screen = b_ch > 0.5
    history = (r_ch < 0.5) & in_screen
    disoccluded = (r_ch > 0.5) & in_screen
    return {
        "history": history,
        "disoccluded": disoccluded,
        "in_screen": in_screen,
        "footprint_quality": g_ch,
    }


def run_app(py, build_py, framework, test_case, extra_args, label,
            use_reblur=True, clear_screenshots=False):
    """Run a C++ test case. Returns success bool."""
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
        print(f"  FAIL: {label} exited with code {result.returncode}")
        if result.stderr:
            for line in result.stderr.strip().splitlines()[-5:]:
                print(f"    {line}")
        return False
    return True


def rename_screenshots(screenshot_dir, old_pattern, new_prefix):
    """Rename screenshots to avoid overwrite by subsequent runs."""
    for path in glob.glob(os.path.join(screenshot_dir, old_pattern)):
        dirname = os.path.dirname(path)
        basename = os.path.basename(path)
        new_name = basename.replace(
            "converged_history_before", f"{new_prefix}_before").replace(
            "converged_history_after", f"{new_prefix}_after")
        new_path = os.path.join(dirname, new_name)
        os.rename(path, new_path)


def save_diagnostic_images(screenshot_dir, reblur_after,
                           masks, reblur_after_luma, vanilla_after_luma):
    """Save visual diagnostic images for manual inspection."""
    h, w = reblur_after_luma.shape
    disoccluded = masks["disoccluded"]

    # 1. Error heatmap: |reblur - vanilla| (blue=low, red=high, clamp 0.3)
    error = np.abs(reblur_after_luma - vanilla_after_luma)
    error_vis = np.clip(error / 0.3, 0, 1)
    err_rgb = np.zeros((h, w, 3), dtype=np.uint8)
    err_rgb[:, :, 0] = (error_vis * 255).astype(np.uint8)
    err_rgb[:, :, 2] = ((1 - error_vis) * 255).astype(np.uint8)
    Image.fromarray(err_rgb).save(
        os.path.join(screenshot_dir, "diag_error_vs_vanilla.png"))

    # 2. Excess noise map: (reblur HF residual - vanilla HF residual)
    reblur_hf = compute_hf_residual(reblur_after_luma)
    vanilla_hf = compute_hf_residual(vanilla_after_luma)
    excess = np.clip((reblur_hf - vanilla_hf) / 0.05, 0, 1)
    exc_rgb = np.zeros((h, w, 3), dtype=np.uint8)
    exc_rgb[:, :, 0] = (excess * 255).astype(np.uint8)
    exc_rgb[~masks["in_screen"]] = [0, 0, 30]
    Image.fromarray(exc_rgb).save(
        os.path.join(screenshot_dir, "diag_excess_noise.png"))

    # 3. Segmented overlay: reblur_after with disoccluded pixels in red
    overlay = (np.clip(reblur_after, 0, 1) * 255).astype(np.uint8).copy()
    overlay[disoccluded, 0] = 255
    overlay[disoccluded, 1] = 0
    overlay[disoccluded, 2] = 0
    Image.fromarray(overlay).save(
        os.path.join(screenshot_dir, "diag_segmented_overlay.png"))

    print(f"  Saved diagnostic images to {screenshot_dir}/diag_*.png")


def main():
    args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    sdir = get_screenshot_dir(fw)

    print("=" * 60)
    print("  Semantic Converged History Validation")
    print("=" * 60)

    # Build
    if not args.skip_build:
        print("\nBuilding...")
        result = subprocess.run([py, build_py, "--framework", fw],
                                cwd=PROJECT_ROOT, capture_output=True,
                                text=True)
        if result.returncode != 0:
            print("FAIL: build failed")
            if result.stderr:
                for line in result.stderr.strip().splitlines()[-10:]:
                    print(f"  {line}")
            return 1

    all_results = []

    # ====================================================================
    # Run 0: Vanilla baseline (converge -> nudge -> fully re-converge)
    # ====================================================================
    print(f"\n{'—' * 60}")
    print("  Run 0: Vanilla baseline (ground truth for nudged viewpoint)")
    print(f"{'—' * 60}")
    ok = run_app(py, build_py, fw, "vanilla_converged_baseline", [],
                 "vanilla", use_reblur=False, clear_screenshots=True)
    if not ok:
        all_results.append(("Run 0: vanilla baseline", False))
        _print_summary(all_results)
        return 1
    all_results.append(("Run 0: vanilla baseline", True))

    vanilla_after_path = find_screenshot(sdir, "*vanilla_baseline_after*")
    if not vanilla_after_path:
        print("  FAIL: vanilla_baseline_after screenshot not found")
        all_results.append(("Run 0: vanilla screenshot found", False))
        _print_summary(all_results)
        return 1

    # Validate vanilla output
    failures = check_valid(vanilla_after_path)
    if failures:
        for f in failures:
            print(f"  FAIL: vanilla_after — {f}")
            all_results.append((f"Run 0: vanilla_after {f}", False))

    # ====================================================================
    # Run 1: Reblur denoised-only (converge -> nudge -> 5 frames settle)
    # ====================================================================
    print(f"\n{'—' * 60}")
    print("  Run 1: Reblur denoised-only (isolates denoiser)")
    print(f"{'—' * 60}")
    ok = run_app(py, build_py, fw, "reblur_converged_history",
                 ["--reblur_no_pt_blend", "true"], "denoised-only")
    if not ok:
        all_results.append(("Run 1: reblur denoised-only", False))
        _print_summary(all_results)
        return 1
    all_results.append(("Run 1: reblur denoised-only", True))

    reblur_before_path = find_screenshot(sdir, "*converged_history_before*")
    reblur_after_path = find_screenshot(sdir, "*converged_history_after*")
    if not reblur_before_path or not reblur_after_path:
        print("  FAIL: reblur screenshots not found")
        all_results.append(("Run 1: screenshots found", False))
        _print_summary(all_results)
        return 1

    # Validate and rename
    for path, tag in [(reblur_before_path, "before"),
                      (reblur_after_path, "after")]:
        failures = check_valid(path)
        if failures:
            for f in failures:
                all_results.append((f"Run 1: {tag} {f}", False))
    rename_screenshots(sdir, "*converged_history_*", "reblur_denoised")
    reblur_before_path = find_screenshot(sdir, "*reblur_denoised_before*")
    reblur_after_path = find_screenshot(sdir, "*reblur_denoised_after*")

    # ====================================================================
    # Run 2: TADisocclusion mask (same sequence as Run 1)
    # ====================================================================
    print(f"\n{'—' * 60}")
    print("  Run 2: TADisocclusion mask (pixel classification)")
    print(f"{'—' * 60}")
    ok = run_app(py, build_py, fw, "reblur_converged_history",
                 ["--reblur_debug_pass", "TADisocclusion"], "disocclusion")
    if not ok:
        all_results.append(("Run 2: TADisocclusion", False))
        _print_summary(all_results)
        return 1
    all_results.append(("Run 2: TADisocclusion", True))

    mask_after_path = find_screenshot(sdir, "*converged_history_after*")
    if not mask_after_path:
        print("  FAIL: disocclusion mask not found")
        all_results.append(("Run 2: mask screenshot found", False))
        _print_summary(all_results)
        return 1
    rename_screenshots(sdir, "*converged_history_*", "disocclusion")
    mask_after_path = find_screenshot(sdir, "*disocclusion_after*")

    # ====================================================================
    # Semantic Analysis
    # ====================================================================
    print(f"\n{'=' * 60}")
    print("  Semantic Analysis: Region-Segmented Noise Check")
    print(f"{'=' * 60}")

    # Load images
    reblur_after_img, reblur_after_luma = load_luminance(reblur_after_path)
    _, reblur_before_luma = load_luminance(reblur_before_path)
    _, vanilla_after_luma = load_luminance(vanilla_after_path)

    # Create masks from TADisocclusion
    masks = create_masks(mask_after_path)
    history = masks["history"]
    disoccluded = masks["disoccluded"]
    in_screen = masks["in_screen"]
    fp_quality = masks["footprint_quality"]

    geo_count = int(np.sum(in_screen))
    hist_count = int(np.sum(history))
    disoccl_count = int(np.sum(disoccluded))
    valid_pct = hist_count / max(geo_count, 1) * 100

    print(f"\n  Pixel classification:")
    print(f"    Geometry:    {geo_count} ({geo_count / in_screen.size * 100:.1f}%)")
    print(f"    History:     {hist_count} ({valid_pct:.1f}% of geometry)")
    print(f"    Disoccluded: {disoccl_count} ({disoccl_count / max(geo_count, 1) * 100:.1f}% of geometry)")

    # --- Check C: Reprojection validity ---
    print(f"\n  Check C: Reprojection validity")
    if valid_pct >= MIN_VALID_REPROJECTION_PCT:
        print(f"    PASS: {valid_pct:.1f}% >= {MIN_VALID_REPROJECTION_PCT}%")
        all_results.append(("Reprojection validity", True))
    else:
        print(f"    FAIL: {valid_pct:.1f}% < {MIN_VALID_REPROJECTION_PCT}%")
        all_results.append(("Reprojection validity", False))

    mean_fp = float(np.mean(fp_quality[history])) if hist_count > 0 else 0
    if mean_fp >= MIN_FOOTPRINT_QUALITY:
        print(f"    PASS: footprint quality {mean_fp:.3f} >= {MIN_FOOTPRINT_QUALITY}")
        all_results.append(("Footprint quality", True))
    else:
        print(f"    FAIL: footprint quality {mean_fp:.3f} < {MIN_FOOTPRINT_QUALITY}")
        all_results.append(("Footprint quality", False))

    # --- Check A: History cleanness ---
    # Compare local noise (high-frequency residual) in history pixels of
    # reblur_after vs vanilla_after.  Vanilla is fully converged (noise-free),
    # so any excess in reblur is residual denoiser noise.
    print(f"\n  Check A: History pixel cleanness")
    reblur_hf = compute_hf_residual(reblur_after_luma)
    vanilla_hf = compute_hf_residual(vanilla_after_luma)

    reblur_hist_noise = float(np.mean(reblur_hf[history]))
    vanilla_hist_noise = float(np.mean(vanilla_hf[history]))
    history_noise_ratio = reblur_hist_noise / max(vanilla_hist_noise, 1e-9)

    print(f"    Reblur history HF residual:  {reblur_hist_noise:.6f}")
    print(f"    Vanilla history HF residual: {vanilla_hist_noise:.6f}")
    print(f"    Ratio (reblur/vanilla):      {history_noise_ratio:.2f}x")
    print(f"    (1.0 = identical to converged; higher = residual noise)")

    if history_noise_ratio <= HISTORY_NOISE_RATIO_MAX:
        print(f"    PASS: {history_noise_ratio:.2f}x <= {HISTORY_NOISE_RATIO_MAX}x")
        all_results.append(("History cleanness", True))
    else:
        print(f"    FAIL: {history_noise_ratio:.2f}x > {HISTORY_NOISE_RATIO_MAX}x")
        all_results.append(("History cleanness", False))

    # --- Check B: Noise concentration ---
    # Disoccluded pixels should be measurably noisier than history pixels.
    print(f"\n  Check B: Noise concentration (disoccluded vs history)")
    if disoccl_count > 0:
        reblur_disoccl_noise = float(np.mean(reblur_hf[disoccluded]))
        concentration_ratio = reblur_disoccl_noise / max(reblur_hist_noise, 1e-9)
        print(f"    Reblur disoccluded HF residual: {reblur_disoccl_noise:.6f}")
        print(f"    Concentration ratio (disoccl/hist): {concentration_ratio:.2f}x")

        if concentration_ratio >= NOISE_CONCENTRATION_MIN:
            print(f"    PASS: {concentration_ratio:.2f}x >= {NOISE_CONCENTRATION_MIN}x")
            all_results.append(("Noise concentration", True))
        else:
            print(f"    FAIL: {concentration_ratio:.2f}x < {NOISE_CONCENTRATION_MIN}x")
            all_results.append(("Noise concentration", False))
    else:
        print(f"    SKIP: no disoccluded pixels (0.0% of geometry)")
        all_results.append(("Noise concentration", True))

    # --- Luma preservation ---
    print(f"\n  Luma preservation (reblur before vs after)")
    mean_before = float(np.mean(reblur_before_luma[in_screen]))
    mean_after = float(np.mean(reblur_after_luma[in_screen]))
    luma_ratio = mean_after / max(mean_before, 1e-6)
    print(f"    Before: {mean_before:.4f}, After: {mean_after:.4f}")
    print(f"    Ratio: {luma_ratio:.4f}")
    if LUMA_RATIO_MIN <= luma_ratio <= LUMA_RATIO_MAX:
        print(f"    PASS: in [{LUMA_RATIO_MIN}, {LUMA_RATIO_MAX}]")
        all_results.append(("Luma preservation", True))
    else:
        print(f"    FAIL: outside [{LUMA_RATIO_MIN}, {LUMA_RATIO_MAX}]")
        all_results.append(("Luma preservation", False))

    # --- Informational: detailed noise breakdown ---
    print(f"\n  Informational: Noise breakdown by scene region")
    local_noise = compute_local_std(reblur_after_luma, window=7)
    vanilla_local = compute_local_std(vanilla_after_luma, window=7)
    print(f"    Reblur history local_std:     {np.mean(local_noise[history]):.6f}")
    print(f"    Vanilla history local_std:    {np.mean(vanilla_local[history]):.6f}")
    print(f"    Reblur converged local_std:   {np.mean(compute_local_std(reblur_before_luma, 7)[in_screen]):.6f}")
    if disoccl_count > 0:
        print(f"    Reblur disoccluded local_std: {np.mean(local_noise[disoccluded]):.6f}")

    # Noise distribution in history pixels
    hist_noise_vals = local_noise[history]
    print(f"    History noise percentiles:")
    for pct in [50, 90, 95, 99]:
        print(f"      P{pct}: {np.percentile(hist_noise_vals, pct):.6f}")

    # --- Save diagnostic images ---
    save_diagnostic_images(sdir, reblur_after_img,
                           masks, reblur_after_luma, vanilla_after_luma)

    # --- Summary ---
    _print_summary(all_results)
    failed = sum(1 for _, ok in all_results if not ok)
    return 0 if failed == 0 else 1


def _print_summary(results):
    passed = sum(1 for _, ok in results if ok)
    failed = sum(1 for _, ok in results if not ok)
    print(f"\n{'=' * 60}")
    print(f"  Results: {passed} passed, {failed} failed")
    for name, ok in results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    print(f"{'=' * 60}")


if __name__ == "__main__":
    sys.exit(main())
