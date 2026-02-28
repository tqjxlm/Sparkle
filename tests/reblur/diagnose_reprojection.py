"""Diagnostic for ReBLUR reprojection after camera motion.

Runs the reblur_converged_history test case with temporal accumulation
diagnostic debug passes to understand why reprojection fails:
  - debug_pass 10: Disocclusion map (R=disoccluded, G=footprintQuality, B=inScreen)
  - debug_pass 11: Motion vector visualization (RG=displacement, B=magnitude)
  - debug_pass 12: Depth test diagnostic (R=depth_ratio, G=normal_agreement, B=prev_accum)

Usage:
  python3 tests/reblur/diagnose_reprojection.py --framework macos [--skip_build]
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
    parser = argparse.ArgumentParser(description="ReBLUR reprojection diagnostic")
    parser.add_argument("--framework", default="glfw", choices=("glfw", "macos"))
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


def run_test(py, build_py, framework, debug_pass, label, clear=False):
    cmd = [py, build_py, "--framework", framework, "--skip_build",
           "--run", "--test_case", "reblur_converged_history", "--headless", "true",
           "--pipeline", "gpu", "--spp", "1", "--use_reblur", "true",
           "--reblur_debug_pass", str(debug_pass)]
    if clear:
        cmd += ["--clear_screenshots", "true"]
    print(f"  cmd: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=PROJECT_ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  FAIL: app exited with code {result.returncode}")
        if result.stderr:
            for line in result.stderr.strip().splitlines()[-5:]:
                print(f"    {line}")
        return False
    return True


def analyze_disocclusion_map(screenshot_dir):
    """Analyze debug_pass 10 output: R=disoccluded, G=footprintQuality, B=inScreen."""
    after_path = find_screenshot(screenshot_dir, "*converged_history_after*")
    if not after_path:
        print("  No 'after' screenshot found")
        return

    img = np.array(Image.open(after_path).convert("RGB"), dtype=np.float32) / 255.0
    r, g, b = img[:, :, 0], img[:, :, 1], img[:, :, 2]

    # R channel: disoccluded (1 = yes)
    disoccluded_mask = r > 0.5
    pct_disoccluded = np.mean(disoccluded_mask) * 100

    # G channel: footprintQuality (0-1)
    # Only meaningful where not disoccluded
    valid_mask = ~disoccluded_mask
    mean_fq = np.mean(g[valid_mask]) if np.any(valid_mask) else 0.0

    # B channel: inScreen (1 = yes)
    pct_in_screen = np.mean(b > 0.5) * 100

    print(f"\n  === Disocclusion Map Analysis ===")
    print(f"  Pixels disoccluded: {pct_disoccluded:.1f}%")
    print(f"  Pixels in screen:   {pct_in_screen:.1f}%")
    print(f"  Out of screen:      {100 - pct_in_screen:.1f}%")
    print(f"  Mean footprint quality (valid pixels): {mean_fq:.3f}")

    # Breakdown of disocclusion reasons
    out_of_screen = (b < 0.5)
    in_screen_but_disoccluded = disoccluded_mask & ~out_of_screen
    pct_oos = np.mean(out_of_screen) * 100
    pct_isd = np.mean(in_screen_but_disoccluded) * 100
    print(f"  Disoccluded because out-of-screen: {pct_oos:.1f}%")
    print(f"  Disoccluded because depth/normal fail: {pct_isd:.1f}%")
    print(f"  Valid reprojection: {100 - pct_disoccluded:.1f}%")


def analyze_motion_vectors(screenshot_dir):
    """Analyze debug_pass 11 output: RG=displacement*0.01+0.5, B=magnitude*0.01."""
    after_path = find_screenshot(screenshot_dir, "*converged_history_after*")
    if not after_path:
        print("  No 'after' screenshot found")
        return

    img = np.array(Image.open(after_path).convert("RGB"), dtype=np.float32) / 255.0
    r, g, b = img[:, :, 0], img[:, :, 1], img[:, :, 2]

    # Decode: mv_pixels = (channel - 0.5) / 0.01
    mv_x = (r - 0.5) / 0.01
    mv_y = (g - 0.5) / 0.01
    mv_mag = b / 0.01

    print(f"\n  === Motion Vector Analysis ===")
    print(f"  MV X (pixels): mean={np.mean(mv_x):.1f}, std={np.std(mv_x):.1f}, "
          f"min={np.min(mv_x):.1f}, max={np.max(mv_x):.1f}")
    print(f"  MV Y (pixels): mean={np.mean(mv_y):.1f}, std={np.std(mv_y):.1f}, "
          f"min={np.min(mv_y):.1f}, max={np.max(mv_y):.1f}")
    print(f"  MV magnitude:  mean={np.mean(mv_mag):.1f}, std={np.std(mv_mag):.1f}, "
          f"min={np.min(mv_mag):.1f}, max={np.max(mv_mag):.1f}")

    pct_zero = np.mean(mv_mag < 0.5) * 100
    pct_nonzero = np.mean(mv_mag >= 0.5) * 100
    print(f"  Pixels with ~zero MV (< 0.5 px): {pct_zero:.1f}%")
    print(f"  Pixels with non-zero MV:          {pct_nonzero:.1f}%")


def analyze_depth_diagnostic(screenshot_dir):
    """Analyze debug_pass 12 output: R=depth_ratio, G=normal_agreement, B=prev_accum/63."""
    after_path = find_screenshot(screenshot_dir, "*converged_history_after*")
    if not after_path:
        print("  No 'after' screenshot found")
        return

    img = np.array(Image.open(after_path).convert("RGB"), dtype=np.float32) / 255.0
    r, g, b = img[:, :, 0], img[:, :, 1], img[:, :, 2]

    # R channel: depth_ratio = |cornerZ - centerZ| / (threshold * max(|centerZ|, 1))
    # Values > 1.0 mean depth test fails. saturate() clamps to [0,1], so 1.0 = fail
    pct_depth_fail = np.mean(r > 0.99) * 100
    mean_depth_ratio = np.mean(r)

    # G channel: normal agreement = dot(prev_normal, curr_normal) * 0.5 + 0.5
    # < 0.5 means dot < 0, definitely different surface
    # < 0.75 means dot < 0.5, the threshold used in BilinearHistorySample
    pct_normal_fail = np.mean(g < 0.75) * 100
    mean_normal_agree = np.mean(g)

    # B channel: prev_accum_speed / 63
    mean_prev_accum = np.mean(b) * 63

    print(f"\n  === Depth/Normal Diagnostic ===")
    print(f"  Depth ratio (0=perfect, 1=fail):   mean={mean_depth_ratio:.3f}")
    print(f"  Pixels with depth ratio >= 1.0:     {pct_depth_fail:.1f}%")
    print(f"  Normal agreement (0.5=orthogonal):  mean={mean_normal_agree:.3f}")
    print(f"  Pixels with normal dot < 0.5:       {pct_normal_fail:.1f}%")
    print(f"  Mean previous accum speed:          {mean_prev_accum:.1f}")


def main():
    args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)

    print("=" * 60)
    print("  ReBLUR Reprojection Diagnostic")
    print("=" * 60)

    if not args.skip_build:
        print("\nBuilding...")
        result = subprocess.run([py, build_py, "--framework", fw],
                                cwd=PROJECT_ROOT, capture_output=True, text=True)
        if result.returncode != 0:
            print("FAIL: build failed")
            return 1

    # --- Diagnostic 1: Disocclusion map ---
    print(f"\n{'—'*60}")
    print("  Diagnostic 1: Disocclusion map (debug_pass 10)")
    print(f"{'—'*60}")
    if run_test(py, build_py, fw, 10, "disocclusion", clear=True):
        analyze_disocclusion_map(screenshot_dir)
        # Rename to avoid overwrite
        for p in glob.glob(os.path.join(screenshot_dir, "*converged_history_*")):
            new = p.replace("converged_history", "diag_disocclusion")
            os.rename(p, new)

    # --- Diagnostic 2: Motion vectors ---
    print(f"\n{'—'*60}")
    print("  Diagnostic 2: Motion vectors (debug_pass 11)")
    print(f"{'—'*60}")
    if run_test(py, build_py, fw, 11, "motion_vectors"):
        analyze_motion_vectors(screenshot_dir)
        for p in glob.glob(os.path.join(screenshot_dir, "*converged_history_*")):
            new = p.replace("converged_history", "diag_motion_vec")
            os.rename(p, new)

    # --- Diagnostic 3: Depth/normal test ---
    print(f"\n{'—'*60}")
    print("  Diagnostic 3: Depth/normal test (debug_pass 12)")
    print(f"{'—'*60}")
    if run_test(py, build_py, fw, 12, "depth_normal"):
        analyze_depth_diagnostic(screenshot_dir)
        for p in glob.glob(os.path.join(screenshot_dir, "*converged_history_*")):
            new = p.replace("converged_history", "diag_depth_normal")
            os.rename(p, new)

    print(f"\n{'='*60}")
    print("  Diagnostic complete. Check screenshots in:")
    print(f"  {screenshot_dir}")
    print(f"{'='*60}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
