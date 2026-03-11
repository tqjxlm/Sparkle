#!/usr/bin/env python3
"""Repeated-motion settled-output stage delta diagnostic for REBLUR.

This test compares two REBLUR debug passes on the same repeated camera-motion
sequence, but measures the same-pose settled frames instead of the early fast
frames.

Question:
Does the compare stage become substantially cleaner than the base stage on the
history-valid motion-leading shells after both stages are allowed to settle?

This is useful when fast-frame amplification alone does not explain a large
same-stage fast/settled ratio. A large settled-output delta means stage-local
settling strength is itself changing the reference, so fast/settled ratios are
not sufficient to localize the bug.
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

MAX_WORST_TOP3_LEADING_SETTLED_GAIN = 1.05
MAX_WORST_SINGLE_COMPONENT_SETTLED_GAIN = 1.08
MAX_WORST_TOP3_SETTLED_LEAD_TRAIL_ASYMMETRY = 1.10


def parse_args():
    parser = argparse.ArgumentParser(
        description="Repeated-motion stage settled delta diagnostic")
    parser.add_argument("--framework", default="macos",
                        choices=("glfw", "macos"))
    parser.add_argument("--base_debug_pass", default="PostBlurSpecular")
    parser.add_argument("--compare_debug_pass", default="CompositeSpecular")
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
    artifact_dir = os.path.join(screenshot_dir, "repeated_motion_stage_settled_debug")
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


def analyze_nudge(index, base_settled_path, compare_settled_path, disocclusion_path,
                  prev_material_path, current_material_path):
    base_img, base_luma = load_luminance(base_settled_path)
    _, compare_luma = load_luminance(compare_settled_path)
    base_hf = compute_hf_residual(base_luma)
    compare_hf = compute_hf_residual(compare_luma)
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

        lead_gain = float(
            np.mean(base_hf[leading_history]) /
            max(np.mean(compare_hf[leading_history]), 1e-9))
        trail_gain = float(
            np.mean(base_hf[trailing_history]) /
            max(np.mean(compare_hf[trailing_history]), 1e-9))
        asymmetry = lead_gain / max(trail_gain, 1e-9)

        component_metrics.append({
            "component_id": match["after"]["label_id"],
            "lead_gain": lead_gain,
            "trail_gain": trail_gain,
            "asymmetry": asymmetry,
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

    lead_gains = np.array([m["lead_gain"]
                          for m in component_metrics], dtype=np.float32)
    asymmetry = np.array([m["asymmetry"]
                         for m in component_metrics], dtype=np.float32)
    top_count = min(3, len(component_metrics))
    top3_lead_gain = float(np.mean(np.sort(lead_gains)[-top_count:]))
    top3_asym_mean = float(np.mean(np.sort(asymmetry)[-top_count:]))
    worst_component_lead_gain = float(np.max(lead_gains))

    failures = []
    if top3_lead_gain > MAX_WORST_TOP3_LEADING_SETTLED_GAIN:
        failures.append(
            f"top-{top_count} leading base/compare settled gain {top3_lead_gain:.2f} > "
            f"{MAX_WORST_TOP3_LEADING_SETTLED_GAIN:.2f}")
    if worst_component_lead_gain > MAX_WORST_SINGLE_COMPONENT_SETTLED_GAIN:
        failures.append(
            f"worst single-component leading base/compare settled gain "
            f"{worst_component_lead_gain:.2f} > "
            f"{MAX_WORST_SINGLE_COMPONENT_SETTLED_GAIN:.2f}")
    if top3_asym_mean > MAX_WORST_TOP3_SETTLED_LEAD_TRAIL_ASYMMETRY:
        failures.append(
            f"top-{top_count} settled lead/trail asymmetry {top3_asym_mean:.2f} > "
            f"{MAX_WORST_TOP3_SETTLED_LEAD_TRAIL_ASYMMETRY:.2f}")

    return {
        "ok": not failures,
        "hard_error": False,
        "reason": "; ".join(failures),
        "top3_lead_gain": top3_lead_gain,
        "top3_asym_mean": top3_asym_mean,
        "worst_component_lead_gain": worst_component_lead_gain,
        "leading_union": leading_union,
        "trailing_union": trailing_union,
        "base_img": base_img,
        "base_hf": base_hf,
        "compare_hf": compare_hf,
    }


def save_diagnostics(artifact_dir, index, analysis):
    overlay = np.clip(analysis["base_img"], 0, 1)
    overlay = (overlay * 255).astype(np.uint8)
    overlay[analysis["trailing_union"]] = [0, 255, 0]
    overlay[analysis["leading_union"]] = [255, 0, 0]
    Image.fromarray(overlay).save(
        os.path.join(artifact_dir, f"diag_repeated_motion_stage_settled_overlay_{index}.png"))

    gain = np.clip(
        (analysis["base_hf"] - analysis["compare_hf"]) / 0.05, 0, 1)
    heatmap = np.zeros_like(overlay)
    heatmap[:, :, 1] = (gain * 255).astype(np.uint8)
    heatmap[:, :, 2] = ((1.0 - gain) * 64).astype(np.uint8)
    heatmap[analysis["trailing_union"]] = [0, 180, 0]
    heatmap[analysis["leading_union"]] = [255, 200, 0]
    Image.fromarray(heatmap).save(
        os.path.join(artifact_dir, f"diag_repeated_motion_stage_settled_gain_{index}.png"))


def main():
    args, extra_args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(fw)
    artifact_dir = prepare_artifact_dir(screenshot_dir)

    print("=" * 70)
    print("  Repeated-Motion Stage Settled Delta")
    print("=" * 70)
    print(f"  Base debug pass:    {args.base_debug_pass}")
    print(f"  Compare debug pass: {args.compare_debug_pass}")

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
        ("base", ["--reblur_no_pt_blend", "true",
                   "--reblur_debug_pass", args.base_debug_pass]),
        ("compare", ["--reblur_no_pt_blend", "true",
                      "--reblur_debug_pass", args.compare_debug_pass]),
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
    print("  Analysis: settled-frame stage drift")
    print(f"{'=' * 70}")

    per_nudge = []
    for index in range(NUDGE_COUNT):
        prev_name = "ghosting_before.png" if index == 0 else \
            f"ghosting_nudge_{index - 1}_settled.png"
        settled_name = f"ghosting_nudge_{index}_settled.png"
        fast_name = f"ghosting_nudge_{index}_fast.png"

        analysis = analyze_nudge(
            index,
            archived["base"][settled_name],
            archived["compare"][settled_name],
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
            f"  Nudge {index}: lead_base/compare_settled={analysis['top3_lead_gain']:.2f}x "
            f"worst_comp={analysis['worst_component_lead_gain']:.2f}x "
            f"lead/trail={analysis['top3_asym_mean']:.2f}x"
        )

    failures = [entry["reason"] for entry in per_nudge if not entry["ok"]]
    worst_gain = max(entry["top3_lead_gain"] for entry in per_nudge)
    worst_asym = max(entry["top3_asym_mean"] for entry in per_nudge)
    worst_component = max(entry["worst_component_lead_gain"] for entry in per_nudge)

    print(f"\n  Worst leading base/compare settled gain: {worst_gain:.2f}x")
    print(f"  Worst single-component settled gain:     {worst_component:.2f}x")
    print(f"  Worst settled lead/trail asymmetry:      {worst_asym:.2f}x")
    print(f"  Diagnostics saved to {artifact_dir}")
    if failures:
        print(f"  FAIL: {'; '.join(failures)}")
        return 1

    print("  PASS: compare stage does not materially change settled leading-shell output")
    return 0


if __name__ == "__main__":
    sys.exit(main())
