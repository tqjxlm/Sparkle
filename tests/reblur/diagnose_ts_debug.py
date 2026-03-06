"""TS (Temporal Stabilization) debug diagnostic for floor noise after camera nudge.

Captures the TSStabCount debug pass before and after a 2-degree camera nudge
to visualize stab_count, blend weight, and antilag values at each floor pixel.

TSStabCount output channels:
  Diffuse (shown via diagnostic composite passthrough):
    R = stab_count / max_stabilized_frame_num (normalized stab_count)
    G = diff_blend (actual diffuse blend weight)
    B = min(diff_antilag, spec_antilag) (min antilag factor)
  Specular (stored but NOT shown by diagnostic passthrough):
    R = spec_antilag
    G = spec_accum_incoming / 63 (normalized)
    B = prev_stab_count / max_stabilized_frame_num

Usage:
  python tests/reblur/diagnose_ts_debug.py --framework glfw [--skip_build]
"""

import argparse
import glob
import os
import subprocess
import sys

import numpy as np
from PIL import Image

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, PROJECT_ROOT)


def parse_args():
    parser = argparse.ArgumentParser(description="TS debug diagnostic")
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
            for line in result.stderr.strip().splitlines()[-10:]:
                print(f"    {line}")
        return False
    return True


def rename_screenshots(sdir, prefix):
    for suffix in ("before", "after"):
        path = find_screenshot(sdir, f"*converged_history_{suffix}*")
        if path:
            new_path = os.path.join(sdir, f"ts_debug_{prefix}_{suffix}.png")
            if os.path.exists(new_path):
                os.remove(new_path)
            os.rename(path, new_path)


def analyze_ts_debug(path, floor_mask, label):
    """Analyze TSStabCount diagnostic screenshot.

    Diagnostic passthrough outputs denoisedDiffuse directly:
      R = stab_count / max_stabilized_frame_num
      G = diff_blend
      B = min(diff_antilag, spec_antilag)
    """
    img = load_image(path)
    r, g, b = img[:, :, 0], img[:, :, 1], img[:, :, 2]

    # Extract values for floor pixels
    stab_norm = r[floor_mask]   # stab_count / max_stab
    blend = g[floor_mask]       # diff_blend weight
    antilag = b[floor_mask]     # min(diff_antilag, spec_antilag)

    n = int(np.sum(floor_mask))
    print(f"\n  {label} ({n} floor pixels):")
    print(f"    stab_count/max  : mean={np.mean(stab_norm):.4f}  "
          f"min={np.min(stab_norm):.4f}  max={np.max(stab_norm):.4f}  "
          f"median={np.median(stab_norm):.4f}")
    print(f"    diff_blend      : mean={np.mean(blend):.4f}  "
          f"min={np.min(blend):.4f}  max={np.max(blend):.4f}  "
          f"median={np.median(blend):.4f}")
    print(f"    min_antilag     : mean={np.mean(antilag):.4f}  "
          f"min={np.min(antilag):.4f}  max={np.max(antilag):.4f}  "
          f"median={np.median(antilag):.4f}")

    # Distribution of stab_count
    low_stab = np.sum(stab_norm < 0.1) / n * 100
    mid_stab = np.sum((stab_norm >= 0.1) & (stab_norm < 0.5)) / n * 100
    high_stab = np.sum(stab_norm >= 0.5) / n * 100
    print(f"    stab distribution: <10%={low_stab:.1f}%  10-50%={mid_stab:.1f}%  >50%={high_stab:.1f}%")

    # Distribution of blend weight
    low_blend = np.sum(blend < 0.5) / n * 100
    mid_blend = np.sum((blend >= 0.5) & (blend < 0.9)) / n * 100
    high_blend = np.sum(blend >= 0.9) / n * 100
    print(f"    blend distribution: <0.5={low_blend:.1f}%  0.5-0.9={mid_blend:.1f}%  >0.9={high_blend:.1f}%")

    # Distribution of antilag
    low_antilag = np.sum(antilag < 0.5) / n * 100
    mid_antilag = np.sum((antilag >= 0.5) & (antilag < 0.9)) / n * 100
    high_antilag = np.sum(antilag >= 0.9) / n * 100
    print(f"    antilag distribution: <0.5={low_antilag:.1f}%  0.5-0.9={mid_antilag:.1f}%  >0.9={high_antilag:.1f}%")

    return {
        "stab_norm": {"mean": float(np.mean(stab_norm)), "min": float(np.min(stab_norm)),
                       "max": float(np.max(stab_norm)), "median": float(np.median(stab_norm))},
        "blend": {"mean": float(np.mean(blend)), "min": float(np.min(blend)),
                   "max": float(np.max(blend)), "median": float(np.median(blend))},
        "antilag": {"mean": float(np.mean(antilag)), "min": float(np.min(antilag)),
                     "max": float(np.max(antilag)), "median": float(np.median(antilag))},
    }


