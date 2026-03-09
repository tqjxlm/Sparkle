#!/usr/bin/env python3
"""Semantic Run 1 end-to-end regression for converged-history camera nudges.

This test checks the user-visible failure directly: after a tiny camera yaw,
the history-valid motion-leading side of an object should still look like the
converged vanilla reference, not like a newly noisy shell.

The pass/fail rule is semantic rather than a global noise ratio:
  1. Extract object silhouettes from TAMaterialId.
  2. Match before/after components and infer motion direction.
  3. Isolate the history-valid motion-leading shell from TADisocclusion.
  4. Compare that shell against the converged vanilla reference.
  5. Fail if any object develops a continuous wrong-looking arc on the
     history-valid shell.

Runs:
  0. Vanilla baseline (ground truth for the nudged viewpoint)
  1. REBLUR full pipeline (Run 1 from test_converged_history.py)
  2. TADisocclusion (history-valid mask)
  3. TAMaterialId (object silhouettes)

Optional:
  --analyze_only reuses archived screenshots in the semantic debug folder
  instead of invoking the app again.

Usage:
  python3 tests/reblur/test_run1_semantic_e2e.py --framework macos [--skip_build]
  python3 tests/reblur/test_run1_semantic_e2e.py --framework macos --analyze_only
"""

import argparse
import glob
import os
import subprocess
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import binary_erosion, label

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, PROJECT_ROOT)

MIN_COMPONENT_AREA = 1500
MIN_ANALYZED_COMPONENTS = 5
MAX_COMPONENT_MATCH_DISTANCE = 20.0
MIN_COMPONENT_MOTION_PX = 1.0
SHELL_EROSION_PX = 6
CORE_EROSION_PX = 10
MIN_LEADING_HISTORY_PIXELS = 200
MIN_CORE_PIXELS = 400

ARC_BIN_COUNT = 32
MIN_PIXELS_PER_ARC_BIN = 16
BAD_BIN_FRACTION_THRESHOLD = 0.18
MAX_BAD_ARC_RUN_BINS = 3
MAX_FAILED_COMPONENTS = 1

MIN_RGB_BAD_THRESHOLD = 0.03
MIN_LUMA_BAD_THRESHOLD = 0.018
RGB_CORE_BAD_MARGIN = 0.006
LUMA_CORE_BAD_MARGIN = 0.005


def parse_args():
    parser = argparse.ArgumentParser(
        description="Semantic Run 1 end-to-end motion-shell regression")
    parser.add_argument("--framework", default="macos",
                        choices=("glfw", "macos"))
    parser.add_argument("--skip_build", action="store_true")
    parser.add_argument("--analyze_only", action="store_true")
    return parser.parse_known_args()


def get_screenshot_dir(framework):
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    return os.path.join(PROJECT_ROOT, "build_system", "glfw", "output",
                        "build", "generated", "screenshots")


def prepare_artifact_dir(screenshot_dir, analyze_only):
    artifact_dir = os.path.join(screenshot_dir, "run1_semantic_debug")
    os.makedirs(artifact_dir, exist_ok=True)
    if not analyze_only:
        for path in glob.glob(os.path.join(artifact_dir, "*.png")):
            os.remove(path)
    return artifact_dir


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
    luma = img[:, :, 0] * 0.2126 + img[:, :, 1] * \
        0.7152 + img[:, :, 2] * 0.0722
    return img, luma


def get_direct_run_command(framework, test_case, extra_args,
                           clear_screenshots=False):
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

    cmd = [executable, "--test_case", test_case, "--headless", "true"]
    if clear_screenshots:
        cmd += ["--clear_screenshots", "true"]
    cmd += extra_args
    return cmd


def run_app(py, build_py, framework, test_case, extra_args, label,
            clear_screenshots=False, direct_run=False):
    if direct_run:
        cmd = get_direct_run_command(
            framework, test_case, extra_args, clear_screenshots=clear_screenshots)
    else:
        cmd = [py, build_py, "--framework", framework, "--skip_build",
               "--run", "--test_case", test_case, "--headless", "true"]
        if clear_screenshots:
            cmd += ["--clear_screenshots", "true"]
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


def load_history_masks(disocclusion_path):
    img = load_image(disocclusion_path)
    disoccluded = img[:, :, 0] > 0.5
    in_screen = img[:, :, 2] > 0.5
    history = (~disoccluded) & in_screen
    return history, disoccluded


