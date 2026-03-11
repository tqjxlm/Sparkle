#!/usr/bin/env python3
"""Repeated-motion TA confidence regression for REBLUR.

This test reuses the repeated camera-nudge ghosting sequence and asks a more
specific question than the output-only contamination regression:

Do the history-valid motion-leading shells that still look contaminated also
retain large temporal-accumulation confidence?

That is the behavior the upstream NRD implementation tries hard to avoid by
modulating history length with footprint / motion confidence instead of using a
binary valid-invalid test alone.
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import binary_erosion, gaussian_filter, label

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, PROJECT_ROOT)

NUDGE_COUNT = 5
MIN_COMPONENT_AREA = 1500
MIN_COMPONENT_MOTION_PX = 1.0
MIN_REGION_PIXELS = 200
SHELL_EROSION_PX = 6
MAX_COMPONENT_MATCH_DISTANCE = 24.0
MIN_ANALYZED_COMPONENTS = 4

MIN_CONTAMINATED_RATIO = 1.10
MIN_FOOTPRINT_QUALITY = 0.85
MAX_TOP3_CONTAMINATED_LEAD_ACCUM = 8.0
MAX_ACCUMULATED_FRAME_NUM = 30.0


def parse_args():
    parser = argparse.ArgumentParser(
        description="Repeated-motion TA confidence regression")
    parser.add_argument("--framework", default="macos",
                        choices=("glfw", "macos"))
    parser.add_argument("--eval_debug_pass", default="Full",
                        help="Debug pass to evaluate for the denoised run")
    parser.add_argument("--ta_channel", default="diffuse",
                        choices=("diffuse", "specular"),
                        help="Which TA accum debug channel to analyze")
    parser.add_argument("--skip_build", action="store_true")
    return parser.parse_known_args()


def get_screenshot_dir(framework):
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    return os.path.join(PROJECT_ROOT, "build_system", "glfw", "output",
                        "build", "generated", "screenshots")


def load_image(path):
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def load_luminance(path):
    img = load_image(path)
    luma = img[:, :, 0] * 0.2126 + img[:, :, 1] * \
        0.7152 + img[:, :, 2] * 0.0722
    return img, luma


def compute_hf_residual(luma, sigma=2.0):
    return np.abs(luma - gaussian_filter(luma, sigma=sigma))


def run_app(py, build_py, framework, extra_args, label, clear_screenshots=False):
    cmd = [py, build_py, "--framework", framework, "--skip_build",
           "--run", "--test_case", "reblur_ghosting", "--headless", "true"]
    if clear_screenshots:
        cmd += ["--clear_screenshots", "true"]
    cmd += extra_args
    print(f"  cmd: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=PROJECT_ROOT,
                            capture_output=True, text=True, timeout=900)
    if result.returncode != 0:
        print(f"  FAIL: {label} exited with code {result.returncode}")
        if result.stderr:
            for line in result.stderr.strip().splitlines()[-5:]:
                print(f"    {line}")
        return False
    return True


def prepare_artifact_dir(screenshot_dir):
    artifact_dir = os.path.join(screenshot_dir, "repeated_motion_ta_debug")
    os.makedirs(artifact_dir, exist_ok=True)
    for path in glob.glob(os.path.join(artifact_dir, "*.png")):
        os.remove(path)
    return artifact_dir


def archive_ghosting_shots(screenshot_dir, artifact_dir, prefix):
    paths = {}
    for path in glob.glob(os.path.join(screenshot_dir, "ghosting_*.png")):
        name = os.path.basename(path)
        dest = os.path.join(artifact_dir, f"{prefix}_{name}")
        shutil.move(path, dest)
        paths[name] = dest
    return paths


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


def analyze_nudge(index, fast_path, settled_path, disocclusion_path,
                  prev_material_path, current_material_path, ta_accum_path):
    fast_img, fast_luma = load_luminance(fast_path)
    _, settled_luma = load_luminance(settled_path)
    fast_hf = compute_hf_residual(fast_luma)
    settled_hf = compute_hf_residual(settled_luma)
    history_mask = load_history_mask(disocclusion_path)
    ta_accum = load_image(ta_accum_path)

    labels_prev, prev_components = extract_components(prev_material_path)
    labels_curr, curr_components = extract_components(current_material_path)
    matches = match_components(prev_components, curr_components)

    if len(matches) < MIN_ANALYZED_COMPONENTS:
        return {
            "ok": False,
            "reason": (f"nudge {index}: only {len(matches)} matched components, "
                       f"need >= {MIN_ANALYZED_COMPONENTS}"),
        }

    h, w = history_mask.shape
    yy, xx = np.indices((h, w))
    leading_union = np.zeros((h, w), dtype=bool)
    contaminated_union = np.zeros((h, w), dtype=bool)
    component_metrics = []

    for match in matches:
        if match["motion_px"] < MIN_COMPONENT_MOTION_PX:
            continue

        comp_mask = labels_curr == match["after"]["label_id"]
        shell_mask = comp_mask & ~binary_erosion(
            comp_mask, iterations=SHELL_EROSION_PX)
        if np.sum(shell_mask) < MIN_REGION_PIXELS * 2:
            continue

        motion_dir = np.array([match["dx"], match["dy"]], dtype=np.float32)
        motion_dir /= np.linalg.norm(motion_dir)
        signed = (xx - match["after"]["cx"]) * motion_dir[0] + \
                 (yy - match["after"]["cy"]) * motion_dir[1]

        leading_shell = shell_mask & (signed > 0.0)
        leading_history = leading_shell & history_mask

        if np.sum(leading_history) < MIN_REGION_PIXELS:
            continue

        lead_ratio = float(
            np.mean(fast_hf[leading_history]) /
            max(np.mean(settled_hf[leading_history]), 1e-9))
        lead_accum = float(np.mean(ta_accum[:, :, 0][leading_history]) *
                           MAX_ACCUMULATED_FRAME_NUM)
        lead_prev_accum = float(np.mean(ta_accum[:, :, 1][leading_history]) *
                                MAX_ACCUMULATED_FRAME_NUM)
        lead_quality = float(np.mean(ta_accum[:, :, 2][leading_history]))

        metric = {
            "component_id": match["after"]["label_id"],
            "motion_px": match["motion_px"],
            "lead_ratio": lead_ratio,
            "lead_accum": lead_accum,
            "lead_prev_accum": lead_prev_accum,
            "lead_quality": lead_quality,
            "leading_history": leading_history,
        }
        component_metrics.append(metric)
        leading_union |= leading_history
        if lead_ratio > MIN_CONTAMINATED_RATIO:
            contaminated_union |= leading_history

    if len(component_metrics) < MIN_ANALYZED_COMPONENTS:
        return {
            "ok": False,
            "reason": (f"nudge {index}: only {len(component_metrics)} analyzed components, "
                       f"need >= {MIN_ANALYZED_COMPONENTS}"),
        }

    contaminated = [
        m for m in component_metrics
        if m["lead_ratio"] > MIN_CONTAMINATED_RATIO and
        m["lead_quality"] >= MIN_FOOTPRINT_QUALITY
    ]
    contaminated.sort(key=lambda m: m["lead_ratio"], reverse=True)

    if not contaminated:
        return {
            "ok": True,
            "reason": "",
            "contaminated_count": 0,
            "top3_contaminated_accum": 0.0,
            "worst_ratio": max(m["lead_ratio"] for m in component_metrics),
            "leading_union": leading_union,
            "contaminated_union": contaminated_union,
            "fast_img": fast_img,
            "ta_accum": ta_accum,
        }

    top_count = min(3, len(contaminated))
    top3_contaminated_accum = float(np.mean(
        [m["lead_accum"] for m in contaminated[:top_count]]))
    top3_contaminated_prev_accum = float(np.mean(
        [m["lead_prev_accum"] for m in contaminated[:top_count]]))
    top3_contaminated_quality = float(np.mean(
        [m["lead_quality"] for m in contaminated[:top_count]]))

    failures = []
    if top3_contaminated_accum > MAX_TOP3_CONTAMINATED_LEAD_ACCUM:
        failures.append(
            f"top-{top_count} contaminated leading accum {top3_contaminated_accum:.2f} > "
            f"{MAX_TOP3_CONTAMINATED_LEAD_ACCUM:.2f}")

    return {
        "ok": not failures,
        "reason": "; ".join(failures),
        "contaminated_count": len(contaminated),
        "top3_contaminated_accum": top3_contaminated_accum,
        "top3_contaminated_prev_accum": top3_contaminated_prev_accum,
        "top3_contaminated_quality": top3_contaminated_quality,
        "worst_ratio": contaminated[0]["lead_ratio"],
        "leading_union": leading_union,
        "contaminated_union": contaminated_union,
        "fast_img": fast_img,
        "ta_accum": ta_accum,
    }


def save_diagnostics(artifact_dir, index, analysis):
    overlay = np.clip(analysis["fast_img"], 0, 1)
    overlay = (overlay * 255).astype(np.uint8)
    overlay[analysis["leading_union"]] = [255, 96, 0]
    overlay[analysis["contaminated_union"]] = [255, 0, 255]
    Image.fromarray(overlay).save(
        os.path.join(artifact_dir, f"diag_repeated_motion_ta_overlay_{index}.png"))

    accum_norm = np.clip(analysis["ta_accum"][:, :, 0], 0, 1)
    heatmap = np.zeros_like(overlay)
    heatmap[:, :, 0] = (accum_norm * 255).astype(np.uint8)
    heatmap[:, :, 1] = ((1.0 - accum_norm) * 96).astype(np.uint8)
    heatmap[analysis["leading_union"]] = [255, 128, 0]
    heatmap[analysis["contaminated_union"]] = [255, 0, 255]
    Image.fromarray(heatmap).save(
        os.path.join(artifact_dir, f"diag_repeated_motion_ta_accum_{index}.png"))


def main():
    args, extra_args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)
    artifact_dir = prepare_artifact_dir(screenshot_dir)

    print("=" * 70)
    print("  Repeated-Motion TA Confidence Regression")
    print("=" * 70)
    print(f"  Eval debug pass: {args.eval_debug_pass}")
    print(f"  TA channel:      {args.ta_channel}")

    if not args.skip_build:
        print("\nBuilding...")
        result = subprocess.run([py, build_py, "--framework", fw] + extra_args,
                                cwd=PROJECT_ROOT, capture_output=True,
                                text=True, timeout=900)
        if result.returncode != 0:
            print("FAIL: build failed")
            if result.stderr:
                for line in result.stderr.strip().splitlines()[-10:]:
                    print(f"  {line}")
            return 1

    eval_flags = ["--reblur_no_pt_blend", "true"]
    if args.eval_debug_pass != "Full":
        eval_flags += ["--reblur_debug_pass", args.eval_debug_pass]

    ta_debug_pass = "TAAccumSpeed" if args.ta_channel == "diffuse" else "TASpecAccumSpeed"

    runs = [
        ("denoised", eval_flags),
        ("disocclusion", ["--reblur_debug_pass", "TADisocclusion"]),
        ("material", ["--reblur_debug_pass", "TAMaterialId"]),
        ("ta_accum", ["--reblur_debug_pass", ta_debug_pass]),
    ]
    archived = {}

    for prefix, flags in runs:
        print(f"\n{'-' * 70}")
        print(f"  Run: {prefix}")
        print(f"{'-' * 70}")
        ok = run_app(py, build_py, fw, flags + list(extra_args),
                     prefix, clear_screenshots=True)
        if not ok:
            return 1
        archived[prefix] = archive_ghosting_shots(screenshot_dir, artifact_dir,
                                                  f"{prefix}")

    print(f"\n{'=' * 70}")
    print("  Analysis: contaminated leading shells vs TA accumulation")
    print(f"{'=' * 70}")

    any_failures = False
    for index in range(NUDGE_COUNT):
        prev_name = "ghosting_before.png" if index == 0 else \
            f"ghosting_nudge_{index - 1}_settled.png"
        current_name = f"ghosting_nudge_{index}_fast.png"
        settled_name = f"ghosting_nudge_{index}_settled.png"

        analysis = analyze_nudge(
            index,
            archived["denoised"][current_name],
            archived["denoised"][settled_name],
            archived["disocclusion"][current_name],
            archived["material"][prev_name],
            archived["material"][current_name],
            archived["ta_accum"][current_name],
        )
        if not analysis["ok"]:
            print(f"  Nudge {index}: FAIL - {analysis['reason']}")
            any_failures = True
        else:
            print(
                f"  Nudge {index}: contaminated={analysis['contaminated_count']} "
                f"worst_ratio={analysis['worst_ratio']:.2f}x "
                f"top3_accum={analysis['top3_contaminated_accum']:.2f} "
                f"top3_prev={analysis.get('top3_contaminated_prev_accum', 0.0):.2f} "
                f"quality={analysis.get('top3_contaminated_quality', 0.0):.3f}"
            )

        save_diagnostics(artifact_dir, index, analysis)

    print(f"\n  Diagnostics saved to {artifact_dir}")

    if any_failures:
        print("  FAIL: contaminated motion-leading shells keep too much TA accumulation")
        return 1

    print("  PASS: contaminated shells do not retain excessive TA accumulation")
    return 0


if __name__ == "__main__":
    sys.exit(main())
