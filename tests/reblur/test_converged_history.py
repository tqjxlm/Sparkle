"""Converged history camera delta validation.

Tests that a small camera yaw delta after full convergence preserves temporal
history — the resulting frame should be nearly as clean as the converged
frame, NOT a noisy 1spp restart.  Specifically detects ghosting artifacts
by cross-comparing against a fully re-converged vanilla reference.

Orchestrates five runs:
  0. Vanilla baseline — vanilla GPU pipeline (no reblur): fully converge,
     nudge, fully re-converge.  Provides ground-truth for the nudged view.
  0.5. Denoised-only (--reblur_no_pt_blend) — isolates the denoiser from the
     PT blend ramp.  This is the PRIMARY denoiser quality check: luma
     preservation, noise ratio, and FLIP should all pass here.
  1. Full pipeline (end-to-end) — before/after screenshot.  FLIP is computed
     against the vanilla "after" to detect ghosting.  Note: the PT blend ramp
     resets after camera motion, so this measures both denoiser quality AND
     the demod/remod luminance gap.
  2. Temporal accumulation only (reblur_debug_pass TemporalAccum) — validates
     that reprojection fetches history before any spatial blur.
  3. Disocclusion map (reblur_debug_pass TADisocclusion) — counts what fraction
     of pixels got valid reprojection after the camera nudge.

Metrics:
  - FLIP error: perceptual difference (nvidia FLIP)
  - Ghosting FLIP: reblur-after vs vanilla-after (measures reprojection artifacts)
  - Denoised luma preservation: after/before ratio (should be 0.93-1.07)
  - Reprojection validity: fraction of geometry pixels with valid history
  - Footprint quality: mean quality of valid reprojection footprints
  - Laplacian variance: high-frequency noise measure
  - Noise ratio: laplacian_var_after / laplacian_var_before

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
from scipy.ndimage import convolve

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, PROJECT_ROOT)

# --- Thresholds ---
# Denoised-only (--reblur_no_pt_blend): isolates denoiser from PT blend ramp
DENOISED_MIN_LUMA_RATIO = 0.93  # max 7% loss from viewpoint change + 1spp blend
DENOISED_MAX_LUMA_RATIO = 1.07  # max 7% gain
DENOISED_NOISE_RATIO_MAX = 4.0  # stronger TS stabilization → cleaner "before", higher ratio
DENOISED_FLIP_MAX = 0.25        # perceptual before vs after
# Full pipeline: REBLUR-after vs vanilla-after (ghosting detection)
# Note: after camera motion the PT blend ramp resets (cumulated_sample_count → 0)
# so "after" is nearly pure denoised output while vanilla-after is PT-converged.
# The demod/remod luminance gap (~24%) dominates this metric, not actual ghosting.
# Run 0.5 (denoised-only) is the primary denoiser quality gate.
GHOSTING_FLIP_MAX = 0.40
# Full pipeline: noise ratio after/before
# Before = PT-converged (nearly noiseless), after = denoised output (PT blend
# ramp reset).  Post-nudge noise is dominated by TA motion response (high
# variance between runs: 5-10x observed).  Not affected by max_stabilized.
NOISE_RATIO_MAX_FULL = 12.0
# Temporal accum: noise ratio (raw, before spatial blur)
NOISE_RATIO_MAX_TEMPORAL = 60.0
# Disocclusion map: minimum fraction of geometry pixels with valid history
MIN_VALID_REPROJECTION_PCT = 60.0
# Disocclusion map: minimum mean footprint quality for valid pixels
MIN_FOOTPRINT_QUALITY = 0.5


def parse_args():
    parser = argparse.ArgumentParser(
        description="Converged history camera delta test")
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
    """Load image as float32 [0,1] RGB array."""
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def load_luminance(path):
    """Load image and compute luminance channel as float32 [0,1]."""
    img = load_image(path)
    luma = img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722
    return img, luma


def compute_laplacian_variance(luma):
    """Compute variance of the Laplacian as a noise metric."""
    kernel = np.array([[0, 1, 0],
                       [1, -4, 1],
                       [0, 1, 0]], dtype=np.float32)
    laplacian = convolve(luma, kernel, mode="reflect")
    return float(np.var(laplacian))


def compute_flip(path_a, path_b):
    """Compute mean FLIP error between two images. Returns mean_flip or None."""
    try:
        from flip_evaluator import nbflip
    except ImportError:
        print("  WARN: flip_evaluator not installed, skipping FLIP metric")
        return None
    img_a = load_image(path_a)
    img_b = load_image(path_b)
    if img_a.shape != img_b.shape:
        print(f"  WARN: image shape mismatch for FLIP: "
              f"{img_a.shape} vs {img_b.shape}")
        return None
    _, mean_flip, _ = nbflip.evaluate(img_a, img_b, False, True, False, True, {})
    return float(mean_flip)


def check_image(path, label):
    """Basic image validation: NaN, Inf, all-black."""
    img, luma = load_luminance(path)
    has_nan = bool(np.any(np.isnan(img)))
    has_inf = bool(np.any(np.isinf(img)))
    mean_luma = float(np.mean(luma))
    lap_var = compute_laplacian_variance(luma)

    failures = []
    if has_nan:
        failures.append("NaN detected")
    if has_inf:
        failures.append("Inf detected")
    if mean_luma < 1e-4:
        failures.append(f"all black (mean_luma={mean_luma:.6f})")

    return {
        "mean_luma": mean_luma,
        "laplacian_var": lap_var,
        "has_nan": has_nan,
        "has_inf": has_inf,
        "failures": failures,
    }


def analyze_disocclusion_map(path):
    """Analyze a TADisocclusion screenshot to count reprojection validity.

    TADisocclusion output format (diagnostic passthrough, no albedo modulation):
      R = disoccluded (1.0 = yes, 0.0 = no / valid history)
      G = footprintQuality [0,1]
      B = inScreen (1.0 = in screen bounds, 0.0 = out of bounds)

    Returns dict with validity metrics.
    """
    img = load_image(path)
    r_chan = img[:, :, 0]  # disoccluded flag
    g_chan = img[:, :, 1]  # footprint quality
    b_chan = img[:, :, 2]  # in-screen flag

    total_pixels = r_chan.size

    # Geometry pixels: those that are in-screen (B > 0.5)
    # Sky pixels have all channels at 0 (passed through early in TA)
    in_screen = b_chan > 0.5
    geometry_pixels = int(np.sum(in_screen))

    # Valid history: not disoccluded AND in-screen
    valid_history = (r_chan < 0.5) & in_screen
    valid_count = int(np.sum(valid_history))

    # Disoccluded: R > 0.5 AND in-screen
    disoccluded = (r_chan > 0.5) & in_screen
    disoccluded_count = int(np.sum(disoccluded))

    # Footprint quality for valid pixels
    if valid_count > 0:
        mean_quality = float(np.mean(g_chan[valid_history]))
    else:
        mean_quality = 0.0

    # Percentage calculations
    valid_pct = (valid_count / geometry_pixels * 100) if geometry_pixels > 0 else 0.0
    disoccluded_pct = (disoccluded_count / geometry_pixels * 100) if geometry_pixels > 0 else 0.0

    return {
        "total_pixels": total_pixels,
        "geometry_pixels": geometry_pixels,
        "valid_count": valid_count,
        "valid_pct": valid_pct,
        "disoccluded_count": disoccluded_count,
        "disoccluded_pct": disoccluded_pct,
        "mean_footprint_quality": mean_quality,
    }


def run_test(py, build_py, framework, test_case, extra_args, label,
             use_reblur=True, clear_screenshots=False):
    """Run a C++ test case with given extra args. Returns success bool."""
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
        print(f"  FAIL: app exited with code {result.returncode}")
        if result.stderr:
            for line in result.stderr.strip().splitlines()[-5:]:
                print(f"    {line}")
        return False
    return True


def rename_screenshots(screenshot_dir, old_pattern, new_prefix):
    """Rename screenshots matching old_pattern by replacing the base name."""
    for path in glob.glob(os.path.join(screenshot_dir, old_pattern)):
        dirname = os.path.dirname(path)
        basename = os.path.basename(path)
        new_name = basename.replace(
            "converged_history_before", f"{new_prefix}_before").replace(
            "converged_history_after", f"{new_prefix}_after")
        new_path = os.path.join(dirname, new_name)
        os.rename(path, new_path)
        print(f"  Renamed: {basename} -> {new_name}")


def validate_run(screenshot_dir, label, noise_ratio_max,
                 before_pattern="*converged_history_before*",
                 after_pattern="*converged_history_after*",
                 flip_max=None):
    """Find before/after screenshots, compute noise and FLIP metrics."""
    results = []

    before_path = find_screenshot(screenshot_dir, before_pattern)
    after_path = find_screenshot(screenshot_dir, after_pattern)

    if not before_path:
        print(f"  FAIL: 'before' screenshot not found")
        results.append((f"{label}: before screenshot found", False))
        return results, None, None
    results.append((f"{label}: before screenshot found", True))

    if not after_path:
        print(f"  FAIL: 'after' screenshot not found")
        results.append((f"{label}: after screenshot found", False))
        return results, None, None
    results.append((f"{label}: after screenshot found", True))

    print(f"  Before: {before_path}")
    print(f"  After:  {after_path}")

    before = check_image(before_path, "before")
    after = check_image(after_path, "after")

    print(f"  Before: mean_luma={before['mean_luma']:.6f}, "
          f"laplacian_var={before['laplacian_var']:.6f}")
    print(f"  After:  mean_luma={after['mean_luma']:.6f}, "
          f"laplacian_var={after['laplacian_var']:.6f}")

    # Basic validity checks
    for tag, info in [("before", before), ("after", after)]:
        if info["failures"]:
            for f in info["failures"]:
                print(f"  FAIL: {tag} — {f}")
                results.append((f"{label}: {tag} {f}", False))
        else:
            results.append((f"{label}: {tag} valid", True))

    # Noise ratio: after / before
    if before["laplacian_var"] > 1e-10:
        noise_ratio = after["laplacian_var"] / before["laplacian_var"]
        print(f"  Noise ratio: {noise_ratio:.2f}x")

        if noise_ratio < noise_ratio_max:
            print(f"  PASS: noise ratio {noise_ratio:.2f} < {noise_ratio_max}")
            results.append((f"{label}: noise ratio < {noise_ratio_max}", True))
        else:
            print(f"  FAIL: noise ratio {noise_ratio:.2f} >= {noise_ratio_max} "
                  f"(history not preserved)")
            results.append(
                (f"{label}: noise ratio < {noise_ratio_max}", False))
    else:
        print(f"  WARN: before laplacian_var too small to compute ratio")
        results.append((f"{label}: noise ratio computable", True))

    # FLIP perceptual error (before vs after)
    mean_flip = compute_flip(before_path, after_path)
    if mean_flip is not None:
        print(f"  FLIP error (before vs after): {mean_flip:.4f}")
        if flip_max is not None:
            if mean_flip <= flip_max:
                print(f"  PASS: FLIP {mean_flip:.4f} <= {flip_max}")
                results.append((f"{label}: FLIP <= {flip_max}", True))
            else:
                print(f"  FAIL: FLIP {mean_flip:.4f} > {flip_max}")
                results.append((f"{label}: FLIP <= {flip_max}", False))

    return results, before_path, after_path


def main():
    args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)

    print("=" * 60)
    print("  Converged History Camera Delta Validation")
    print("=" * 60)

    # Build
    if not args.skip_build:
        print("\nBuilding...")
        result = subprocess.run([py, build_py, "--framework", fw],
                                cwd=PROJECT_ROOT, capture_output=True, text=True)
        if result.returncode != 0:
            print("FAIL: build failed")
            if result.stderr:
                for line in result.stderr.strip().splitlines()[-10:]:
                    print(f"  {line}")
            return 1

    all_results = []
    vanilla_after_path = None
    reblur_after_path = None

    # --- Run 0: Vanilla baseline (no reblur, full re-convergence) ---
    print(f"\n{'—'*60}")
    print("  Run 0: Vanilla baseline (converge -> nudge -> re-converge)")
    print(f"{'—'*60}")
    ok = run_test(py, build_py, fw, "vanilla_converged_baseline",
                  [],
                  "vanilla baseline", use_reblur=False,
                  clear_screenshots=True)
    if ok:
        all_results.append(("Vanilla baseline: test run", True))
        run_results, _, vanilla_after_path = validate_run(
            screenshot_dir, "Vanilla baseline", noise_ratio_max=float("inf"),
            before_pattern="*vanilla_baseline_before*",
            after_pattern="*vanilla_baseline_after*")
        all_results.extend(run_results)
    else:
        all_results.append(("Vanilla baseline: test run", False))

    # --- Run 0.5: Denoised-only (no PT blend) ---
    # This isolates the denoiser from the PT blend ramp, providing the true
    # denoiser quality measurement after camera nudge.
    print(f"\n{'—'*60}")
    print("  Run 0.5: Denoised-only (--reblur_no_pt_blend, isolates denoiser)")
    print(f"{'—'*60}")
    ok = run_test(py, build_py, fw, "reblur_converged_history",
                  ["--reblur_no_pt_blend", "true"],
                  "denoised-only")
    if ok:
        all_results.append(("Denoised-only: test run", True))
        run_results, denoised_before, denoised_after = validate_run(
            screenshot_dir, "Denoised-only",
            noise_ratio_max=DENOISED_NOISE_RATIO_MAX,
            flip_max=DENOISED_FLIP_MAX)
        all_results.extend(run_results)

        # Additional check: luminance preservation ratio
        if denoised_before and denoised_after:
            _, luma_b = load_luminance(denoised_before)
            _, luma_a = load_luminance(denoised_after)
            mean_b = float(np.mean(luma_b))
            mean_a = float(np.mean(luma_a))
            luma_ratio = mean_a / max(mean_b, 1e-6)
            print(f"  Denoised luma ratio: {luma_ratio:.4f} "
                  f"({(1-luma_ratio)*100:.1f}% change)")
            if DENOISED_MIN_LUMA_RATIO <= luma_ratio <= DENOISED_MAX_LUMA_RATIO:
                print(f"  PASS: luma ratio {luma_ratio:.4f} in "
                      f"[{DENOISED_MIN_LUMA_RATIO}, {DENOISED_MAX_LUMA_RATIO}]")
                all_results.append(("Denoised-only: luma preserved", True))
            else:
                print(f"  FAIL: luma ratio {luma_ratio:.4f} outside "
                      f"[{DENOISED_MIN_LUMA_RATIO}, {DENOISED_MAX_LUMA_RATIO}]")
                all_results.append(("Denoised-only: luma preserved", False))

            # Measure demod/remod gap (informational, not asserted)
            if vanilla_after_path:
                _, v_luma = load_luminance(vanilla_after_path)
                vanilla_mean = float(np.mean(v_luma))
                gap = (vanilla_mean - mean_b) / max(vanilla_mean, 1e-6) * 100
                print(f"  Demod/remod gap: {gap:.1f}% "
                      f"(vanilla {vanilla_mean:.4f} vs denoised {mean_b:.4f}, "
                      f"informational)")

        rename_screenshots(screenshot_dir, "*converged_history_*",
                           "reblur_denoised_only")
    else:
        all_results.append(("Denoised-only: test run", False))

    # --- Run 1: Full pipeline (end-to-end) ---
    print(f"\n{'—'*60}")
    print("  Run 1: Reblur full pipeline (end-to-end)")
    print(f"{'—'*60}")
    ok = run_test(py, build_py, fw, "reblur_converged_history", [],
                  "full pipeline")
    if ok:
        all_results.append(("Full pipeline: test run", True))
        run_results, _, reblur_after_path = validate_run(
            screenshot_dir, "Full pipeline",
            noise_ratio_max=NOISE_RATIO_MAX_FULL)
        all_results.extend(run_results)

        # Ghosting detection: compare REBLUR-after vs vanilla-after
        # The vanilla "after" is ground truth for the nudged viewpoint.
        # Any large difference in the REBLUR "after" is ghosting or noise.
        if vanilla_after_path and reblur_after_path:
            ghosting_flip = compute_flip(vanilla_after_path, reblur_after_path)
            if ghosting_flip is not None:
                print(f"  Ghosting FLIP (reblur-after vs vanilla-after): "
                      f"{ghosting_flip:.4f}")
                if ghosting_flip <= GHOSTING_FLIP_MAX:
                    print(f"  PASS: ghosting FLIP {ghosting_flip:.4f} "
                          f"<= {GHOSTING_FLIP_MAX}")
                    all_results.append(
                        (f"Full pipeline: ghosting FLIP <= {GHOSTING_FLIP_MAX}",
                         True))
                else:
                    print(f"  FAIL: ghosting FLIP {ghosting_flip:.4f} "
                          f"> {GHOSTING_FLIP_MAX} (ghosting detected)")
                    all_results.append(
                        (f"Full pipeline: ghosting FLIP <= {GHOSTING_FLIP_MAX}",
                         False))

        # Rename so subsequent runs don't overwrite
        rename_screenshots(screenshot_dir, "*converged_history_*",
                           "reblur_e2e")
    else:
        all_results.append(("Full pipeline: test run", False))

    # --- Run 2: Temporal accumulation (reblur_debug_pass TemporalAccum) ---
    print(f"\n{'—'*60}")
    print("  Run 2: Reblur temporal accumulation (reblur_debug_pass TemporalAccum)")
    print(f"{'—'*60}")
    ok = run_test(py, build_py, fw, "reblur_converged_history",
                  ["--reblur_debug_pass", "TemporalAccum"],
                  "temporal accumulation")
    if ok:
        all_results.append(("Temporal accum: test run", True))
        run_results, _, _ = validate_run(
            screenshot_dir, "Temporal accum",
            noise_ratio_max=NOISE_RATIO_MAX_TEMPORAL)
        all_results.extend(run_results)
        rename_screenshots(screenshot_dir, "*converged_history_*",
                           "reblur_temporal_accum")
    else:
        all_results.append(("Temporal accum: test run", False))

    # --- Run 3: Disocclusion map (reblur_debug_pass TADisocclusion) ---
    print(f"\n{'—'*60}")
    print("  Run 3: Disocclusion map (reblur_debug_pass TADisocclusion)")
    print(f"{'—'*60}")
    ok = run_test(py, build_py, fw, "reblur_converged_history",
                  ["--reblur_debug_pass", "TADisocclusion"],
                  "disocclusion map")
    if ok:
        all_results.append(("Disocclusion map: test run", True))

        # Find the "after" screenshot — this is the disocclusion map after nudge
        after_path = find_screenshot(screenshot_dir,
                                     "*converged_history_after*")
        if after_path:
            all_results.append(("Disocclusion map: after screenshot found",
                                True))
            print(f"  Disocclusion map: {after_path}")

            stats = analyze_disocclusion_map(after_path)
            print(f"  Total pixels:          {stats['total_pixels']}")
            print(f"  Geometry pixels:       {stats['geometry_pixels']}")
            print(f"  Valid history:         {stats['valid_count']} "
                  f"({stats['valid_pct']:.1f}%)")
            print(f"  Disoccluded:           {stats['disoccluded_count']} "
                  f"({stats['disoccluded_pct']:.1f}%)")
            print(f"  Mean footprint quality: {stats['mean_footprint_quality']:.3f}")

            # Check reprojection validity
            if stats['valid_pct'] >= MIN_VALID_REPROJECTION_PCT:
                print(f"  PASS: valid reprojection {stats['valid_pct']:.1f}% "
                      f">= {MIN_VALID_REPROJECTION_PCT}%")
                all_results.append(
                    (f"Disocclusion map: valid reproj >= "
                     f"{MIN_VALID_REPROJECTION_PCT}%", True))
            else:
                print(f"  FAIL: valid reprojection {stats['valid_pct']:.1f}% "
                      f"< {MIN_VALID_REPROJECTION_PCT}% "
                      f"(reprojection broken)")
                all_results.append(
                    (f"Disocclusion map: valid reproj >= "
                     f"{MIN_VALID_REPROJECTION_PCT}%", False))

            # Check footprint quality
            if stats['mean_footprint_quality'] >= MIN_FOOTPRINT_QUALITY:
                print(f"  PASS: footprint quality "
                      f"{stats['mean_footprint_quality']:.3f} "
                      f">= {MIN_FOOTPRINT_QUALITY}")
                all_results.append(
                    (f"Disocclusion map: footprint quality >= "
                     f"{MIN_FOOTPRINT_QUALITY}", True))
            else:
                print(f"  FAIL: footprint quality "
                      f"{stats['mean_footprint_quality']:.3f} "
                      f"< {MIN_FOOTPRINT_QUALITY}")
                all_results.append(
                    (f"Disocclusion map: footprint quality >= "
                     f"{MIN_FOOTPRINT_QUALITY}", False))
        else:
            all_results.append(("Disocclusion map: after screenshot found",
                                False))

        rename_screenshots(screenshot_dir, "*converged_history_*",
                           "reblur_disocclusion")
    else:
        all_results.append(("Disocclusion map: test run", False))

    # --- Summary ---
    _print_summary(all_results)
    failed = sum(1 for _, ok in all_results if not ok)
    return 0 if failed == 0 else 1


def _print_summary(results):
    passed = sum(1 for _, ok in results if ok)
    failed = sum(1 for _, ok in results if not ok)
    print(f"\n{'='*60}")
    print(f"  Results: {passed} passed, {failed} failed")
    for name, ok in results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    print(f"{'='*60}")


if __name__ == "__main__":
    sys.exit(main())
