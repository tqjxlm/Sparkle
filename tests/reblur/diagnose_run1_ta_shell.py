#!/usr/bin/env python3
"""Single-nudge TA shell diagnostic for Run 1 of test_converged_history.py.

This script targets the user-visible macOS symptom:
after a tiny camera yaw, the floor stays roughly stable against vanilla but
object silhouettes remain visibly noisy.

It reuses the exact `reblur_converged_history` capture sequence and archives
the debug passes needed to inspect the history-valid motion-leading shells:
  - Run 1 end-to-end output
  - vanilla reference
  - TADisocclusion
  - TAMaterialId
  - TemporalAccumSpecular
  - TASpecHistory
  - HistoryFixSpecular
  - PostBlurSpecular
  - StabilizedSpecular
  - TASpecAccumSpeed
  - TASpecMotionInputs
  - TASpecSurfaceInputs
  - TSSpecBlend
  - TSSpecAntilagInputs
  - TSSpecClampInputs
  - TAMotionVectorFine

For the top contaminated object shells it reports:
  - visible Run 1 shell noise vs vanilla
  - stage-by-stage specular shell contamination and amplification
  - spec accum / prev accum / history quality
  - full-footprint-valid fraction
  - shell motion regime
  - TS leading/trailing shell state
  - roughness / normalized hit distance / spec magic

Usage:
  python3 tests/reblur/diagnose_run1_ta_shell.py --framework macos [--skip_build]
  python3 tests/reblur/diagnose_run1_ta_shell.py --framework macos --analyze_only
"""

import argparse
import glob
import os
import subprocess
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import binary_erosion, gaussian_filter, label
from reblur_settings import get_default_max_accumulated_frame_num

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, PROJECT_ROOT)

MAX_ACCUMULATED_FRAME_NUM = get_default_max_accumulated_frame_num(PROJECT_ROOT)
MOTION_DEBUG_SCALE = 10.0
MIN_COMPONENT_AREA = 1500
MAX_COMPONENT_MATCH_DISTANCE = 20.0
MIN_COMPONENT_MOTION_PX = 1.0
SHELL_EROSION_PX = 6
MIN_LEADING_HISTORY_PIXELS = 200
TOP_COMPONENT_COUNT = 5
CONTAMINATION_RATIO_MIN = 1.15


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run 1 TA shell diagnostic")
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


def find_screenshot(screenshot_dir, pattern):
    matches = glob.glob(os.path.join(screenshot_dir, pattern))
    if not matches:
        return None
    matches.sort(key=os.path.getmtime, reverse=True)
    return matches[0]


def prepare_artifact_dir(screenshot_dir, analyze_only):
    artifact_dir = os.path.join(screenshot_dir, "run1_ta_shell_debug")
    os.makedirs(artifact_dir, exist_ok=True)
    if not analyze_only:
        for path in glob.glob(os.path.join(artifact_dir, "*.png")):
            os.remove(path)
    return artifact_dir


def load_image(path):
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def srgb_to_linear(image):
    return np.where(image <= 0.04045,
                    image / 12.92,
                    ((image + 0.055) / 1.055) ** 2.4)


def load_numeric_image(path):
    return srgb_to_linear(load_image(path))


def load_luminance(path):
    img = load_image(path)
    luma = img[:, :, 0] * 0.2126 + img[:, :, 1] * \
        0.7152 + img[:, :, 2] * 0.0722
    return img, luma


def compute_hf_residual(luma, sigma=2.0):
    return np.abs(luma - gaussian_filter(luma, sigma=sigma))


def run_app(py, build_py, framework, test_case, extra_args, label,
            clear_screenshots=False):
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
            for line in result.stderr.strip().splitlines()[-8:]:
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


