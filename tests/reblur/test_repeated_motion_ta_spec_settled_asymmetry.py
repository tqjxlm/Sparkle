#!/usr/bin/env python3
"""Repeated-motion settled TA specular confidence asymmetry diagnostic.

Runs the repeated camera-nudge sequence with the `TASpecAccumSpeed` debug pass
and measures settled-frame specular TA confidence on history-valid motion-
leading and trailing shell pixels.

Question:
Does TA itself preserve materially more specular confidence on the leading side
before TS ever consumes that state?
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import binary_erosion, label
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
MAX_ACCUMULATED_FRAME_NUM = 30.0

MAX_SETTLED_SPEC_TA_ACCUM_LEAD_TRAIL_ASYMMETRY = 1.08
MAX_SETTLED_SPEC_TA_HISTORY_QUALITY_LEAD_TRAIL_ASYMMETRY = 1.05


def parse_args():
    parser = argparse.ArgumentParser(
        description="Repeated-motion settled TA specular confidence asymmetry")
    parser.add_argument("--framework", default="macos",
                        choices=("glfw", "macos"))
    parser.add_argument("--skip_build", action="store_true")
    return parser.parse_known_args()


def get_screenshot_dir(framework):
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    return os.path.join(PROJECT_ROOT, "build_system", "glfw", "output",
                        "build", "generated", "screenshots")


def load_image(path):
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0



def prepare_artifact_dir(screenshot_dir):
    artifact_dir = os.path.join(
        screenshot_dir, "repeated_motion_ta_spec_settled_debug")
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


def analyze_nudge(index, signal_path, disocclusion_path,
                  prev_material_path, current_material_path):
    signal = load_image(signal_path)
    current_accum = signal[:, :, 0] * MAX_ACCUMULATED_FRAME_NUM
    prev_accum = signal[:, :, 1] * MAX_ACCUMULATED_FRAME_NUM
    history_quality = signal[:, :, 2]
    history_mask = load_history_mask(disocclusion_path)

    labels_prev, prev_components = extract_components(prev_material_path)
    labels_curr, curr_components = extract_components(current_material_path)
    matches = match_components(prev_components, curr_components)

    if len(matches) < MIN_ANALYZED_COMPONENTS:
        return {
            "ok": False,
            "hard_error": True,
            "reason": (f"nudge {index}: only {len(matches)} matched components, "
                       f"need >= {MIN_ANALYZED_COMPONENTS}"),
        }

    h, w = history_mask.shape
    yy, xx = np.indices((h, w))
    leading_union = np.zeros((h, w), dtype=bool)
    trailing_union = np.zeros((h, w), dtype=bool)
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
        trailing_shell = shell_mask & (signed < 0.0)
        leading_history = leading_shell & history_mask
        trailing_history = trailing_shell & history_mask

        if np.sum(leading_history) < MIN_REGION_PIXELS:
            continue
        if np.sum(trailing_history) < MIN_REGION_PIXELS:
            continue

        lead_current = float(np.mean(current_accum[leading_history]))
        trail_current = float(np.mean(current_accum[trailing_history]))
        lead_prev = float(np.mean(prev_accum[leading_history]))
        trail_prev = float(np.mean(prev_accum[trailing_history]))
        lead_quality = float(np.mean(history_quality[leading_history]))
        trail_quality = float(np.mean(history_quality[trailing_history]))

        component_metrics.append({
            "lead_current": lead_current,
            "trail_current": trail_current,
            "current_asymmetry": lead_current / max(trail_current, 1e-9),
            "lead_prev": lead_prev,
            "trail_prev": trail_prev,
            "prev_asymmetry": lead_prev / max(trail_prev, 1e-9),
            "lead_quality": lead_quality,
            "trail_quality": trail_quality,
            "quality_asymmetry": lead_quality / max(trail_quality, 1e-9),
            "leading_history": leading_history,
            "trailing_history": trailing_history,
        })

        leading_union |= leading_history
        trailing_union |= trailing_history

    if len(component_metrics) < MIN_ANALYZED_COMPONENTS:
        return {
            "ok": False,
            "hard_error": True,
            "reason": (f"nudge {index}: only {len(component_metrics)} analyzed components, "
                       f"need >= {MIN_ANALYZED_COMPONENTS}"),
        }

    top_count = min(3, len(component_metrics))
    current_asymmetry = np.array([m["current_asymmetry"]
                                 for m in component_metrics], dtype=np.float32)
    prev_asymmetry = np.array([m["prev_asymmetry"]
                              for m in component_metrics], dtype=np.float32)
    quality_asymmetry = np.array([m["quality_asymmetry"]
                                 for m in component_metrics], dtype=np.float32)
    lead_current = np.array([m["lead_current"]
                            for m in component_metrics], dtype=np.float32)
    trail_current = np.array([m["trail_current"]
                             for m in component_metrics], dtype=np.float32)
    lead_prev = np.array([m["lead_prev"]
                         for m in component_metrics], dtype=np.float32)
    trail_prev = np.array([m["trail_prev"]
                          for m in component_metrics], dtype=np.float32)
    lead_quality = np.array([m["lead_quality"]
                            for m in component_metrics], dtype=np.float32)
    trail_quality = np.array([m["trail_quality"]
                             for m in component_metrics], dtype=np.float32)

    worst_current_asymmetry = float(np.mean(np.sort(current_asymmetry)[-top_count:]))
    worst_prev_asymmetry = float(np.mean(np.sort(prev_asymmetry)[-top_count:]))
    worst_quality_asymmetry = float(np.mean(np.sort(quality_asymmetry)[-top_count:]))

    failures = []
    if worst_current_asymmetry > MAX_SETTLED_SPEC_TA_ACCUM_LEAD_TRAIL_ASYMMETRY:
        failures.append(
            f"top-{top_count} settled TA current spec accum lead/trail asymmetry "
            f"{worst_current_asymmetry:.2f} > "
            f"{MAX_SETTLED_SPEC_TA_ACCUM_LEAD_TRAIL_ASYMMETRY:.2f}")
    if worst_quality_asymmetry > MAX_SETTLED_SPEC_TA_HISTORY_QUALITY_LEAD_TRAIL_ASYMMETRY:
        failures.append(
            f"top-{top_count} settled TA spec history-quality lead/trail asymmetry "
            f"{worst_quality_asymmetry:.2f} > "
            f"{MAX_SETTLED_SPEC_TA_HISTORY_QUALITY_LEAD_TRAIL_ASYMMETRY:.2f}")

    return {
        "ok": not failures,
        "hard_error": False,
        "reason": "; ".join(failures),
        "worst_current_asymmetry": worst_current_asymmetry,
        "worst_prev_asymmetry": worst_prev_asymmetry,
        "worst_quality_asymmetry": worst_quality_asymmetry,
        "mean_lead_current": float(np.mean(lead_current)),
        "mean_trail_current": float(np.mean(trail_current)),
        "mean_lead_prev": float(np.mean(lead_prev)),
        "mean_trail_prev": float(np.mean(trail_prev)),
        "mean_lead_quality": float(np.mean(lead_quality)),
        "mean_trail_quality": float(np.mean(trail_quality)),
        "leading_union": leading_union,
        "trailing_union": trailing_union,
        "current_accum": current_accum,
    }


def save_diagnostics(artifact_dir, index, analysis):
    current_accum = np.clip(analysis["current_accum"] / MAX_ACCUMULATED_FRAME_NUM, 0.0, 1.0)
    gray = (current_accum * 255).astype(np.uint8)
    overlay = np.stack([gray, gray, gray], axis=-1)
    overlay[analysis["trailing_union"]] = [0, 255, 0]
    overlay[analysis["leading_union"]] = [255, 0, 0]
    Image.fromarray(overlay).save(
        os.path.join(
            artifact_dir,
            f"diag_repeated_motion_ta_spec_settled_overlay_{index}.png"))


def main():
    args, extra_args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)
    artifact_dir = prepare_artifact_dir(screenshot_dir)

    print("=" * 70)
    print("  Repeated-Motion Settled TA Specular Asymmetry")
    print("=" * 70)

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

    runs = [
        ("signal", ["--reblur_debug_pass", "TASpecAccumSpeed"]),
        ("disocclusion", ["--reblur_debug_pass", "TADisocclusion"]),
        ("material", ["--reblur_debug_pass", "TAMaterialId"]),
    ]
    archived = {}

    for prefix, flags in runs:
        print(f"\n{'-' * 70}")
        print(f"  Run: {prefix}")
        print(f"{'-' * 70}")
        ok = run_ghosting_app(py, build_py, PROJECT_ROOT, fw, flags + list(extra_args),
                     prefix, clear_screenshots=True)
        if not ok:
            return 1
        archived[prefix] = archive_ghosting_shots(screenshot_dir, artifact_dir,
                                                  prefix)

    print(f"\n{'=' * 70}")
    print("  Analysis: settled TA specular confidence asymmetry")
    print(f"{'=' * 70}")

    per_nudge = []
    for index in range(NUDGE_COUNT):
        prev_name = "ghosting_before.png" if index == 0 else \
            f"ghosting_nudge_{index - 1}_settled.png"
        fast_name = f"ghosting_nudge_{index}_fast.png"
        settled_name = f"ghosting_nudge_{index}_settled.png"

        analysis = analyze_nudge(
            index,
            archived["signal"][settled_name],
            archived["disocclusion"][fast_name],
            archived["material"][prev_name],
            archived["material"][fast_name],
        )
        if analysis["hard_error"]:
            print(f"  FAIL: {analysis['reason']}")
            return 1

        per_nudge.append(analysis)
        save_diagnostics(artifact_dir, index, analysis)
        print(
            f"  Nudge {index}: lead_cur={analysis['mean_lead_current']:.2f} "
            f"trail_cur={analysis['mean_trail_current']:.2f} "
            f"cur_lt={analysis['worst_current_asymmetry']:.2f}x "
            f"lead_prev={analysis['mean_lead_prev']:.2f} "
            f"trail_prev={analysis['mean_trail_prev']:.2f} "
            f"prev_lt={analysis['worst_prev_asymmetry']:.2f}x "
            f"lead_q={analysis['mean_lead_quality']:.2f} "
            f"trail_q={analysis['mean_trail_quality']:.2f} "
            f"q_lt={analysis['worst_quality_asymmetry']:.2f}x"
        )

    failures = [entry["reason"] for entry in per_nudge if not entry["ok"]]
    worst_current_asymmetry = max(entry["worst_current_asymmetry"] for entry in per_nudge)
    worst_prev_asymmetry = max(entry["worst_prev_asymmetry"] for entry in per_nudge)
    worst_quality_asymmetry = max(entry["worst_quality_asymmetry"] for entry in per_nudge)

    print(f"\n  Worst settled TA current spec accum asymmetry: {worst_current_asymmetry:.2f}x")
    print(f"  Worst settled TA previous spec accum asymmetry: {worst_prev_asymmetry:.2f}x")
    print(f"  Worst settled TA history-quality asymmetry:    {worst_quality_asymmetry:.2f}x")
    print(f"  Diagnostics saved to {artifact_dir}")
    if failures:
        print(f"  FAIL: {'; '.join(failures)}")
        return 1

    print("  PASS: settled TA spec confidence stays symmetric under repeated motion")
    return 0


if __name__ == "__main__":
    sys.exit(main())

