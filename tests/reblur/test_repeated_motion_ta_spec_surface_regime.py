#!/usr/bin/env python3
"""Repeated-motion TA specular surface-regime diagnostic for REBLUR.

This diagnostic asks a narrower attribution question on the exact
contaminated motion-leading shells:

Are these shells in the smooth-specular, subpixel-motion regime where
surface-motion reprojection remains almost fully trusted even though the
reflection itself can shift?
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
MOTION_DEBUG_SCALE = 100.0
MOTION_RESET_THRESHOLD_PX = 1.0


def parse_args():
    parser = argparse.ArgumentParser(
        description="Repeated-motion TA specular surface-regime diagnostic")
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
        screenshot_dir, "repeated_motion_ta_spec_surface_regime_debug")
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
                  surface_path):
    _, fast_luma = load_luminance(fast_path)
    _, settled_luma = load_luminance(settled_path)
    fast_hf = compute_hf_residual(fast_luma)
    settled_hf = compute_hf_residual(settled_luma)
    history_mask = load_history_mask(disocclusion_path)
    motion = load_numeric_image(motion_path)
    surface = load_numeric_image(surface_path)

    motion_x = (motion[:, :, 0] - 0.5) * MOTION_DEBUG_SCALE
    motion_y = (motion[:, :, 1] - 0.5) * MOTION_DEBUG_SCALE
    motion_px = np.hypot(motion_x, motion_y)
    roughness = surface[:, :, 0]
    hit_dist = surface[:, :, 1]
    spec_magic = surface[:, :, 2]

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
            "lead_motion_median": float(np.median(motion_px[leading_history])),
            "lead_subpixel_fraction": float(np.mean(motion_px[leading_history] < MOTION_RESET_THRESHOLD_PX)),
            "lead_roughness_median": float(np.median(roughness[leading_history])),
            "lead_roughness_p90": float(np.percentile(roughness[leading_history], 90.0)),
            "lead_hit_dist_median": float(np.median(hit_dist[leading_history])),
            "lead_spec_magic_median": float(np.median(spec_magic[leading_history])),
            "trail_roughness_median": float(np.median(roughness[trailing_history])),
            "trail_hit_dist_median": float(np.median(hit_dist[trailing_history])),
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
        "lead_motion_median": float(np.mean([m["lead_motion_median"] for m in top])),
        "lead_subpixel_fraction": float(np.mean([m["lead_subpixel_fraction"] for m in top])),
        "lead_roughness_median": float(np.mean([m["lead_roughness_median"] for m in top])),
        "lead_roughness_p90": float(np.mean([m["lead_roughness_p90"] for m in top])),
        "lead_hit_dist_median": float(np.mean([m["lead_hit_dist_median"] for m in top])),
        "lead_spec_magic_median": float(np.mean([m["lead_spec_magic_median"] for m in top])),
        "trail_roughness_median": float(np.mean([m["trail_roughness_median"] for m in top])),
        "trail_hit_dist_median": float(np.mean([m["trail_hit_dist_median"] for m in top])),
    }


def main():
    (args, extra) = parse_args()
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    screenshot_dir = get_screenshot_dir(args.framework)
    artifact_dir = prepare_artifact_dir(screenshot_dir)

    print("=" * 70)
    print("  Repeated-Motion TA Spec Surface Regime")
    print("=" * 70)
    print(f"  Eval debug pass: {args.eval_debug_pass}")

    runs = [
        ("eval", ["--reblur_no_pt_blend", "true",
                  "--reblur_debug_pass", args.eval_debug_pass]),
        ("motion", ["--reblur_debug_pass", "TAMotionVector"]),
        ("surface", ["--reblur_debug_pass", "TASpecSurfaceInputs"]),
        ("disocclusion", ["--reblur_debug_pass", "TADisocclusion"]),
        ("material", ["--reblur_debug_pass", "TAMaterialId"]),
    ]

    archived = {}
    for label, extra_args in runs:
        print("\n" + "-" * 70)
        print(f"  Run: {label}")
        print("-" * 70)
        ok = run_app(py, build_py, args.framework, extra_args + extra, label,
                     clear_screenshots=True)
        if not ok:
            return 1
        archived[label] = archive_ghosting_shots(screenshot_dir, artifact_dir, label)

    print("\n" + "=" * 70)
    print("  Analysis: contaminated-shell spec surface regime")
    print("=" * 70)

    analyzed = []
    for nudge_idx in range(NUDGE_COUNT):
        fast_name = f"ghosting_nudge_{nudge_idx}_fast.png"
        settled_name = f"ghosting_nudge_{nudge_idx}_settled.png"
        if fast_name not in archived["eval"] or settled_name not in archived["eval"]:
            print(f"  FAIL: missing eval capture for nudge {nudge_idx}")
            return 1

        result = analyze_nudge(
            nudge_idx,
            archived["eval"][fast_name],
            archived["eval"][settled_name],
            archived["disocclusion"][fast_name],
            archived["material"]["ghosting_before.png"],
            archived["material"][fast_name],
            archived["motion"][fast_name],
            archived["surface"][fast_name],
        )

        if not result["ok"]:
            print(f"  {result['reason']}")
            continue

        analyzed.append(result)
        print(
            f"  Nudge {nudge_idx}: lead_ratio={result['top_lead_ratio']:.2f}x "
            f"motion_med={result['lead_motion_median']:.2f}px "
            f"sub1={result['lead_subpixel_fraction']:.2f} "
            f"rough_med={result['lead_roughness_median']:.3f} "
            f"rough_p90={result['lead_roughness_p90']:.3f} "
            f"hit_med={result['lead_hit_dist_median']:.3f} "
            f"spec_magic={result['lead_spec_magic_median']:.3f}")

    if not analyzed:
        print("\n  FAIL: no contaminated leading-shell regions analyzed")
        return 1

    analyzed.sort(key=lambda r: r["top_lead_ratio"], reverse=True)
    top = analyzed[:min(3, len(analyzed))]

    print("\n  Worst-top3: "
          f"motion_med={np.mean([r['lead_motion_median'] for r in top]):.2f}px "
          f"sub1={np.mean([r['lead_subpixel_fraction'] for r in top]):.2f} "
          f"rough_med={np.mean([r['lead_roughness_median'] for r in top]):.3f} "
          f"rough_p90={np.mean([r['lead_roughness_p90'] for r in top]):.3f} "
          f"hit_med={np.mean([r['lead_hit_dist_median'] for r in top]):.3f} "
          f"spec_magic={np.mean([r['lead_spec_magic_median'] for r in top]):.3f}")
    print(f"\n  Diagnostics saved to {artifact_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