def build_shell_metrics(paths):
    e2e_img, e2e_luma = load_luminance(paths["e2e_after"])
    vanilla_img, vanilla_luma = load_luminance(paths["vanilla_after"])
    ta_spec_img, ta_spec_luma = load_luminance(paths["ta_spec_after"])
    ta_hist_img, ta_hist_luma = load_luminance(paths["ta_history_after"])
    history_fix_img, history_fix_luma = load_luminance(paths["history_fix_after"])
    postblur_img, postblur_luma = load_luminance(paths["postblur_after"])
    stabilized_img, stabilized_luma = load_luminance(paths["stabilized_after"])
    ta_accum = load_numeric_image(paths["ta_accum_after"])
    ta_inputs = load_numeric_image(paths["ta_inputs_after"])
    ts_blend = load_numeric_image(paths["ts_blend_after"])
    ts_antilag = load_numeric_image(paths["ts_antilag_after"])
    ts_clamp = load_numeric_image(paths["ts_clamp_after"])
    motion = load_numeric_image(paths["motion_after"])
    surface = load_numeric_image(paths["surface_after"])
    history_mask = load_history_mask(paths["disocclusion_after"])

    e2e_hf = compute_hf_residual(e2e_luma)
    vanilla_hf = compute_hf_residual(vanilla_luma)
    ta_spec_hf = compute_hf_residual(ta_spec_luma)
    ta_hist_hf = compute_hf_residual(ta_hist_luma)
    history_fix_hf = compute_hf_residual(history_fix_luma)
    postblur_hf = compute_hf_residual(postblur_luma)
    stabilized_hf = compute_hf_residual(stabilized_luma)

    motion_x = (motion[:, :, 0] - 0.5) * MOTION_DEBUG_SCALE
    motion_y = (motion[:, :, 1] - 0.5) * MOTION_DEBUG_SCALE
    motion_px = np.hypot(motion_x, motion_y)

    labels_before, before_components = extract_components(paths["material_before"])
    labels_after, after_components = extract_components(paths["material_after"])
    matches = match_components(before_components, after_components)

    h, w = history_mask.shape
    yy, xx = np.indices((h, w))
    shell_overlay = np.zeros((h, w, 3), dtype=np.uint8)
    shell_overlay[:] = np.clip(e2e_img * 255.0, 0, 255).astype(np.uint8)

    metrics = []
    for match in matches:
        if match["motion_px"] < MIN_COMPONENT_MOTION_PX:
            continue

        comp_mask = labels_after == match["after"]["label_id"]
        shell_mask = comp_mask & ~binary_erosion(
            comp_mask, iterations=SHELL_EROSION_PX)
        if np.sum(shell_mask) < MIN_LEADING_HISTORY_PIXELS * 2:
            continue

        motion_dir = np.array([match["dx"], match["dy"]], dtype=np.float32)
        motion_dir /= np.linalg.norm(motion_dir)
        signed = (xx - match["after"]["cx"]) * motion_dir[0] + \
            (yy - match["after"]["cy"]) * motion_dir[1]

        leading_shell = shell_mask & (signed > 0.0)
        trailing_shell = shell_mask & (signed < 0.0)
        leading_history = leading_shell & history_mask
        trailing_history = trailing_shell & history_mask
        if np.sum(leading_history) < MIN_LEADING_HISTORY_PIXELS:
            continue
        if np.sum(trailing_history) < MIN_LEADING_HISTORY_PIXELS:
            continue

        contamination_ratio = float(
            np.mean(e2e_hf[leading_history]) /
            max(np.mean(vanilla_hf[leading_history]), 1e-9))
        rgb_error = np.mean(
            np.abs(e2e_img[leading_history] - vanilla_img[leading_history]),
            axis=0)
        ta_amplification = float(
            np.mean(ta_spec_hf[leading_history]) /
            max(np.mean(ta_hist_hf[leading_history]), 1e-9))
        history_fix_amplification = float(
            np.mean(history_fix_hf[leading_history]) /
            max(np.mean(ta_spec_hf[leading_history]), 1e-9))
        postblur_amplification = float(
            np.mean(postblur_hf[leading_history]) /
            max(np.mean(history_fix_hf[leading_history]), 1e-9))
        stabilized_amplification = float(
            np.mean(stabilized_hf[leading_history]) /
            max(np.mean(postblur_hf[leading_history]), 1e-9))

        metrics.append({
            "component_id": match["after"]["label_id"],
            "history_pixels": int(np.sum(leading_history)),
            "contamination_ratio": contamination_ratio,
            "rgb_error": rgb_error,
            "ta_amplification": ta_amplification,
            "ta_ratio": float(
                np.mean(ta_spec_hf[leading_history]) /
                max(np.mean(vanilla_hf[leading_history]), 1e-9)),
            "history_fix_ratio": float(
                np.mean(history_fix_hf[leading_history]) /
                max(np.mean(vanilla_hf[leading_history]), 1e-9)),
            "postblur_ratio": float(
                np.mean(postblur_hf[leading_history]) /
                max(np.mean(vanilla_hf[leading_history]), 1e-9)),
            "stabilized_ratio": float(
                np.mean(stabilized_hf[leading_history]) /
                max(np.mean(vanilla_hf[leading_history]), 1e-9)),
            "history_fix_amplification": history_fix_amplification,
            "postblur_amplification": postblur_amplification,
            "stabilized_amplification": stabilized_amplification,
            "spec_accum": float(np.mean(ta_accum[:, :, 0][leading_history]) *
                                 MAX_ACCUMULATED_FRAME_NUM),
            "spec_prev_accum": float(np.mean(ta_accum[:, :, 1][leading_history]) *
                                      MAX_ACCUMULATED_FRAME_NUM),
            "spec_quality": float(np.mean(ta_accum[:, :, 2][leading_history])),
            "full_footprint_fraction": float(
                np.mean(ta_inputs[:, :, 2][leading_history] > 0.5)),
            "motion_median": float(np.median(motion_px[leading_history])),
            "motion_p90": float(np.percentile(motion_px[leading_history], 90)),
            "subpixel_fraction": float(
                np.mean(motion_px[leading_history] < 1.0)),
            "roughness": float(np.mean(surface[:, :, 0][leading_history])),
            "hit_norm": float(np.mean(surface[:, :, 1][leading_history])),
            "spec_magic": float(np.mean(surface[:, :, 2][leading_history])),
            "ts_blend_lead": float(np.mean(ts_blend[:, :, 0][leading_history])),
            "ts_blend_trail": float(np.mean(ts_blend[:, :, 0][trailing_history])),
            "ts_antilag_lead": float(np.mean(ts_blend[:, :, 1][leading_history])),
            "ts_antilag_trail": float(np.mean(ts_blend[:, :, 1][trailing_history])),
            "ts_footprint_lead": float(np.mean(ts_blend[:, :, 2][leading_history])),
            "ts_footprint_trail": float(np.mean(ts_blend[:, :, 2][trailing_history])),
            "ts_divergence_lead": float(np.mean(ts_antilag[:, :, 0][leading_history])),
            "ts_divergence_trail": float(np.mean(ts_antilag[:, :, 0][trailing_history])),
            "ts_incoming_conf_lead": float(np.mean(ts_antilag[:, :, 1][leading_history])),
            "ts_incoming_conf_trail": float(np.mean(ts_antilag[:, :, 1][trailing_history])),
            "ts_outgoing_conf_lead": float(np.mean(ts_antilag[:, :, 2][leading_history])),
            "ts_outgoing_conf_trail": float(np.mean(ts_antilag[:, :, 2][trailing_history])),
            "ts_history_delta_lead": float(np.mean(ts_clamp[:, :, 0][leading_history])),
            "ts_history_delta_trail": float(np.mean(ts_clamp[:, :, 0][trailing_history])),
            "ts_clamp_band_lead": float(np.mean(ts_clamp[:, :, 1][leading_history])),
            "ts_clamp_band_trail": float(np.mean(ts_clamp[:, :, 1][trailing_history])),
            "leading_history_mask": leading_history,
        })

    metrics.sort(key=lambda m: m["contamination_ratio"], reverse=True)

    palette = np.array([
        [255, 64, 64],
        [255, 160, 0],
        [255, 255, 0],
        [0, 220, 160],
        [80, 160, 255],
    ], dtype=np.uint8)
    for index, metric in enumerate(metrics[:TOP_COMPONENT_COUNT]):
        shell_overlay[metric["leading_history_mask"]] = palette[index]

    return metrics, shell_overlay


