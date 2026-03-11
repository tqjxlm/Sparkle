#!/usr/bin/env python3
"""Repeated-motion TS specular TS-state diagnostic for REBLUR.

Runs the repeated camera-nudge sequence with the `TSSpecBlend` debug pass and
measures settled-frame TS specular state on history-valid motion-leading and
trailing shell pixels.

Question:
Does temporal stabilization keep materially different specular state on the
leading side than on the trailing side after the frame has settled?
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

MAX_SETTLED_SPEC_BLEND_LEAD_TRAIL_ASYMMETRY = 1.20
MAX_SETTLED_SPEC_BLEND_LEADING = 0.45
MAX_SETTLED_SPEC_ANTILAG_LEAD_TRAIL_ASYMMETRY = 1.08


def parse_args():
    parser = argparse.ArgumentParser(
        description="Repeated-motion TS specular blend diagnostic")
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
    artifact_dir = os.path.join(screenshot_dir, "repeated_motion_ts_spec_blend_debug")
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
    spec_blend = signal[:, :, 0]
    spec_antilag = signal[:, :, 1]
    spec_footprint = signal[:, :, 2]
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

        lead_mean = float(np.mean(spec_blend[leading_history]))
        trail_mean = float(np.mean(spec_blend[trailing_history]))
        asymmetry = lead_mean / max(trail_mean, 1e-9)
        antilag_lead = float(np.mean(spec_antilag[leading_history]))
        antilag_trail = float(np.mean(spec_antilag[trailing_history]))
        antilag_asymmetry = antilag_lead / max(antilag_trail, 1e-9)
        footprint_lead = float(np.mean(spec_footprint[leading_history]))
        footprint_trail = float(np.mean(spec_footprint[trailing_history]))

        component_metrics.append({
            "component_id": match["after"]["label_id"],
            "lead_mean": lead_mean,
            "trail_mean": trail_mean,
            "asymmetry": asymmetry,
            "antilag_lead": antilag_lead,
            "antilag_trail": antilag_trail,
            "antilag_asymmetry": antilag_asymmetry,
            "footprint_lead": footprint_lead,
            "footprint_trail": footprint_trail,
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

    lead_means = np.array([m["lead_mean"]
                          for m in component_metrics], dtype=np.float32)
    trail_means = np.array([m["trail_mean"]
                           for m in component_metrics], dtype=np.float32)
    asymmetry = np.array([m["asymmetry"]
                         for m in component_metrics], dtype=np.float32)
    antilag_leads = np.array([m["antilag_lead"]
                             for m in component_metrics], dtype=np.float32)
    antilag_trails = np.array([m["antilag_trail"]
                              for m in component_metrics], dtype=np.float32)
    antilag_asymmetry = np.array([m["antilag_asymmetry"]
                                 for m in component_metrics], dtype=np.float32)
    footprint_leads = np.array([m["footprint_lead"]
                               for m in component_metrics], dtype=np.float32)
    footprint_trails = np.array([m["footprint_trail"]
                                for m in component_metrics], dtype=np.float32)

    top_count = min(3, len(component_metrics))
    top3_leading = float(np.mean(np.sort(lead_means)[-top_count:]))
    top3_asymmetry = float(np.mean(np.sort(asymmetry)[-top_count:]))
    mean_trailing = float(np.mean(trail_means))
    top3_antilag_leading = float(np.mean(np.sort(antilag_leads)[-top_count:]))
    mean_antilag_trailing = float(np.mean(antilag_trails))
    top3_antilag_asymmetry = float(np.mean(np.sort(antilag_asymmetry)[-top_count:]))
    mean_footprint_leading = float(np.mean(footprint_leads))
    mean_footprint_trailing = float(np.mean(footprint_trails))

    failures = []
    if top3_asymmetry > MAX_SETTLED_SPEC_BLEND_LEAD_TRAIL_ASYMMETRY:
        failures.append(
            f"top-{top_count} settled TS spec blend lead/trail asymmetry {top3_asymmetry:.2f} > "
            f"{MAX_SETTLED_SPEC_BLEND_LEAD_TRAIL_ASYMMETRY:.2f}")
    if top3_leading > MAX_SETTLED_SPEC_BLEND_LEADING:
        failures.append(
            f"top-{top_count} settled TS leading spec blend {top3_leading:.2f} > "
            f"{MAX_SETTLED_SPEC_BLEND_LEADING:.2f}")
    if top3_antilag_asymmetry > MAX_SETTLED_SPEC_ANTILAG_LEAD_TRAIL_ASYMMETRY:
        failures.append(
            f"top-{top_count} settled TS spec antilag lead/trail asymmetry "
            f"{top3_antilag_asymmetry:.2f} > "
            f"{MAX_SETTLED_SPEC_ANTILAG_LEAD_TRAIL_ASYMMETRY:.2f}")

    return {
        "ok": not failures,
        "hard_error": False,
        "reason": "; ".join(failures),
        "top3_leading": top3_leading,
        "mean_trailing": mean_trailing,
        "top3_asymmetry": top3_asymmetry,
        "top3_antilag_leading": top3_antilag_leading,
        "mean_antilag_trailing": mean_antilag_trailing,
        "top3_antilag_asymmetry": top3_antilag_asymmetry,
        "mean_footprint_leading": mean_footprint_leading,
        "mean_footprint_trailing": mean_footprint_trailing,
        "leading_union": leading_union,
        "trailing_union": trailing_union,
        "spec_blend": spec_blend,
    }


def save_diagnostics(artifact_dir, index, analysis):
    spec_blend = np.clip(analysis["spec_blend"], 0.0, 1.0)
    gray = (spec_blend * 255).astype(np.uint8)
    overlay = np.stack([gray, gray, gray], axis=-1)
    overlay[analysis["trailing_union"]] = [0, 255, 0]
    overlay[analysis["leading_union"]] = [255, 0, 0]
    Image.fromarray(overlay).save(
        os.path.join(artifact_dir, f"diag_repeated_motion_ts_spec_blend_overlay_{index}.png"))


def main():
    args, extra_args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)
    artifact_dir = prepare_artifact_dir(screenshot_dir)

    print("=" * 70)
    print("  Repeated-Motion TS Spec State")
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
        ("signal", ["--reblur_no_pt_blend", "true",
                     "--reblur_debug_pass", "TSSpecBlend"]),
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
    print("  Analysis: settled TS specular blend")
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
            f"  Nudge {index}: lead_spec_blend={analysis['top3_leading']:.2f} "
            f"trail_blend={analysis['mean_trailing']:.2f} "
            f"blend_lt={analysis['top3_asymmetry']:.2f}x "
            f"lead_antilag={analysis['top3_antilag_leading']:.2f} "
            f"trail_antilag={analysis['mean_antilag_trailing']:.2f} "
            f"antilag_lt={analysis['top3_antilag_asymmetry']:.2f}x "
            f"foot={analysis['mean_footprint_leading']:.2f}/{analysis['mean_footprint_trailing']:.2f}"
        )

    failures = [entry["reason"] for entry in per_nudge if not entry["ok"]]
    worst_leading = max(entry["top3_leading"] for entry in per_nudge)
    worst_asymmetry = max(entry["top3_asymmetry"] for entry in per_nudge)
    worst_antilag_asymmetry = max(entry["top3_antilag_asymmetry"] for entry in per_nudge)

    print(f"\n  Worst settled leading TS spec blend: {worst_leading:.2f}")
    print(f"  Worst settled blend lead/trail asymmetry:   {worst_asymmetry:.2f}x")
    print(f"  Worst settled antilag lead/trail asymmetry: {worst_antilag_asymmetry:.2f}x")
    print(f"  Diagnostics saved to {artifact_dir}")
    if failures:
        print(f"  FAIL: {'; '.join(failures)}")
        return 1

    print("  PASS: TS spec state stays symmetric under repeated motion")
    return 0


if __name__ == "__main__":
    sys.exit(main())