def main():
    args, extra_args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    sdir = get_screenshot_dir(fw)

    print("=" * 70)
    print("  TS Debug Diagnostic: stab_count, blend, antilag after nudge")
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

    # Step 1: Vanilla reference for floor mask
    print(f"\n{'=' * 70}")
    print("  Step 1: Vanilla reference for floor mask")
    print(f"{'=' * 70}")
    ok = run_app(py, build_py, fw, "screenshot",
                 ["--max_spp", "256"] + extra_args,
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

    # Step 2: TSStabCount debug pass (converged_history test)
    print(f"\n{'=' * 70}")
    print("  Step 2: TSStabCount debug (before/after nudge)")
    print(f"{'=' * 70}")
    ok = run_app(py, build_py, fw, "reblur_converged_history",
                 ["--reblur_debug_pass", "TSStabCount",
                  "--reblur_no_pt_blend", "true"] + extra_args,
                 "ts_stab", clear_screenshots=True)
    if not ok:
        print("FAIL: TSStabCount run failed")
        return 1

    rename_screenshots(sdir, "stab")

    before_path = os.path.join(sdir, "ts_debug_stab_before.png")
    after_path = os.path.join(sdir, "ts_debug_stab_after.png")

    if not os.path.exists(before_path) or not os.path.exists(after_path):
        print("FAIL: TSStabCount screenshots not found")
        for p in [before_path, after_path]:
            print(f"  {p}: {'EXISTS' if os.path.exists(p) else 'MISSING'}")
        return 1

    before_stats = analyze_ts_debug(before_path, floor_mask, "BEFORE nudge (converged)")
    after_stats = analyze_ts_debug(after_path, floor_mask, "AFTER nudge (5 frames)")

    # Step 3: Compare
    print(f"\n{'=' * 70}")
    print("  Step 3: Comparison")
    print(f"{'=' * 70}")
    for metric in ["stab_norm", "blend", "antilag"]:
        b = before_stats[metric]["mean"]
        a = after_stats[metric]["mean"]
        print(f"  {metric:15s}: before={b:.4f}  after={a:.4f}  change={a-b:+.4f}")

    # Step 4: Also run the full pipeline (non-debug) to compare noise
    print(f"\n{'=' * 70}")
    print("  Step 4: Full pipeline noise (for reference)")
    print(f"{'=' * 70}")
    ok = run_app(py, build_py, fw, "reblur_converged_history",
                 ["--reblur_no_pt_blend", "true"] + extra_args,
                 "full", clear_screenshots=True)
    if ok:
        rename_screenshots(sdir, "full")
        full_before = os.path.join(sdir, "ts_debug_full_before.png")
        full_after = os.path.join(sdir, "ts_debug_full_after.png")
        if os.path.exists(full_before) and os.path.exists(full_after):
            from scipy.ndimage import convolve
            kernel = np.array([[0, 1, 0], [1, -4, 1], [0, 1, 0]], dtype=np.float32)

            _, luma_b = load_luminance(full_before)
            _, luma_a = load_luminance(full_after)
            lap_b = float(np.var(convolve(luma_b, kernel, mode="reflect")[floor_mask]))
            lap_a = float(np.var(convolve(luma_a, kernel, mode="reflect")[floor_mask]))
            ratio = lap_a / max(lap_b, 1e-10)
            print(f"  Full BEFORE lap_var: {lap_b:.6f}")
            print(f"  Full AFTER  lap_var: {lap_a:.6f}")
            print(f"  Noise ratio:         {ratio:.2f}x")

    print(f"\n  All screenshots saved to: {sdir}/ts_debug_*.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