def print_summary(metrics):
    if not metrics:
        print("  No analyzable motion-leading history-valid shells found.")
        return

    top = metrics[:TOP_COMPONENT_COUNT]
    contaminated = [m for m in metrics
                    if m["contamination_ratio"] >= CONTAMINATION_RATIO_MIN]

    print("\nTop contaminated Run 1 shells:")
    for index, metric in enumerate(top, start=1):
        rgb_error = metric["rgb_error"]
        print(
            f"  {index}. comp={metric['component_id']} "
            f"hf_ratio={metric['contamination_ratio']:.2f}x "
            f"ta/fix/post/stab="
            f"{metric['ta_ratio']:.2f}/{metric['history_fix_ratio']:.2f}/"
            f"{metric['postblur_ratio']:.2f}/{metric['stabilized_ratio']:.2f}x "
            f"amps="
            f"{metric['ta_amplification']:.2f}/"
            f"{metric['history_fix_amplification']:.2f}/"
            f"{metric['postblur_amplification']:.2f}/"
            f"{metric['stabilized_amplification']:.2f}x "
            f"spec_accum={metric['spec_accum']:.2f} "
            f"prev={metric['spec_prev_accum']:.2f} "
            f"quality={metric['spec_quality']:.3f} "
            f"fullQ={metric['full_footprint_fraction']:.2f} "
            f"motion50={metric['motion_median']:.2f}px "
            f"sub1={metric['subpixel_fraction']:.2f} "
            f"tsBlend={metric['ts_blend_lead']:.2f}/{metric['ts_blend_trail']:.2f} "
            f"tsAnti={metric['ts_antilag_lead']:.2f}/{metric['ts_antilag_trail']:.2f} "
            f"tsInOut={metric['ts_incoming_conf_lead']:.2f}/{metric['ts_incoming_conf_trail']:.2f}|"
            f"{metric['ts_outgoing_conf_lead']:.2f}/{metric['ts_outgoing_conf_trail']:.2f} "
            f"tsDeltaBand={metric['ts_history_delta_lead']:.2f}/{metric['ts_history_delta_trail']:.2f}|"
            f"{metric['ts_clamp_band_lead']:.2f}/{metric['ts_clamp_band_trail']:.2f} "
            f"rough={metric['roughness']:.3f} "
            f"hit={metric['hit_norm']:.3f} "
            f"magic={metric['spec_magic']:.3f} "
            f"rgb=({rgb_error[0]:.3f},{rgb_error[1]:.3f},{rgb_error[2]:.3f}) "
            f"px={metric['history_pixels']}")

    print("\nAggregate:")
    print(f"  analyzed shells: {len(metrics)}")
    print(f"  contaminated shells (hf_ratio >= {CONTAMINATION_RATIO_MIN:.2f}x): {len(contaminated)}")
    print(
        f"  top-{len(top)} mean: "
        f"hf_ratio={np.mean([m['contamination_ratio'] for m in top]):.2f}x, "
        f"ta/fix/post/stab="
        f"{np.mean([m['ta_ratio'] for m in top]):.2f}/"
        f"{np.mean([m['history_fix_ratio'] for m in top]):.2f}/"
        f"{np.mean([m['postblur_ratio'] for m in top]):.2f}/"
        f"{np.mean([m['stabilized_ratio'] for m in top]):.2f}x, "
        f"amps="
        f"{np.mean([m['ta_amplification'] for m in top]):.2f}/"
        f"{np.mean([m['history_fix_amplification'] for m in top]):.2f}/"
        f"{np.mean([m['postblur_amplification'] for m in top]):.2f}/"
        f"{np.mean([m['stabilized_amplification'] for m in top]):.2f}x, "
        f"spec_accum={np.mean([m['spec_accum'] for m in top]):.2f}, "
        f"quality={np.mean([m['spec_quality'] for m in top]):.3f}, "
        f"fullQ={np.mean([m['full_footprint_fraction'] for m in top]):.2f}, "
        f"motion50={np.mean([m['motion_median'] for m in top]):.2f}px, "
        f"sub1={np.mean([m['subpixel_fraction'] for m in top]):.2f}, "
        f"tsBlend={np.mean([m['ts_blend_lead'] for m in top]):.2f}/"
        f"{np.mean([m['ts_blend_trail'] for m in top]):.2f}, "
        f"tsAnti={np.mean([m['ts_antilag_lead'] for m in top]):.2f}/"
        f"{np.mean([m['ts_antilag_trail'] for m in top]):.2f}, "
        f"tsInOut={np.mean([m['ts_incoming_conf_lead'] for m in top]):.2f}/"
        f"{np.mean([m['ts_incoming_conf_trail'] for m in top]):.2f}|"
        f"{np.mean([m['ts_outgoing_conf_lead'] for m in top]):.2f}/"
        f"{np.mean([m['ts_outgoing_conf_trail'] for m in top]):.2f}")


