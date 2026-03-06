"""Compare PostBlur vs Full pipeline output after camera nudge.

Captures PostBlur-only and Full pipeline screenshots for before/after nudge,
then compares them pixel-by-pixel to understand what the TS is doing.

Usage:
  python tests/reblur/diagnose_ts_comparison.py --framework glfw [--skip_build]
"""

import argparse
import glob
import os
import subprocess
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import convolve

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, PROJECT_ROOT)


def parse_args():
    parser = argparse.ArgumentParser(description="TS comparison diagnostic")
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


def load_image(path):
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def load_luminance(path):
    img = load_image(path)
    luma = img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722
    return img, luma


def get_floor_mask(luma):
    h, w = luma.shape
    mask = np.zeros_like(luma, dtype=bool)
    mask[int(h * 0.6):, :] = True
    mask &= (luma > 0.05) & (luma < 0.85)
    return mask


def lap_var(luma, mask):
    kernel = np.array([[0, 1, 0], [1, -4, 1], [0, 1, 0]], dtype=np.float32)
    lap = convolve(luma, kernel, mode="reflect")
    return float(np.var(lap[mask]))


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


def save_screenshots(sdir, outdir, prefix):
    """Copy converged_history screenshots to a safe output directory."""
    os.makedirs(outdir, exist_ok=True)
    for suffix in ("before", "after"):
        path = find_screenshot(sdir, f"*converged_history_{suffix}*")
        if path:
            import shutil
            new_path = os.path.join(outdir, f"ts_cmp_{prefix}_{suffix}.png")
            shutil.copy2(path, new_path)


