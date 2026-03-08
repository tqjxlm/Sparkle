#!/usr/bin/env python3
"""Motion-side history-valid shell regression for camera nudge.

This test targets the case where a small camera yaw keeps most object pixels
history-valid, but the motion-leading side of object silhouettes still becomes
noisy. That is the side that moves into another pixel's previous screen-space
position; for a small rigid-camera nudge it should remain almost as clean as
the converged vanilla reference.

Runs:
  0. Vanilla baseline (ground truth for the nudged view)
  1. Reblur full pipeline (user-visible end-to-end output)
  2. Reblur denoised-only (--reblur_no_pt_blend true)
  3. TADisocclusion (history-valid mask)
  4. TAMaterialId (instance silhouette extraction)

The test extracts visible object silhouettes from TAMaterialId, matches
before/after connected components, builds a thin boundary shell for each
object, and measures only the history-valid motion-leading half of that shell.
It also compares against the motion-trailing half to catch asymmetric noise.

Usage:
  python3 tests/reblur/test_motion_side_history.py --framework macos [--skip_build]
"""

import argparse
import glob
import os
import subprocess
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import binary_erosion, gaussian_filter, label

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, PROJECT_ROOT)

# TAMaterialId debug screenshots encode sky/floor as black and non-floor objects
# as non-zero current IDs in R. Filter out tiny speckles and keep only visible
# object silhouettes large enough to provide a stable shell metric.
MIN_COMPONENT_AREA = 1500
MIN_ANALYZED_COMPONENTS = 5
MAX_COMPONENT_MATCH_DISTANCE = 20.0
MIN_COMPONENT_MOTION_PX = 1.0
SHELL_EROSION_PX = 6
MIN_REGION_PIXELS = 200

# Motion-leading shells should stay mostly history-valid after a 2 degree yaw.
MIN_MEDIAN_HISTORY_VALID_FRACTION = 0.95

# Current bug state is far above these thresholds (top-3 mean ~= 6x).
MAX_TOP3_LEADING_HF_RATIO = 1.8
MAX_TOP3_LEAD_TRAIL_ASYMMETRY = 1.5


def parse_args():
    parser = argparse.ArgumentParser(
        description="Motion-side history-valid shell regression")
    parser.add_argument("--framework", default="macos",
                        choices=("glfw", "macos"))
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


def prepare_artifact_dir(screenshot_dir):
    artifact_dir = os.path.join(screenshot_dir, "motion_side_debug")
    os.makedirs(artifact_dir, exist_ok=True)
    for path in glob.glob(os.path.join(artifact_dir, "*.png")):
        os.remove(path)
    return artifact_dir


def load_image(path):
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def load_luminance(path):
    img = load_image(path)
    luma = img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722
    return img, luma


def compute_hf_residual(luma, sigma=2.0):
    return np.abs(luma - gaussian_filter(luma, sigma=sigma))


def get_direct_run_command(framework, test_case, extra_args,
                           use_reblur=True, clear_screenshots=False):
    if framework == "macos":
        executable = os.path.join(
            PROJECT_ROOT, "build_system", "macos", "output", "build",
            "sparkle.app", "Contents", "MacOS", "sparkle")
    elif framework == "glfw":
        executable = os.path.join(
            PROJECT_ROOT, "build_system", "glfw", "output", "build",
            "sparkle")
    else:
        raise ValueError(f"Unsupported direct-run framework: {framework}")

    cmd = [executable, "--test_case", test_case, "--headless", "true",
           "--pipeline", "gpu", "--spp", "1"]
    if clear_screenshots:
        cmd += ["--clear_screenshots", "true"]
    if use_reblur:
        cmd += ["--use_reblur", "true"]
    cmd += extra_args
    return cmd


def run_app(py, build_py, framework, test_case, extra_args, label,
            use_reblur=True, clear_screenshots=False, direct_run=False):
    if direct_run:
        cmd = get_direct_run_command(
            framework, test_case, extra_args,
            use_reblur=use_reblur, clear_screenshots=clear_screenshots)
    else:
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


