"""Floor noise diagnostic for REBLUR denoiser.

Captures screenshots at multiple pipeline stages and SPP levels to identify
where floor noise originates.

Stages:
  - Vanilla (PT only, no denoiser)
  - Temporal Accumulation output (debug_pass=TemporalAccum)
  - PostBlur output (debug_pass=PostBlur)
  - Full denoiser (--reblur_no_pt_blend)
  - End-to-end (denoiser + PT blend)

SPP levels: 64, 256, 2048

Floor region: lower 40% of image, moderate luminance (0.05 < luma < 0.85)

Outputs:
  - Numeric noise statistics (laplacian variance, local std, luma)
  - Screenshots saved with descriptive names for visual inspection

Usage:
  python tests/reblur/diagnose_floor_noise.py --framework glfw [--skip_build]
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
    parser = argparse.ArgumentParser(description="Floor noise diagnostic")
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


def floor_local_std(luma, mask, window=5):
    mean = uniform_filter(luma, size=window, mode='reflect')
    sqmean = uniform_filter(luma * luma, size=window, mode='reflect')
    local_var = np.maximum(sqmean - mean * mean, 0)
    local_std_map = np.sqrt(local_var)
    vals = local_std_map[mask]
    return {
        "mean": float(np.mean(vals)),
        "median": float(np.median(vals)),
        "p95": float(np.percentile(vals, 95)),
        "p99": float(np.percentile(vals, 99)),
    }


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
                            capture_output=True, text=True, timeout=120)
    if result.returncode != 0:
        print(f"  FAIL: {label} exited with code {result.returncode}")
        if result.stderr:
            for line in result.stderr.strip().splitlines()[-5:]:
                print(f"    {line}")
        return False
    return True


def analyze_screenshot(path, floor_mask, label):
    """Analyze a screenshot for floor noise metrics."""
    _, luma = load_luminance(path)
    floor_luma = float(np.mean(luma[floor_mask]))
    flv = floor_laplacian_var(luma, floor_mask)
    fls = floor_local_std(luma, floor_mask)

    print(f"  {label:40s} luma={floor_luma:.4f}  lap_var={flv:.6f}  "
          f"local_std(mean={fls['mean']:.5f} p95={fls['p95']:.5f} p99={fls['p99']:.5f})")
    return {
        "floor_luma": floor_luma,
        "lap_var": flv,
        "local_std": fls,
    }


def main():
    args, extra_args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    sdir = get_screenshot_dir(fw)

    print("=" * 80)
    print("  Floor Noise Diagnostic")
    print("=" * 80)

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

    # Step 1: Vanilla reference at 2048 spp (ground truth)
    print(f"\n{'=' * 80}")
    print("  STEP 1: Vanilla reference (2048 spp)")
    print(f"{'=' * 80}")
    ok = run_app(py, build_py, fw, "screenshot",
                 ["--max_spp", "2048"] + extra_args,
                 "vanilla", use_reblur=False, clear_screenshots=True)
    if not ok:
        print("FAIL: vanilla run failed")
        return 1

    vanilla_path = find_screenshot(sdir, "*screenshot*")
    if not vanilla_path:
        print("FAIL: vanilla screenshot not found")
        return 1

    _, vanilla_luma = load_luminance(vanilla_path)
    floor_mask = get_floor_mask(vanilla_luma)
    n_floor = int(np.sum(floor_mask))
    print(f"  Floor mask: {n_floor} pixels "
          f"({n_floor / floor_mask.size * 100:.1f}% of image)")

    os.rename(vanilla_path, os.path.join(sdir, "diag_vanilla_2048.png"))
    vanilla_path = os.path.join(sdir, "diag_vanilla_2048.png")

    all_stats = {}
    all_stats["vanilla_2048"] = analyze_screenshot(vanilla_path, floor_mask,
                                                    "Vanilla 2048spp")

    # SPP levels to test
    spp_levels = [64, 256, 2048]

    # Step 2: Per-stage analysis at each SPP level
    configs = [
        # (label, extra_flags, use_reblur)
        ("vanilla", [], False),
        ("ta_only", ["--reblur_debug_pass", "TemporalAccum",
                     "--reblur_no_pt_blend", "true"], True),
        ("postblur", ["--reblur_debug_pass", "PostBlur",
                      "--reblur_no_pt_blend", "true"], True),
        ("denoised_only", ["--reblur_no_pt_blend", "true"], True),
        ("e2e", [], True),
    ]

    for spp in spp_levels:
        print(f"\n{'=' * 80}")
        print(f"  STEP 2: Per-stage analysis at {spp} spp")
        print(f"{'=' * 80}")

        for label, flags, use_reblur in configs:
            tag = f"{label}_{spp}"
            ok = run_app(py, build_py, fw, "screenshot",
                         ["--max_spp", str(spp)] + flags + extra_args,
                         tag, use_reblur=use_reblur, clear_screenshots=True)
            if not ok:
                print(f"  SKIP: {tag} failed")
                continue

            path = find_screenshot(sdir, "*screenshot*")
            if not path:
                print(f"  SKIP: {tag} screenshot not found")
                continue

            new_path = os.path.join(sdir, f"diag_{tag}.png")
            os.rename(path, new_path)
            all_stats[tag] = analyze_screenshot(new_path, floor_mask, f"{label} @ {spp}spp")

    # Step 3: Summary table
    print(f"\n{'=' * 80}")
    print("  SUMMARY: Floor Noise by Stage and SPP")
    print(f"{'=' * 80}")
    print(f"  {'Config':30s} {'Luma':>8s} {'LapVar':>10s} {'LocalStd':>10s} {'Ratio':>8s}")
    print(f"  {'-' * 30} {'-' * 8} {'-' * 10} {'-' * 10} {'-' * 8}")

    ref_lap = all_stats.get("vanilla_2048", {}).get("lap_var", 1e-10)

    for key in sorted(all_stats.keys()):
        s = all_stats[key]
        ratio = s["lap_var"] / max(ref_lap, 1e-10)
        print(f"  {key:30s} {s['floor_luma']:8.4f} {s['lap_var']:10.6f} "
              f"{s['local_std']['mean']:10.6f} {ratio:8.2f}x")

    # Step 4: Noise ratio progression
    print(f"\n{'=' * 80}")
    print("  NOISE PROGRESSION: denoised_only / vanilla at each SPP")
    print(f"{'=' * 80}")
    for spp in spp_levels:
        d_key = f"denoised_only_{spp}"
        v_key = f"vanilla_{spp}"
        if d_key in all_stats and v_key in all_stats:
            d_lap = all_stats[d_key]["lap_var"]
            v_lap = all_stats[v_key]["lap_var"]
            ratio = d_lap / max(v_lap, 1e-10)
            print(f"  {spp:4d} spp: denoised={d_lap:.6f} vanilla={v_lap:.6f} ratio={ratio:.2f}x")

    # Step 5: E2E vs vanilla at each SPP
    print(f"\n{'=' * 80}")
    print("  E2E / VANILLA noise ratio at each SPP")
    print(f"{'=' * 80}")
    for spp in spp_levels:
        e_key = f"e2e_{spp}"
        v_key = f"vanilla_{spp}"
        if e_key in all_stats and v_key in all_stats:
            e_lap = all_stats[e_key]["lap_var"]
            v_lap = all_stats[v_key]["lap_var"]
            ratio = e_lap / max(v_lap, 1e-10)
            print(f"  {spp:4d} spp: e2e={e_lap:.6f} vanilla={v_lap:.6f} ratio={ratio:.2f}x")

    # Step 6: Stage-by-stage noise at 2048 spp
    print(f"\n{'=' * 80}")
    print("  DENOISER PIPELINE BREAKDOWN at 2048 spp (floor noise)")
    print(f"{'=' * 80}")
    for stage in ["ta_only", "postblur", "denoised_only", "e2e"]:
        key = f"{stage}_2048"
        if key in all_stats:
            ratio = all_stats[key]["lap_var"] / max(ref_lap, 1e-10)
            print(f"  {stage:20s}: lap_var={all_stats[key]['lap_var']:.6f} "
                  f"({ratio:.2f}x vanilla)")

    print(f"\n  All diagnostic screenshots saved to: {sdir}/diag_*.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