def compute_longest_circular_run(mask):
    if mask.size == 0 or not np.any(mask):
        return 0

    longest = 0
    current = 0
    for value in np.concatenate([mask, mask]):
        if value:
            current += 1
            longest = max(longest, current)
        else:
            current = 0

    return min(longest, mask.size)


def analyze_semantic_run1(e2e_after_path, vanilla_after_path,
                          material_before_path, material_after_path,
                          disocclusion_after_path):
    eval_img, eval_luma = load_luminance(e2e_after_path)
    vanilla_img, vanilla_luma = load_luminance(vanilla_after_path)
    labels_before, before_components = extract_components(material_before_path)
    labels_after, after_components = extract_components(material_after_path)
    matches = match_components(before_components, after_components)
    history_mask, disoccluded_mask = load_history_masks(
        disocclusion_after_path)

    del vanilla_img, labels_before

    rgb_error = np.max(
        np.abs(eval_img - load_image(vanilla_after_path)), axis=2)
    luma_error = np.abs(eval_luma - vanilla_luma)

    h, w = history_mask.shape
    yy, xx = np.indices((h, w))
    failing_union = np.zeros((h, w), dtype=bool)
    leading_union = np.zeros((h, w), dtype=bool)
    disoccluded_union = np.zeros((h, w), dtype=bool)
    failed_components = []
    component_metrics = []

    for match in matches:
        if match["motion_px"] < MIN_COMPONENT_MOTION_PX:
            continue

        comp_mask = labels_after == match["after"]["label_id"]
        shell_mask = comp_mask & ~binary_erosion(
            comp_mask, iterations=SHELL_EROSION_PX)
        core_mask = binary_erosion(
            comp_mask, iterations=CORE_EROSION_PX) & history_mask
        if np.sum(core_mask) < MIN_CORE_PIXELS:
            continue

        motion_dir = np.array([match["dx"], match["dy"]], dtype=np.float32)
        motion_dir /= np.linalg.norm(motion_dir)
        signed = (xx - match["after"]["cx"]) * motion_dir[0] + \
                 (yy - match["after"]["cy"]) * motion_dir[1]

        leading_shell = shell_mask & (signed > 0.0)
        leading_history = leading_shell & history_mask
        leading_disoccluded = leading_shell & disoccluded_mask
        if np.sum(leading_history) < MIN_LEADING_HISTORY_PIXELS:
            continue

        rgb_threshold = max(
            MIN_RGB_BAD_THRESHOLD,
            float(np.quantile(rgb_error[core_mask], 0.95)) + RGB_CORE_BAD_MARGIN)
        luma_threshold = max(
            MIN_LUMA_BAD_THRESHOLD,
            float(np.quantile(luma_error[core_mask], 0.95)) + LUMA_CORE_BAD_MARGIN)
        bad_pixels = (rgb_error > rgb_threshold) | (
            luma_error > luma_threshold)
        bad_leading_history = bad_pixels & leading_history

        ys, xs = np.where(leading_history)
        rel_angle = np.arctan2(ys - match["after"]["cy"], xs - match["after"]["cx"]) - \
            np.arctan2(motion_dir[1], motion_dir[0])
        rel_angle = (rel_angle + np.pi) % (2.0 * np.pi) - np.pi
        bin_indices = np.floor((rel_angle + np.pi) /
                               (2.0 * np.pi) * ARC_BIN_COUNT).astype(int)
        bin_indices = np.clip(bin_indices, 0, ARC_BIN_COUNT - 1)

        occupied_bins = np.zeros(ARC_BIN_COUNT, dtype=bool)
        bad_fraction_per_bin = np.zeros(ARC_BIN_COUNT, dtype=np.float32)
        for bin_index in range(ARC_BIN_COUNT):
            bin_mask = bin_indices == bin_index
            if np.sum(bin_mask) < MIN_PIXELS_PER_ARC_BIN:
                continue
            occupied_bins[bin_index] = True
            bad_fraction_per_bin[bin_index] = float(
                np.mean(bad_leading_history[ys[bin_mask], xs[bin_mask]]))

        bad_bins = occupied_bins & (
            bad_fraction_per_bin >= BAD_BIN_FRACTION_THRESHOLD)
        longest_bad_arc = compute_longest_circular_run(bad_bins)
        failed = longest_bad_arc > MAX_BAD_ARC_RUN_BINS

        component_metrics.append({
            "component_id": match["after"]["label_id"],
            "area": match["after"]["area"],
            "dx": match["dx"],
            "dy": match["dy"],
            "motion_px": match["motion_px"],
            "leading_history_pixels": int(np.sum(leading_history)),
            "leading_disoccluded_pixels": int(np.sum(leading_disoccluded)),
            "core_pixels": int(np.sum(core_mask)),
            "rgb_threshold": rgb_threshold,
            "luma_threshold": luma_threshold,
            "bad_fraction": float(np.mean(bad_leading_history[leading_history])),
            "max_bad_bin_fraction": float(np.max(bad_fraction_per_bin)),
            "bad_bins": int(np.sum(bad_bins)),
            "longest_bad_arc": int(longest_bad_arc),
            "failed": failed,
            "leading_history": leading_history,
            "leading_disoccluded": leading_disoccluded,
            "bad_leading_history": bad_leading_history,
        })

        leading_union |= leading_history
        disoccluded_union |= leading_disoccluded
        if failed:
            failing_union |= bad_leading_history
            failed_components.append(match["after"]["label_id"])

    print("\n  Semantic object-shell analysis:")
    print(f"    before components: {len(before_components)}")
    print(f"    after components:  {len(after_components)}")
    print(f"    matched:           {len(matches)}")
    for metric in component_metrics:
        status = "FAIL" if metric["failed"] else "PASS"
        print(f"    comp {metric['component_id']:>2d} "
              f"area={metric['area']:>6d} "
              f"motion=({metric['dx']:+5.1f}, {metric['dy']:+4.1f}) "
              f"lead_hist={metric['leading_history_pixels']:>4d} "
              f"lead_new={metric['leading_disoccluded_pixels']:>3d} "
              f"bad_frac={metric['bad_fraction']:.3f} "
              f"bad_arc={metric['longest_bad_arc']:>2d} bins "
              f"max_bin={metric['max_bad_bin_fraction']:.2f} "
              f"[rgb>{metric['rgb_threshold']:.3f}, "
              f"luma>{metric['luma_threshold']:.3f}] "
              f"{status}")

    failures = []
    if len(component_metrics) < MIN_ANALYZED_COMPONENTS:
        failures.append(
            f"only {len(component_metrics)} components analyzed; "
            f"need >= {MIN_ANALYZED_COMPONENTS}")
    if len(failed_components) > MAX_FAILED_COMPONENTS:
        failures.append(
            f"{len(failed_components)} components have a visible wrong-side shell arc; "
            f"max allowed is {MAX_FAILED_COMPONENTS}")

    return {
        "ok": not failures,
        "reason": "; ".join(failures),
        "metrics": component_metrics,
        "failing_union": failing_union,
        "leading_union": leading_union,
        "disoccluded_union": disoccluded_union,
        "eval_img": eval_img,
        "rgb_error": rgb_error,
        "failed_components": failed_components,
    }