def capture_inputs(py, build_py, fw, extra_args, screenshot_dir, artifact_dir):
    runs = [
        ("vanilla", "vanilla_converged_baseline", [], False),
        ("e2e", "reblur_converged_history", [], True),
        ("disocclusion", "reblur_converged_history",
         ["--reblur_debug_pass", "TADisocclusion"], True),
        ("material", "reblur_converged_history",
         ["--reblur_debug_pass", "TAMaterialId"], True),
        ("ta_spec", "reblur_converged_history",
         ["--reblur_debug_pass", "TemporalAccumSpecular"], True),
        ("ta_history", "reblur_converged_history",
         ["--reblur_debug_pass", "TASpecHistory"], True),
        ("history_fix", "reblur_converged_history",
         ["--reblur_debug_pass", "HistoryFixSpecular"], True),
        ("postblur", "reblur_converged_history",
         ["--reblur_debug_pass", "PostBlurSpecular"], True),
        ("stabilized", "reblur_converged_history",
         ["--reblur_debug_pass", "StabilizedSpecular"], True),
        ("ta_accum", "reblur_converged_history",
         ["--reblur_debug_pass", "TASpecAccumSpeed"], True),
        ("ta_inputs", "reblur_converged_history",
         ["--reblur_debug_pass", "TASpecMotionInputs"], True),
        ("ts_blend", "reblur_converged_history",
         ["--reblur_debug_pass", "TSSpecBlend"], True),
        ("ts_antilag", "reblur_converged_history",
         ["--reblur_debug_pass", "TSSpecAntilagInputs"], True),
        ("ts_clamp", "reblur_converged_history",
         ["--reblur_debug_pass", "TSSpecClampInputs"], True),
        ("surface", "reblur_converged_history",
         ["--reblur_debug_pass", "TASpecSurfaceInputs"], True),
        ("motion", "reblur_converged_history",
         ["--reblur_debug_pass", "TAMotionVectorFine"], True),
    ]

    archived = {}
    for index, (prefix, test_case, flags, clear) in enumerate(runs):
        print(f"\n{'—' * 60}")
        print(f"  Capture {index}: {prefix}")
        print(f"{'—' * 60}")
        ok = run_app(py, build_py, fw, test_case, flags + extra_args, prefix,
                     clear_screenshots=clear)
        if not ok:
            return None

        before_pattern = "*vanilla_baseline_before*" if prefix == "vanilla" else "*converged_history_before*"
        after_pattern = "*vanilla_baseline_after*" if prefix == "vanilla" else "*converged_history_after*"
        before_path, after_path = archive_run_pair(
            screenshot_dir, artifact_dir, before_pattern, after_pattern, prefix)
        if not before_path or not after_path:
            print(f"  FAIL: missing screenshots for {prefix}")
            return None

        archived[f"{prefix}_before"] = before_path
        archived[f"{prefix}_after"] = after_path

    return archived