def archive_run_pair(screenshot_dir, artifact_dir,
                     before_pattern, after_pattern, prefix):
    before_path = find_screenshot(screenshot_dir, before_pattern)
    after_path = find_screenshot(screenshot_dir, after_pattern)
    if not before_path or not after_path:
        return None, None

    before_dst = os.path.join(artifact_dir, f"{prefix}_before.png")
    after_dst = os.path.join(artifact_dir, f"{prefix}_after.png")
    os.rename(before_path, before_dst)
    os.rename(after_path, after_dst)
    return before_dst, after_dst


def extract_components(material_id_path):
    # Use the raw 8-bit R channel directly. In TAMaterialId screenshots, any
    # non-zero R value corresponds to a visible non-floor object pixel.
    image_u8 = np.array(Image.open(material_id_path).convert("RGB"))
    object_mask = image_u8[:, :, 0] > 0
    labels, num_labels = label(object_mask)
    components = []

    for label_id in range(1, num_labels + 1):
        ys, xs = np.where(labels == label_id)
        area = len(xs)
        if area < MIN_COMPONENT_AREA:
            continue

        components.append({
            "label_id": label_id,
            "area": area,
            "cx": float(np.mean(xs)),
            "cy": float(np.mean(ys)),
            "bbox": (int(np.min(xs)), int(np.min(ys)),
                      int(np.max(xs)), int(np.max(ys))),
        })

    components.sort(key=lambda c: c["area"], reverse=True)
    return labels, components


def match_components(before_components, after_components):
    matches = []
    used_before = set()

    for after in after_components:
        best = None
        for before in before_components:
            if before["label_id"] in used_before:
                continue

            dx = after["cx"] - before["cx"]
            dy = after["cy"] - before["cy"]
            distance = float(np.hypot(dx, dy))
            if best is None or distance < best[0]:
                best = (distance, before)

        if best is None:
            continue

        distance, before = best
        if distance > MAX_COMPONENT_MATCH_DISTANCE:
            continue

        used_before.add(before["label_id"])
        matches.append({
            "before": before,
            "after": after,
            "dx": after["cx"] - before["cx"],
            "dy": after["cy"] - before["cy"],
            "motion_px": distance,
        })

    return matches


def load_history_mask(disocclusion_path):
    img = load_image(disocclusion_path)
    disoccluded = img[:, :, 0] > 0.5
    in_screen = img[:, :, 2] > 0.5
    return (~disoccluded) & in_screen


