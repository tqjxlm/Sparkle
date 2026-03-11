#!/usr/bin/env python3
"""Repeated-motion TS specular antilag-input diagnostic for REBLUR.

Runs the repeated camera-nudge sequence with the `TSSpecAntilagInputs` debug
pass and measures settled-frame TS specular divergence / confidence on
history-valid motion-leading and trailing shell pixels.

Question:
Is the remaining TS asymmetry driven more by stale incoming confidence or by
TS seeing lower divergence on the motion-leading side?
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

MIN_SETTLED_SPEC_DIVERGENCE_LEAD_TRAIL_RATIO = 0.90
MAX_SETTLED_SPEC_INCOMING_CONFIDENCE_LEAD_TRAIL_ASYMMETRY = 1.08
MAX_SETTLED_SPEC_OUTGOING_CONFIDENCE_LEAD_TRAIL_ASYMMETRY = 1.10


def parse_args():
    parser = argparse.ArgumentParser(
        description="Repeated-motion TS specular antilag-input diagnostic")
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
    artifact_dir = os.path.join(
        screenshot_dir, "repeated_motion_ts_spec_antilag_inputs_debug")
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
    divergence = signal[:, :, 0]
    incoming_conf = signal[:, :, 1]
    outgoing_conf = signal[:, :, 2]
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

        lead_divergence = float(np.mean(divergence[leading_history]))
        trail_divergence = float(np.mean(divergence[trailing_history]))
        divergence_ratio = lead_divergence / max(trail_divergence, 1e-9)
        lead_incoming = float(np.mean(incoming_conf[leading_history]))
        trail_incoming = float(np.mean(incoming_conf[trailing_history]))
        incoming_asymmetry = lead_incoming / max(trail_incoming, 1e-9)
        lead_outgoing = float(np.mean(outgoing_conf[leading_history]))
        trail_outgoing = float(np.mean(outgoing_conf[trailing_history]))
        outgoing_asymmetry = lead_outgoing / max(trail_outgoing, 1e-9)

        component_metrics.append({
            "component_id": match["after"]["label_id"],
            "lead_divergence": lead_divergence,
            "trail_divergence": trail_divergence,
            "divergence_ratio": divergence_ratio,
            "lead_incoming": lead_incoming,
            "trail_incoming": trail_incoming,
            "incoming_asymmetry": incoming_asymmetry,
            "lead_outgoing": lead_outgoing,
            "trail_outgoing": trail_outgoing,
            "outgoing_asymmetry": outgoing_asymmetry,
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
    divergence_ratio = np.array([m["divergence_ratio"]
                                for m in component_metrics], dtype=np.float32)
    incoming_asymmetry = np.array([m["incoming_asymmetry"]
                                  for m in component_metrics], dtype=np.float32)
    outgoing_asymmetry = np.array([m["outgoing_asymmetry"]
                                  for m in component_metrics], dtype=np.float32)
    lead_divergence = np.array([m["lead_divergence"]
                               for m in component_metrics], dtype=np.float32)
    trail_divergence = np.array([m["trail_divergence"]
                                for m in component_metrics], dtype=np.float32)
    lead_incoming = np.array([m["lead_incoming"]
                             for m in component_metrics], dtype=np.float32)
    trail_incoming = np.array([m["trail_incoming"]
                              for m in component_metrics], dtype=np.float32)
    lead_outgoing = np.array([m["lead_outgoing"]
                             for m in component_metrics], dtype=np.float32)
    trail_outgoing = np.array([m["trail_outgoing"]
                              for m in component_metrics], dtype=np.float32)

    worst_divergence_ratio = float(np.mean(np.sort(divergence_ratio)[:top_count]))
    worst_incoming_asymmetry = float(np.mean(np.sort(incoming_asymmetry)[-top_count:]))
    worst_outgoing_asymmetry = float(np.mean(np.sort(outgoing_asymmetry)[-top_count:]))

    failures = []
    if worst_divergence_ratio < MIN_SETTLED_SPEC_DIVERGENCE_LEAD_TRAIL_RATIO:
        failures.append(
            f"top-{top_count} settled TS spec divergence lead/trail ratio {worst_divergence_ratio:.2f} < "
            f"{MIN_SETTLED_SPEC_DIVERGENCE_LEAD_TRAIL_RATIO:.2f}")
    if worst_incoming_asymmetry > MAX_SETTLED_SPEC_INCOMING_CONFIDENCE_LEAD_TRAIL_ASYMMETRY:
        failures.append(
            f"top-{top_count} settled TS incoming spec confidence lead/trail asymmetry "
            f"{worst_incoming_asymmetry:.2f} > "
            f"{MAX_SETTLED_SPEC_INCOMING_CONFIDENCE_LEAD_TRAIL_ASYMMETRY:.2f}")
    if worst_outgoing_asymmetry > MAX_SETTLED_SPEC_OUTGOING_CONFIDENCE_LEAD_TRAIL_ASYMMETRY:
        failures.append(
            f"top-{top_count} settled TS outgoing spec confidence lead/trail asymmetry "
            f"{worst_outgoing_asymmetry:.2f} > "
            f"{MAX_SETTLED_SPEC_OUTGOING_CONFIDENCE_LEAD_TRAIL_ASYMMETRY:.2f}")

    return {
        "ok": not failures,
        "hard_error": False,
        "reason": "; ".join(failures),
        "worst_divergence_ratio": worst_divergence_ratio,
        "worst_incoming_asymmetry": worst_incoming_asymmetry,
        "worst_outgoing_asymmetry": worst_outgoing_asymmetry,
        "mean_lead_divergence": float(np.mean(lead_divergence)),
        "mean_trail_divergence": float(np.mean(trail_divergence)),
        "mean_lead_incoming": float(np.mean(lead_incoming)),
        "mean_trail_incoming": float(np.mean(trail_incoming)),
        "mean_lead_outgoing": float(np.mean(lead_outgoing)),
        "mean_trail_outgoing": float(np.mean(trail_outgoing)),
        "leading_union": leading_union,
        "trailing_union": trailing_union,
        "outgoing_conf": outgoing_conf,
    }


def save_diagnostics(artifact_dir, index, analysis):
    outgoing_conf = np.clip(analysis["outgoing_conf"], 0.0, 1.0)
    gray = (outgoing_conf * 255).astype(np.uint8)
    overlay = np.stack([gray, gray, gray], axis=-1)
    overlay[analysis["trailing_union"]] = [0, 255, 0]
    overlay[analysis["leading_union"]] = [255, 0, 0]
    Image.fromarray(overlay).save(
        os.path.join(
            artifact_dir,
            f"diag_repeated_motion_ts_spec_antilag_inputs_overlay_{index}.png"))


def main():
    args, extra_args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)
    artifact_dir = prepare_artifact_dir(screenshot_dir)

    print("=" * 70)
    print("  Repeated-Motion TS Spec Antilag Inputs")
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
                     "--reblur_debug_pass", "TSSpecAntilagInputs"]),
        ("disocclusion", ["--reblur_debug_pass", "TADisocclusion"]),
        ("material", ["--reblur_debug_pass", "TAMaterialId"]),
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
                                                  prefix)

    print(f"\n{'=' * 70}")
    print("  Analysis: settled TS specular antilag inputs")
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
            f"  Nudge {index}: lead_d={analysis['mean_lead_divergence']:.3f} "
            f"trail_d={analysis['mean_trail_divergence']:.3f} "
            f"d_lt={analysis['worst_divergence_ratio']:.2f}x "
            f"lead_in={analysis['mean_lead_incoming']:.2f} "
            f"trail_in={analysis['mean_trail_incoming']:.2f} "
            f"in_lt={analysis['worst_incoming_asymmetry']:.2f}x "
            f"lead_out={analysis['mean_lead_outgoing']:.2f} "
            f"trail_out={analysis['mean_trail_outgoing']:.2f} "
            f"out_lt={analysis['worst_outgoing_asymmetry']:.2f}x"
        )

    failures = [entry["reason"] for entry in per_nudge if not entry["ok"]]
    worst_divergence_ratio = min(entry["worst_divergence_ratio"] for entry in per_nudge)
    worst_incoming_asymmetry = max(entry["worst_incoming_asymmetry"] for entry in per_nudge)
    worst_outgoing_asymmetry = max(entry["worst_outgoing_asymmetry"] for entry in per_nudge)

    print(f"\n  Worst settled TS divergence lead/trail ratio: {worst_divergence_ratio:.2f}x")
    print(f"  Worst settled TS incoming confidence asymmetry: {worst_incoming_asymmetry:.2f}x")
    print(f"  Worst settled TS outgoing confidence asymmetry: {worst_outgoing_asymmetry:.2f}x")
    print(f"  Diagnostics saved to {artifact_dir}")
    if failures:
        print(f"  FAIL: {'; '.join(failures)}")
        return 1

    print("  PASS: TS spec antilag inputs stay symmetric under repeated motion")
    return 0


if __name__ == "__main__":
    sys.exit(main())