def load_archived_paths(artifact_dir):
    keys = (
        "vanilla_before", "vanilla_after",
        "e2e_before", "e2e_after",
        "disocclusion_before", "disocclusion_after",
        "material_before", "material_after",
        "ta_spec_before", "ta_spec_after",
        "ta_history_before", "ta_history_after",
        "history_fix_before", "history_fix_after",
        "postblur_before", "postblur_after",
        "stabilized_before", "stabilized_after",
        "ta_accum_before", "ta_accum_after",
        "ta_inputs_before", "ta_inputs_after",
        "ts_blend_before", "ts_blend_after",
        "ts_antilag_before", "ts_antilag_after",
        "ts_clamp_before", "ts_clamp_after",
        "surface_before", "surface_after",
        "motion_before", "motion_after",
    )
    paths = {}
    for key in keys:
        path = os.path.join(artifact_dir, f"{key}.png")
        if not os.path.exists(path):
            print(f"  MISSING: {path}")
            return None
        paths[key] = path
    return paths


def main():
    args, extra_args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)
    artifact_dir = prepare_artifact_dir(screenshot_dir, args.analyze_only)

    print("=" * 60)
    print("  Run 1 TA Shell Diagnostic")
    print("=" * 60)

    if not args.skip_build and not args.analyze_only:
        print("\nBuilding...")
        result = subprocess.run([py, build_py, "--framework", fw] + extra_args,
                                cwd=PROJECT_ROOT, capture_output=True, text=True)
        if result.returncode != 0:
            print("FAIL: build failed")
            if result.stderr:
                for line in result.stderr.strip().splitlines()[-8:]:
                    print(f"  {line}")
            return 1

    if args.analyze_only:
        paths = load_archived_paths(artifact_dir)
        if paths is None:
            return 1
    else:
        paths = capture_inputs(py, build_py, fw, extra_args,
                               screenshot_dir, artifact_dir)
        if paths is None:
            return 1

    metrics, shell_overlay = build_shell_metrics(paths)
    overlay_path = os.path.join(artifact_dir, "diag_run1_ta_shell_overlay.png")
    Image.fromarray(shell_overlay).save(overlay_path)

    print_summary(metrics)
    print(f"\nSaved overlay: {overlay_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