def save_diagnostics(artifact_dir, analysis):
    overlay = np.clip(analysis["eval_img"], 0, 1)
    overlay = (overlay * 255).astype(np.uint8)
    overlay[analysis["disoccluded_union"]] = [0, 128, 255]
    overlay[analysis["leading_union"]] = [255, 255, 0]
    overlay[analysis["failing_union"]] = [255, 0, 0]
    Image.fromarray(overlay).save(
        os.path.join(artifact_dir, "diag_run1_semantic_overlay.png"))

    error_vis = np.clip(analysis["rgb_error"] / 0.12, 0, 1)
    heatmap = np.zeros_like(overlay)
    heatmap[:, :, 0] = (error_vis * 255).astype(np.uint8)
    heatmap[:, :, 2] = ((1.0 - error_vis) * 64).astype(np.uint8)
    heatmap[analysis["leading_union"]] = [255, 220, 0]
    heatmap[analysis["failing_union"]] = [255, 0, 0]
    Image.fromarray(heatmap).save(
        os.path.join(artifact_dir, "diag_run1_semantic_error.png"))


def find_existing_artifact(paths, label):
    for path in paths:
        if os.path.exists(path):
            return path
    raise FileNotFoundError(
        f"{label} not found. Looked in: {', '.join(paths)}")


