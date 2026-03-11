#!/usr/bin/env python3
"""Repeated-motion TA quality-delta diagnostic for REBLUR.

This diagnostic asks a narrower question than the motion-regime test:

On the contaminated leading shells, is the remaining TA trust problem caused by
`spec_history_quality` diverging from `footprintQuality`, or because
`footprintQuality` itself stays too generous even when the full reprojection
footprint is not valid?
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
from ghosting_harness import run_ghosting_app

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
DELTA_SCALE = 16.0


def parse_args():
    parser = argparse.ArgumentParser(
        description="Repeated-motion TA quality-delta diagnostic")
    parser.add_argument("--framework", default="macos",
                        choices=("glfw", "macos"))
    parser.add_argument("--eval_debug_pass", default="TemporalAccumSpecular")
    parser.add_argument("--skip_build", action="store_true")
    return parser.parse_known_args()


def get_screenshot_dir(framework):
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    return os.path.join(PROJECT_ROOT, "build_system", "glfw", "output",
                        "build", "generated", "screenshots")


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



def prepare_artifact_dir(screenshot_dir):
    artifact_dir = os.path.join(
        screenshot_dir, "repeated_motion_ta_quality_delta_debug")
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


def analyze_nudge(index, fast_path, settled_path, delta_path, disocclusion_path,
                  prev_material_path, current_material_path):
    _, fast_luma = load_luminance(fast_path)
    _, settled_luma = load_luminance(settled_path)
    fast_hf = compute_hf_residual(fast_luma)
    settled_hf = compute_hf_residual(settled_luma)
    delta = load_numeric_image(delta_path)
    history_mask = load_history_mask(disocclusion_path)

    footprint_deficit = delta[:, :, 0] / DELTA_SCALE
    quality_deficit = delta[:, :, 1] / DELTA_SCALE
    full_valid = delta[:, :, 2]

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
    contaminated_components = []

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
        trailing_shell = shell_mask & (signed < 0.0)
        leading_history = leading_shell & history_mask
        trailing_history = trailing_shell & history_mask

        if np.sum(leading_history) < MIN_REGION_PIXELS:
            continue
        if np.sum(trailing_history) < MIN_REGION_PIXELS:
            continue

        lead_ratio = float(
            np.mean(fast_hf[leading_history]) /
            max(np.mean(settled_hf[leading_history]), 1e-9))

        if lead_ratio <= MIN_CONTAMINATED_RATIO:
            continue

        contaminated_components.append({
            "lead_ratio": lead_ratio,
            "lead_footprint_deficit": float(np.mean(footprint_deficit[leading_history])),
            "trail_footprint_deficit": float(np.mean(footprint_deficit[trailing_history])),
            "lead_quality_deficit": float(np.mean(quality_deficit[leading_history])),
            "trail_quality_deficit": float(np.mean(quality_deficit[trailing_history])),
            "lead_full_valid": float(np.mean(full_valid[leading_history] > 0.5)),
            "trail_full_valid": float(np.mean(full_valid[trailing_history] > 0.5)),
        })

    if len(contaminated_components) == 0:
        return {
            "ok": False,
            "reason": f"nudge {index}: no contaminated leading components found",
        }

    contaminated_components.sort(key=lambda m: m["lead_ratio"], reverse=True)
    top_count = min(3, len(contaminated_components))
    top = contaminated_components[:top_count]

    return {
        "ok": True,
        "top_lead_ratio": float(np.mean([m["lead_ratio"] for m in top])),
        "lead_footprint_deficit": float(np.mean([m["lead_footprint_deficit"] for m in top])),
        "trail_footprint_deficit": float(np.mean([m["trail_footprint_deficit"] for m in top])),
        "lead_quality_deficit": float(np.mean([m["lead_quality_deficit"] for m in top])),
        "trail_quality_deficit": float(np.mean([m["trail_quality_deficit"] for m in top])),
        "lead_full_valid": float(np.mean([m["lead_full_valid"] for m in top])),
        "trail_full_valid": float(np.mean([m["trail_full_valid"] for m in top])),
    }


def main():
    args, extra = parse_args()
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(args.framework)
    artifact_dir = prepare_artifact_dir(screenshot_dir)

    print("=" * 70)
    print("  Repeated-Motion TA Quality Delta")
    print("=" * 70)
    print(f"  Eval debug pass: {args.eval_debug_pass}")
    print()

    runs = [
        ("eval", ["--reblur_no_pt_blend", "true",
                  "--reblur_debug_pass", args.eval_debug_pass], True),
        ("delta", ["--reblur_debug_pass", "TASpecQualityDelta"], True),
        ("disocclusion", ["--reblur_debug_pass", "TADisocclusion"], True),
        ("material", ["--reblur_debug_pass", "TAMaterialId"], True),
    ]

    archived = {}
    for label_name, run_args, clear in runs:
        print("-" * 70)
        print(f"  Run: {label_name}")
        print("-" * 70)
        if not run_ghosting_app(py, build_py, PROJECT_ROOT, args.framework, run_args + extra,
                       label_name, clear_screenshots=clear):
            return 1
        archived[label_name] = archive_ghosting_shots(screenshot_dir, artifact_dir, label_name)
        print()

    results = []
    failures = []
    print("=" * 70)
    print("  Analysis: contaminated-shell footprint vs quality deficits")
    print("=" * 70)

    for index in range(NUDGE_COUNT):
        result = analyze_nudge(
            index,
            archived["eval"][f"ghosting_nudge_{index}_fast.png"],
            archived["eval"][f"ghosting_nudge_{index}_settled.png"],
            archived["delta"][f"ghosting_nudge_{index}_fast.png"],
            archived["disocclusion"][f"ghosting_nudge_{index}_fast.png"],
            archived["material"]["ghosting_before.png"] if index == 0
            else archived["material"][f"ghosting_nudge_{index - 1}_fast.png"],
            archived["material"][f"ghosting_nudge_{index}_fast.png"],
        )

        if not result["ok"]:
            print(f"  {result['reason']}")
            continue

        results.append(result)
        print(
            f"  Nudge {index}: lead_ratio={result['top_lead_ratio']:.2f}x "
            f"foot_def={result['lead_footprint_deficit']:.4f}/{result['trail_footprint_deficit']:.4f} "
            f"qual_def={result['lead_quality_deficit']:.4f}/{result['trail_quality_deficit']:.4f} "
            f"full_valid={result['lead_full_valid']:.2f}/{result['trail_full_valid']:.2f}"
        )

    print()
    print(f"  Diagnostics saved to {artifact_dir}")

    if len(results) == 0:
        print("  FAIL: no analyzed nudges")
        return 1

    top_count = min(3, len(results))
    top = sorted(results, key=lambda r: r["top_lead_ratio"], reverse=True)[:top_count]
    print(
        f"  Worst-top{top_count}: "
        f"foot_def={np.mean([r['lead_footprint_deficit'] for r in top]):.4f}/"
        f"{np.mean([r['trail_footprint_deficit'] for r in top]):.4f} "
        f"qual_def={np.mean([r['lead_quality_deficit'] for r in top]):.4f}/"
        f"{np.mean([r['trail_quality_deficit'] for r in top]):.4f} "
        f"full_valid={np.mean([r['lead_full_valid'] for r in top]):.2f}/"
        f"{np.mean([r['trail_full_valid'] for r in top]):.2f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