def analyze_pass(pass_name, eval_after_path, vanilla_after_path,
                 labels_after, matches, history_mask):
    eval_img, eval_luma = load_luminance(eval_after_path)
    _, vanilla_luma = load_luminance(vanilla_after_path)
    eval_hf = compute_hf_residual(eval_luma)
    vanilla_hf = compute_hf_residual(vanilla_luma)

    h, w = history_mask.shape
    yy, xx = np.indices((h, w))

    component_metrics = []
    leading_union = np.zeros((h, w), dtype=bool)
    trailing_union = np.zeros((h, w), dtype=bool)

    for match in matches:
        if match["motion_px"] < MIN_COMPONENT_MOTION_PX:
            continue

        comp_mask = labels_after == match["after"]["label_id"]
        shell_mask = comp_mask & ~binary_erosion(comp_mask, iterations=SHELL_EROSION_PX)
        if np.sum(shell_mask) < MIN_REGION_PIXELS * 2:
            continue

        motion_dir = np.array([match["dx"], match["dy"]], dtype=np.float32)
        motion_dir /= np.linalg.norm(motion_dir)
        signed = (xx - match["after"]["cx"]) * motion_dir[0] + \
                 (yy - match["after"]["cy"]) * motion_dir[1]

        leading_shell = shell_mask & (signed > 0.0)
        trailing_shell = shell_mask & (signed < 0.0)
        leading_history = leading_shell & history_mask
        trailing_history = trailing_shell & history_mask

        if np.sum(leading_history) < MIN_REGION_PIXELS:
            continue
        if np.sum(trailing_history) < MIN_REGION_PIXELS:
            continue

        lead_history_fraction = float(
            np.sum(leading_history) / max(np.sum(leading_shell), 1))

        lead_ratio = float(
            np.mean(eval_hf[leading_history]) /
            max(np.mean(vanilla_hf[leading_history]), 1e-9))
        trail_ratio = float(
            np.mean(eval_hf[trailing_history]) /
            max(np.mean(vanilla_hf[trailing_history]), 1e-9))
        asymmetry = lead_ratio / max(trail_ratio, 1e-9)

        component_metrics.append({
            "component_id": match["after"]["label_id"],
            "area": match["after"]["area"],
            "motion_px": match["motion_px"],
            "dx": match["dx"],
            "dy": match["dy"],
            "lead_history_fraction": lead_history_fraction,
            "lead_ratio": lead_ratio,
            "trail_ratio": trail_ratio,
            "asymmetry": asymmetry,
            "leading_history": leading_history,
            "trailing_history": trailing_history,
        })

        leading_union |= leading_history
        trailing_union |= trailing_history

    print(f"\n  {pass_name}:")
    for metric in component_metrics:
        print(f"    comp {metric['component_id']:>2d} "
              f"area={metric['area']:>6d} "
              f"motion=({metric['dx']:+5.1f}, {metric['dy']:+4.1f}) "
              f"lead_valid={metric['lead_history_fraction']:.3f} "
              f"lead/van={metric['lead_ratio']:.2f}x "
              f"trail/van={metric['trail_ratio']:.2f}x "
              f"lead/trail={metric['asymmetry']:.2f}x")

    if len(component_metrics) < MIN_ANALYZED_COMPONENTS:
        return {
            "ok": False,
            "reason": (f"only {len(component_metrics)} components analyzed; "
                       f"need >= {MIN_ANALYZED_COMPONENTS}"),
            "metrics": component_metrics,
            "leading_union": leading_union,
            "trailing_union": trailing_union,
            "eval_img": eval_img,
            "eval_hf": eval_hf,
            "vanilla_hf": vanilla_hf,
        }

    lead_ratios = np.array([m["lead_ratio"] for m in component_metrics], dtype=np.float32)
    asymmetry = np.array([m["asymmetry"] for m in component_metrics], dtype=np.float32)
    lead_valid = np.array([m["lead_history_fraction"] for m in component_metrics],
                          dtype=np.float32)
    top_count = min(3, len(component_metrics))
    top3_lead_mean = float(np.mean(np.sort(lead_ratios)[-top_count:]))
    top3_asym_mean = float(np.mean(np.sort(asymmetry)[-top_count:]))
    median_history_fraction = float(np.median(lead_valid))

    print(f"    analyzed components:        {len(component_metrics)}")
    print(f"    median leading valid frac: {median_history_fraction:.3f}")
    print(f"    top-{top_count} leading HF ratio mean: {top3_lead_mean:.2f}x")
    print(f"    top-{top_count} lead/trail asym mean:  {top3_asym_mean:.2f}x")

    failures = []
    if median_history_fraction < MIN_MEDIAN_HISTORY_VALID_FRACTION:
        failures.append(
            f"median leading history fraction {median_history_fraction:.3f} < "
            f"{MIN_MEDIAN_HISTORY_VALID_FRACTION:.3f}")
    if top3_lead_mean > MAX_TOP3_LEADING_HF_RATIO:
        failures.append(
            f"top-{top_count} leading HF ratio mean {top3_lead_mean:.2f} > "
            f"{MAX_TOP3_LEADING_HF_RATIO:.2f}")
    if top3_asym_mean > MAX_TOP3_LEAD_TRAIL_ASYMMETRY:
        failures.append(
            f"top-{top_count} lead/trail asym mean {top3_asym_mean:.2f} > "
            f"{MAX_TOP3_LEAD_TRAIL_ASYMMETRY:.2f}")

    return {
        "ok": not failures,
        "reason": "; ".join(failures),
        "metrics": component_metrics,
        "leading_union": leading_union,
        "trailing_union": trailing_union,
        "eval_img": eval_img,
        "eval_hf": eval_hf,
        "vanilla_hf": vanilla_hf,
        "top3_lead_mean": top3_lead_mean,
        "top3_asym_mean": top3_asym_mean,
        "median_history_fraction": median_history_fraction,
    }


