#!/usr/bin/env python3
"""Repeated-motion TA motion-regime diagnostic for REBLUR.

This test reuses the repeated camera-nudge ghosting sequence and asks a narrow
attribution question:

On the exact history-valid motion-leading shells that still look contaminated,
what TA motion/confidence regime are we in?

It measures:
- fast/settled contamination ratio on leading shells
- per-pixel motion magnitude from `TAMotionVectorFine`
- spec TA confidence / accum length / full-footprint-valid from
  `TASpecMotionInputs`
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
MAX_ACCUMULATED_FRAME_NUM = 30.0
MOTION_DEBUG_SCALE = 10.0


def parse_args():
    parser = argparse.ArgumentParser(
        description="Repeated-motion TA motion-regime diagnostic")
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
        screenshot_dir, "repeated_motion_ta_motion_regime_debug")
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
                  prev_material_path, current_material_path, motion_path,
                  ta_inputs_path):
    _, fast_luma = load_luminance(fast_path)
    _, settled_luma = load_luminance(settled_path)
    fast_hf = compute_hf_residual(fast_luma)
    settled_hf = compute_hf_residual(settled_luma)
    history_mask = load_history_mask(disocclusion_path)
    motion = load_numeric_image(motion_path)
    ta_inputs = load_numeric_image(ta_inputs_path)

    motion_x = (motion[:, :, 0] - 0.5) * MOTION_DEBUG_SCALE
    motion_y = (motion[:, :, 1] - 0.5) * MOTION_DEBUG_SCALE
    motion_px = np.hypot(motion_x, motion_y)
    spec_accum = ta_inputs[:, :, 0] * MAX_ACCUMULATED_FRAME_NUM
    spec_quality = ta_inputs[:, :, 1]
    full_footprint_valid = ta_inputs[:, :, 2]

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
            "component_id": match["after"]["label_id"],
            "object_motion_px": match["motion_px"],
            "lead_ratio": lead_ratio,
            "pixel_motion_median": float(np.median(motion_px[leading_history])),
            "pixel_motion_p90": float(np.percentile(motion_px[leading_history], 90.0)),
            "pixel_sub1_fraction": float(np.mean(motion_px[leading_history] < 1.0)),
            "pixel_sub05_fraction": float(np.mean(motion_px[leading_history] < 0.5)),
            "spec_accum": float(np.mean(spec_accum[leading_history])),
            "spec_quality": float(np.mean(spec_quality[leading_history])),
            "full_valid_fraction": float(np.mean(full_footprint_valid[leading_history] > 0.5)),
            "trail_spec_quality": float(np.mean(spec_quality[trailing_history])),
            "trail_full_valid_fraction": float(np.mean(full_footprint_valid[trailing_history] > 0.5)),
            "partial_full_quality_fraction": float(np.mean(
                (full_footprint_valid[leading_history] <= 0.5) &
                (spec_quality[leading_history] >= 0.95))),
            "pixel_count": int(np.sum(leading_history)),
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
        "top_pixel_motion_median": float(np.mean([m["pixel_motion_median"] for m in top])),
        "top_pixel_motion_p90": float(np.mean([m["pixel_motion_p90"] for m in top])),
        "top_sub1_fraction": float(np.mean([m["pixel_sub1_fraction"] for m in top])),
        "top_sub05_fraction": float(np.mean([m["pixel_sub05_fraction"] for m in top])),
        "top_spec_accum": float(np.mean([m["spec_accum"] for m in top])),
        "top_spec_quality": float(np.mean([m["spec_quality"] for m in top])),
        "top_full_valid_fraction": float(np.mean([m["full_valid_fraction"] for m in top])),
        "top_trail_spec_quality": float(np.mean([m["trail_spec_quality"] for m in top])),
        "top_trail_full_valid_fraction": float(np.mean(
            [m["trail_full_valid_fraction"] for m in top])),
        "top_partial_full_quality_fraction": float(np.mean(
            [m["partial_full_quality_fraction"] for m in top])),
        "top_components": top,
    }


def main():
    args, extra = parse_args()
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(args.framework)
    artifact_dir = prepare_artifact_dir(screenshot_dir)

    print("=" * 70)
    print("  Repeated-Motion TA Motion Regime")
    print("=" * 70)
    print(f"  Eval debug pass: {args.eval_debug_pass}")
    print()

    runs = [
        ("eval", ["--reblur_no_pt_blend", "true",
                  "--reblur_debug_pass", args.eval_debug_pass], True),
        ("motion", ["--reblur_debug_pass", "TAMotionVectorFine"], True),
        ("ta_inputs", ["--reblur_debug_pass", "TASpecMotionInputs"], True),
        ("disocclusion", ["--reblur_debug_pass", "TADisocclusion"], True),
        ("material", ["--reblur_debug_pass", "TAMaterialId"], True),
    ]

    archived = {}
    for label, run_args, clear in runs:
        print("-" * 70)
        print(f"  Run: {label}")
        print("-" * 70)
        if not run_ghosting_app(py, build_py, PROJECT_ROOT, args.framework, run_args + extra,
                       label, clear_screenshots=clear):
            return 1
        archived[label] = archive_ghosting_shots(screenshot_dir, artifact_dir, label)
        print()

    results = []
    hard_failures = []
    print("=" * 70)
    print("  Analysis: contaminated leading-shell TA motion regime")
    print("=" * 70)

    for index in range(NUDGE_COUNT):
        result = analyze_nudge(
            index,
            archived["eval"][f"ghosting_nudge_{index}_fast.png"],
            archived["eval"][f"ghosting_nudge_{index}_settled.png"],
            archived["disocclusion"][f"ghosting_nudge_{index}_fast.png"],
            archived["material"]["ghosting_before.png"] if index == 0
            else archived["material"][f"ghosting_nudge_{index - 1}_fast.png"],
            archived["material"][f"ghosting_nudge_{index}_fast.png"],
            archived["motion"][f"ghosting_nudge_{index}_fast.png"],
            archived["ta_inputs"][f"ghosting_nudge_{index}_fast.png"],
        )

        if not result["ok"]:
            if "no contaminated leading components found" in result["reason"]:
                print(f"  {result['reason']}")
                continue
            hard_failures.append(result["reason"])
            print(f"  {result['reason']}")
            continue

        results.append(result)
        print(
            f"  Nudge {index}: lead_ratio={result['top_lead_ratio']:.2f}x "
            f"motion_med={result['top_pixel_motion_median']:.2f}px "
            f"motion_p90={result['top_pixel_motion_p90']:.2f}px "
            f"sub1={result['top_sub1_fraction']:.2f} "
            f"sub0.5={result['top_sub05_fraction']:.2f} "
            f"spec_accum={result['top_spec_accum']:.2f} "
            f"quality={result['top_spec_quality']:.3f}/{result['top_trail_spec_quality']:.3f} "
            f"full_valid={result['top_full_valid_fraction']:.2f}/{result['top_trail_full_valid_fraction']:.2f} "
            f"partial&fullQ={result['top_partial_full_quality_fraction']:.2f}"
        )

    print()
    print(f"  Diagnostics saved to {artifact_dir}")

    if hard_failures:
        print("  FAIL: " + "; ".join(hard_failures))
        return 1

    if len(results) == 0:
        print("  FAIL: no analyzed nudges")
        return 1

    top_count = min(3, len(results))
    sorted_by_ratio = sorted(results, key=lambda r: r["top_lead_ratio"], reverse=True)
    top = sorted_by_ratio[:top_count]
    print(
        f"  Worst-top{top_count} contaminated shells: "
        f"motion_med={np.mean([r['top_pixel_motion_median'] for r in top]):.2f}px "
        f"motion_p90={np.mean([r['top_pixel_motion_p90'] for r in top]):.2f}px "
        f"sub1={np.mean([r['top_sub1_fraction'] for r in top]):.2f} "
        f"sub0.5={np.mean([r['top_sub05_fraction'] for r in top]):.2f} "
        f"spec_accum={np.mean([r['top_spec_accum'] for r in top]):.2f} "
        f"quality={np.mean([r['top_spec_quality'] for r in top]):.3f}/"
        f"{np.mean([r['top_trail_spec_quality'] for r in top]):.3f} "
        f"full_valid={np.mean([r['top_full_valid_fraction'] for r in top]):.2f}/"
        f"{np.mean([r['top_trail_full_valid_fraction'] for r in top]):.2f} "
        f"partial&fullQ={np.mean([r['top_partial_full_quality_fraction'] for r in top]):.2f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