def main():
    args, extra_args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    sdir = get_screenshot_dir(fw)

    print("=" * 70)
    print("  PostBlur vs Full Pipeline Comparison (after nudge)")
    print("=" * 70)

    if not args.skip_build:
        print("\nBuilding...")
        result = subprocess.run([py, build_py, "--framework", fw] + extra_args,
                                cwd=PROJECT_ROOT, capture_output=True, text=True,
                                timeout=600)
        if result.returncode != 0:
            print("FAIL: build failed")
            return 1

    # Step 1: Vanilla reference for floor mask (256 spp is enough)
    print(f"\n{'=' * 70}")
    print("  Step 1: Vanilla reference for floor mask")
    print(f"{'=' * 70}")
    ok = run_app(py, build_py, fw, "screenshot",
                 ["--max_spp", "256"] + extra_args,
                 "vanilla ref", use_reblur=False, clear_screenshots=True)
    if not ok:
        return 1
    ref_path = find_screenshot(sdir, "*screenshot*")
    if not ref_path:
        print("FAIL: vanilla screenshot not found")
        return 1
    _, ref_luma = load_luminance(ref_path)
    floor_mask = get_floor_mask(ref_luma)
    n_floor = int(np.sum(floor_mask))
    print(f"  Floor mask: {n_floor} pixels ({n_floor/floor_mask.size*100:.1f}%)")

    # Output directory for preserved screenshots
    outdir = os.path.join(sdir, "ts_comparison")
    os.makedirs(outdir, exist_ok=True)

    # Step 2: Capture PostBlur (before/after nudge)
    print(f"\n{'=' * 70}")
    print("  Step 2: PostBlur (no TS)")
    print(f"{'=' * 70}")
    ok = run_app(py, build_py, fw, "reblur_converged_history",
                 ["--reblur_debug_pass", "PostBlur",
                  "--reblur_no_pt_blend", "true"] + extra_args,
                 "postblur", clear_screenshots=True)
    if not ok:
        return 1
    save_screenshots(sdir, outdir, "postblur")

    # Step 3: Capture Full pipeline (before/after nudge)
    print(f"\n{'=' * 70}")
    print("  Step 3: Full pipeline (with TS)")
    print(f"{'=' * 70}")
    ok = run_app(py, build_py, fw, "reblur_converged_history",
                 ["--reblur_no_pt_blend", "true"] + extra_args,
                 "full", clear_screenshots=True)
    if not ok:
        return 1
    save_screenshots(sdir, outdir, "full")

    # Step 4: Capture TA-only (before/after nudge) for reference
    print(f"\n{'=' * 70}")
    print("  Step 4: TA only (for reference)")
    print(f"{'=' * 70}")
    ok = run_app(py, build_py, fw, "reblur_converged_history",
                 ["--reblur_debug_pass", "TemporalAccum",
                  "--reblur_no_pt_blend", "true"] + extra_args,
                 "ta", clear_screenshots=True)
    if not ok:
        print("  (TA-only failed, continuing)")
    else:
        save_screenshots(sdir, outdir, "ta")

    # Step 5: Analyze all captures
    print(f"\n{'=' * 70}")
    print("  ANALYSIS")
    print(f"{'=' * 70}")

    results = {}
    for tag in ["postblur", "full", "ta"]:
        for suffix in ["before", "after"]:
            path = os.path.join(outdir, f"ts_cmp_{tag}_{suffix}.png")
            if not os.path.exists(path):
                continue
            _, luma = load_luminance(path)
            lv = lap_var(luma, floor_mask)
            fl = float(np.mean(luma[floor_mask]))
            results[f"{tag}_{suffix}"] = {"lap_var": lv, "luma": fl, "luma_arr": luma}
            print(f"  {tag:10s} {suffix:6s}: luma={fl:.4f}  lap_var={lv:.6f}")

    # Noise ratios
    print(f"\n  Noise increase (after/before):")
    for tag in ["ta", "postblur", "full"]:
        if f"{tag}_before" in results and f"{tag}_after" in results:
            r = results[f"{tag}_after"]["lap_var"] / max(results[f"{tag}_before"]["lap_var"], 1e-10)
            print(f"    {tag:10s}: {r:.2f}x")

    # TS noise reduction
    print(f"\n  TS noise reduction (postblur / full):")
    for suffix in ["before", "after"]:
        pk = f"postblur_{suffix}"
        fk = f"full_{suffix}"
        if pk in results and fk in results:
            r = results[pk]["lap_var"] / max(results[fk]["lap_var"], 1e-10)
            print(f"    {suffix:6s}: {r:.2f}x")

    # Pixel-by-pixel comparison: PostBlur-AFTER vs Full-AFTER
    print(f"\n  Pixel-by-pixel comparison (floor region):")
    pk = "postblur_after"
    fk = "full_after"
    if pk in results and fk in results:
        postblur_luma = results[pk]["luma_arr"]
        full_luma = results[fk]["luma_arr"]

        diff = full_luma - postblur_luma
        floor_diff = diff[floor_mask]

        print(f"    Full - PostBlur (AFTER):")
        print(f"      mean={np.mean(floor_diff):.6f}  std={np.std(floor_diff):.6f}")
        print(f"      min={np.min(floor_diff):.6f}  max={np.max(floor_diff):.6f}")
        print(f"      abs_mean={np.mean(np.abs(floor_diff)):.6f}")

        # Correlation between PostBlur and Full
        correlation = np.corrcoef(postblur_luma[floor_mask], full_luma[floor_mask])[0, 1]
        print(f"      correlation: {correlation:.6f}")

        # What fraction of PostBlur noise does Full contain?
        # If Full = a*PostBlur + (1-a)*history, then:
        # We can estimate 'a' from the noise patterns
        pb_centered = postblur_luma[floor_mask] - np.mean(postblur_luma[floor_mask])
        full_centered = full_luma[floor_mask] - np.mean(full_luma[floor_mask])
        if np.var(pb_centered) > 1e-12:
            regression_slope = np.sum(pb_centered * full_centered) / np.sum(pb_centered * pb_centered)
            print(f"      regression slope (Full noise / PostBlur noise): {regression_slope:.4f}")
            print(f"        -> TS is passing through {regression_slope*100:.1f}% of PostBlur noise")

    # Also compare BEFORE screenshots
    pk = "postblur_before"
    fk = "full_before"
    if pk in results and fk in results:
        postblur_luma = results[pk]["luma_arr"]
        full_luma = results[fk]["luma_arr"]

        diff = full_luma - postblur_luma
        floor_diff = diff[floor_mask]

        print(f"\n    Full - PostBlur (BEFORE):")
        print(f"      mean={np.mean(floor_diff):.6f}  std={np.std(floor_diff):.6f}")
        print(f"      abs_mean={np.mean(np.abs(floor_diff)):.6f}")

        correlation = np.corrcoef(postblur_luma[floor_mask], full_luma[floor_mask])[0, 1]
        print(f"      correlation: {correlation:.6f}")

        pb_centered = postblur_luma[floor_mask] - np.mean(postblur_luma[floor_mask])
        full_centered = full_luma[floor_mask] - np.mean(full_luma[floor_mask])
        if np.var(pb_centered) > 1e-12:
            regression_slope = np.sum(pb_centered * full_centered) / np.sum(pb_centered * pb_centered)
            print(f"      regression slope (Full noise / PostBlur noise): {regression_slope:.4f}")
            print(f"        -> TS is passing through {regression_slope*100:.1f}% of PostBlur noise")

    print(f"\n  All screenshots saved to: {outdir}/ts_cmp_*.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