def get_archived_paths(screenshot_dir, artifact_dir):
    motion_side_dir = os.path.join(screenshot_dir, "motion_side_debug")
    return {
        "vanilla_after": find_existing_artifact([
            os.path.join(artifact_dir, "run1_semantic_vanilla_after.png"),
            os.path.join(motion_side_dir, "motion_side_vanilla_after.png"),
        ], "vanilla after"),
        "e2e_after": find_existing_artifact([
            os.path.join(artifact_dir, "run1_semantic_e2e_after.png"),
            os.path.join(motion_side_dir, "motion_side_e2e_after.png"),
            os.path.join(screenshot_dir, "reblur_e2e_after.png"),
        ], "Run 1 e2e after"),
        "material_before": find_existing_artifact([
            os.path.join(artifact_dir, "run1_semantic_materialid_before.png"),
            os.path.join(motion_side_dir, "motion_side_materialid_before.png"),
        ], "material id before"),
        "material_after": find_existing_artifact([
            os.path.join(artifact_dir, "run1_semantic_materialid_after.png"),
            os.path.join(motion_side_dir, "motion_side_materialid_after.png"),
        ], "material id after"),
        "disocclusion_after": find_existing_artifact([
            os.path.join(artifact_dir, "run1_semantic_disocclusion_after.png"),
            os.path.join(motion_side_dir,
                         "motion_side_disocclusion_after.png"),
        ], "disocclusion after"),
    }


def main():
    args, extra_args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)
    artifact_dir = prepare_artifact_dir(screenshot_dir, args.analyze_only)

    print("=" * 70)
    print("  Semantic Run 1 End-to-End Regression")
    print("=" * 70)

    if not args.analyze_only and not args.skip_build:
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

    if args.analyze_only:
        archived = get_archived_paths(screenshot_dir, artifact_dir)
    else:
        direct_run = args.skip_build and fw in ("macos", "glfw")

        print(f"\n{'-' * 70}")
        print("  Run 0: Vanilla baseline")
        print(f"{'-' * 70}")
        ok = run_app(py, build_py, fw, "vanilla_converged_baseline",
                     extra_args, "vanilla", clear_screenshots=True, direct_run=direct_run)
        if not ok:
            return 1
        _, vanilla_after_path = archive_run_pair(
            screenshot_dir, artifact_dir,
            "*vanilla_baseline_before*",
            "*vanilla_baseline_after*",
            "run1_semantic_vanilla")
        if not vanilla_after_path:
            print("FAIL: vanilla screenshots not found")
            return 1

        print(f"\n{'-' * 70}")
        print("  Run 1: REBLUR full pipeline")
        print(f"{'-' * 70}")
        ok = run_app(py, build_py, fw, "reblur_converged_history",
                     extra_args, "full pipeline", clear_screenshots=True,
                     direct_run=direct_run)
        if not ok:
            return 1
        _, e2e_after_path = archive_run_pair(
            screenshot_dir, artifact_dir,
            "*converged_history_before*",
            "*converged_history_after*",
            "run1_semantic_e2e")
        if not e2e_after_path:
            print("FAIL: Run 1 screenshots not found")
            return 1

        print(f"\n{'-' * 70}")
        print("  Run 2: TADisocclusion")
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
            "run1_semantic_disocclusion")
        if not disocclusion_after_path:
            print("FAIL: disocclusion screenshot not found")
            return 1

        print(f"\n{'-' * 70}")
        print("  Run 3: TAMaterialId")
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
            "run1_semantic_materialid")
        if not material_before_path or not material_after_path:
            print("FAIL: material-id screenshots not found")
            return 1

        archived = {
            "vanilla_after": vanilla_after_path,
            "e2e_after": e2e_after_path,
            "material_before": material_before_path,
            "material_after": material_after_path,
            "disocclusion_after": disocclusion_after_path,
        }

    print(f"\n{'=' * 70}")
    print("  Semantic Analysis: Wrong-Side Shell Arcs")
    print(f"{'=' * 70}")

    analysis = analyze_semantic_run1(
        archived["e2e_after"],
        archived["vanilla_after"],
        archived["material_before"],
        archived["material_after"],
        archived["disocclusion_after"])
    save_diagnostics(artifact_dir, analysis)

    if analysis["ok"]:
        print("\nPASS: Run 1 history-valid leading shells stay visually stable")
        return 0

    print(f"\nFAIL: {analysis['reason']}")
    print(f"  Diagnostics: {artifact_dir}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