def save_diagnostics(artifact_dir, tag, analysis):
    overlay = np.clip(analysis["eval_img"], 0, 1)
    overlay = (overlay * 255).astype(np.uint8)
    overlay[analysis["trailing_union"]] = [0, 255, 0]
    overlay[analysis["leading_union"]] = [255, 0, 0]
    Image.fromarray(overlay).save(
        os.path.join(artifact_dir, f"diag_motion_side_overlay_{tag}.png"))

    excess = np.clip((analysis["eval_hf"] - analysis["vanilla_hf"]) / 0.05, 0, 1)
    heatmap = np.zeros_like(overlay)
    heatmap[:, :, 0] = (excess * 255).astype(np.uint8)
    heatmap[:, :, 2] = ((1.0 - excess) * 64).astype(np.uint8)
    heatmap[analysis["trailing_union"]] = [0, 180, 0]
    heatmap[analysis["leading_union"]] = [255, 64, 0]
    Image.fromarray(heatmap).save(
        os.path.join(artifact_dir, f"diag_motion_side_excess_{tag}.png"))


def print_result(name, ok, failure_reason):
    if ok:
        print(f"  PASS: {name}")
    else:
        print(f"  FAIL: {name} - {failure_reason}")


def main():
    args, extra_args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)
    artifact_dir = prepare_artifact_dir(screenshot_dir)

    print("=" * 70)
    print("  Motion-Side History Shell Regression")
    print("=" * 70)

    if not args.skip_build:
        print("\nBuilding...")
        result = subprocess.run([py, build_py, "--framework", fw] + extra_args,
                                cwd=PROJECT_ROOT, capture_output=True,
                                text=True)
        if result.returncode != 0:
            print("FAIL: build failed")
            if result.stderr:
                for line in result.stderr.strip().splitlines()[-10:]:
                    print(f"  {line}")
            return 1

    # Run 0: vanilla baseline
    print(f"\n{'-' * 70}")
    print("  Run 0: Vanilla baseline")
    print(f"{'-' * 70}")
    direct_run = args.skip_build and fw in ("macos", "glfw")
    ok = run_app(py, build_py, fw, "vanilla_converged_baseline",
                 extra_args, "vanilla", use_reblur=False,
                 clear_screenshots=True, direct_run=direct_run)
    if not ok:
        return 1
    vanilla_before_path, vanilla_after_path = archive_run_pair(
        screenshot_dir, artifact_dir,
        "*vanilla_baseline_before*",
        "*vanilla_baseline_after*",
        "motion_side_vanilla")
    if not vanilla_before_path or not vanilla_after_path:
        print("FAIL: vanilla screenshots not found")
        return 1

    # Run 1: end-to-end full pipeline
    print(f"\n{'-' * 70}")
    print("  Run 1: Reblur full pipeline")
    print(f"{'-' * 70}")
    ok = run_app(py, build_py, fw, "reblur_converged_history",
                 extra_args, "full pipeline", clear_screenshots=True,
                 direct_run=direct_run)
    if not ok:
        return 1
    e2e_before_path, e2e_after_path = archive_run_pair(
        screenshot_dir, artifact_dir,
        "*converged_history_before*",
        "*converged_history_after*",
        "motion_side_e2e")
    if not e2e_before_path or not e2e_after_path:
        print("FAIL: full-pipeline screenshots not found")
        return 1

    # Run 2: denoised-only
    print(f"\n{'-' * 70}")
    print("  Run 2: Reblur denoised-only")
    print(f"{'-' * 70}")
    ok = run_app(py, build_py, fw, "reblur_converged_history",
                 ["--reblur_no_pt_blend", "true"] + extra_args,
                 "denoised-only", clear_screenshots=True,
                 direct_run=direct_run)
    if not ok:
        return 1
    denoised_before_path, denoised_after_path = archive_run_pair(
        screenshot_dir, artifact_dir,
        "*converged_history_before*",
        "*converged_history_after*",
        "motion_side_denoised")
    if not denoised_before_path or not denoised_after_path:
        print("FAIL: denoised-only screenshots not found")
        return 1

    # Run 3: TADisocclusion
    print(f"\n{'-' * 70}")
    print("  Run 3: TADisocclusion")
    print(f"{'-' * 70}")
    ok = run_app(py, build_py, fw, "reblur_converged_history",
                 ["--reblur_debug_pass", "TADisocclusion"] + extra_args,
                 "disocclusion", clear_screenshots=True,
                 direct_run=direct_run)
    if not ok:
        return 1
    _, disocclusion_after_path = archive_run_pair(
        screenshot_dir, artifact_dir,
        "*converged_history_before*",
        "*converged_history_after*",
        "motion_side_disocclusion")
    if not disocclusion_after_path:
        print("FAIL: disocclusion screenshot not found")
        return 1

    # Run 4: TAMaterialId
    print(f"\n{'-' * 70}")
    print("  Run 4: TAMaterialId")
    print(f"{'-' * 70}")
    ok = run_app(py, build_py, fw, "reblur_converged_history",
                 ["--reblur_debug_pass", "TAMaterialId"] + extra_args,
                 "material id", clear_screenshots=True,
                 direct_run=direct_run)
    if not ok:
        return 1
    material_before_path, material_after_path = archive_run_pair(
        screenshot_dir, artifact_dir,
        "*converged_history_before*",
        "*converged_history_after*",
        "motion_side_materialid")
    if not material_before_path or not material_after_path:
        print("FAIL: material-id screenshots not found")
        return 1

    print(f"\n{'=' * 70}")
    print("  Semantic Analysis: History-Valid Motion-Leading Shells")
    print(f"{'=' * 70}")

    history_mask = load_history_mask(disocclusion_after_path)
    labels_before, before_components = extract_components(material_before_path)
    labels_after, after_components = extract_components(material_after_path)
    matches = match_components(before_components, after_components)

    print(f"  Before components: {len(before_components)}")
    print(f"  After components:  {len(after_components)}")
    print(f"  Matched components: {len(matches)}")
    if len(matches) < MIN_ANALYZED_COMPONENTS:
        print(f"FAIL: only {len(matches)} matched components, need >= {MIN_ANALYZED_COMPONENTS}")
        return 1

    full_analysis = analyze_pass(
        "Full pipeline", e2e_after_path, vanilla_after_path,
        labels_after, matches, history_mask)
    denoised_analysis = analyze_pass(
        "Denoised-only", denoised_after_path, vanilla_after_path,
        labels_after, matches, history_mask)

    save_diagnostics(artifact_dir, "e2e", full_analysis)
    save_diagnostics(artifact_dir, "denoised", denoised_analysis)
    print(f"\n  Saved diagnostics to {artifact_dir}/diag_motion_side_*.png")

    print("")
    print_result("Full pipeline motion-leading shell", full_analysis["ok"],
                 full_analysis["reason"])
    print_result("Denoised-only motion-leading shell", denoised_analysis["ok"],
                 denoised_analysis["reason"])

    return 0 if full_analysis["ok"] and denoised_analysis["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
