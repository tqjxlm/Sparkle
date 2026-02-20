"""ASVGF pass sanity tests with quantitative pass/fail checks.

This script runs a small set of debug captures and validates:
1) ray-trace feature outputs are non-degenerate,
2) reprojection output has no center-line seam artifact (quadrant discontinuity proxy),
3) reprojection mask is not trivially all-valid/all-invalid,
4) history_length and history_length_raw encode consistent normalized history,
5) small deterministic camera nudge keeps mostly-valid reprojection while introducing some invalidation.

Optional tonemap-sensitive checks are available, but disabled by default because
the debug views are captured after display mapping, which can saturate values.
"""

import argparse
import glob
import os
import platform
import subprocess
import sys
import time

import numpy as np
from PIL import Image

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)

SUPPORTED_FRAMEWORKS = ("glfw", "macos")
DEFAULT_SCENE = "TestScene"
SUITES = ("reprojection", "raytrace", "all")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run ASVGF debug captures and validate sanity invariants.")
    parser.add_argument("--framework", required=True, choices=SUPPORTED_FRAMEWORKS)
    parser.add_argument("--suite", choices=SUITES, default="all")
    parser.add_argument(
        "--scene",
        help="Scene path/name passed to runtime. Omit to use built-in standard scene.")
    parser.add_argument(
        "--screenshot_scene",
        default=DEFAULT_SCENE,
        help="Scene prefix used in screenshot filename matching.")
    parser.add_argument("--spp", type=int, default=1)
    parser.add_argument("--max_spp", type=int, default=64)
    parser.add_argument("--seam_percentile_threshold", type=float, default=95.0)
    parser.add_argument("--seam_ratio_threshold", type=float, default=4.0)
    parser.add_argument("--mask_min_valid_ratio", type=float, default=0.05)
    parser.add_argument("--mask_max_valid_ratio", type=float, default=0.95)
    parser.add_argument("--history_raw_red_mae_threshold", type=float, default=0.03)
    parser.add_argument("--history_mid_ratio_min", type=float, default=0.02)
    parser.add_argument("--raytrace_normal_std_min", type=float, default=0.03)
    parser.add_argument("--raytrace_albedo_std_min", type=float, default=0.02)
    parser.add_argument("--raytrace_depth_std_min", type=float, default=0.01)
    parser.add_argument(
        "--disable_motion_reprojection_check",
        action="store_true",
        help="Skip deterministic small-camera-move reprojection mask check.",
    )
    parser.add_argument("--motion_yaw_deg", type=float, default=0.8)
    parser.add_argument("--motion_pitch_deg", type=float, default=0.0)
    parser.add_argument("--motion_post_nudge_frames", type=int, default=1)
    parser.add_argument("--motion_min_drop", type=float, default=0.01)
    parser.add_argument("--motion_min_valid_fraction", type=float, default=0.5)
    parser.add_argument(
        "--enable_tonemap_sensitive_checks",
        action="store_true",
        help=(
            "Enable spp/history-cap growth checks. Off by default because screenshot-space "
            "values can be saturated by display mapping."
        ),
    )
    parser.add_argument("--growth_small_spp", type=int, default=8)
    parser.add_argument("--growth_large_spp", type=int, default=64)
    parser.add_argument("--growth_history_cap", type=int, default=256)
    parser.add_argument("--growth_min_delta", type=float, default=0.05)
    parser.add_argument("--cap_test_spp", type=int, default=64)
    parser.add_argument("--cap_small", type=int, default=16)
    parser.add_argument("--cap_large", type=int, default=256)
    parser.add_argument("--cap_min_delta", type=float, default=0.12)
    parser.add_argument("--capture_retries", type=int, default=2)
    return parser.parse_known_args()


def get_executable(framework):
    if framework == "glfw":
        exe_name = "sparkle.exe" if platform.system() == "Windows" else "sparkle"
        build_dir = os.path.join(PROJECT_ROOT, "build_system", "glfw", "output", "build")
        return os.path.join(build_dir, exe_name), build_dir
    if framework == "macos":
        app_path = os.path.join(PROJECT_ROOT, "build_system", "macos", "output", "build", "sparkle.app")
        return os.path.join(app_path, "Contents", "MacOS", "sparkle"), None
    raise ValueError(f"Unsupported framework: {framework}")


def get_screenshot_dir(framework):
    if framework == "glfw":
        return os.path.join(PROJECT_ROOT, "build_system", "glfw", "output", "build", "generated", "screenshots")
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    raise ValueError(f"Unsupported framework: {framework}")


def find_newest_screenshot(framework, screenshot_scene):
    screenshot_dir = get_screenshot_dir(framework)
    pattern = os.path.join(screenshot_dir, f"{screenshot_scene}_gpu_*.png")
    matches = glob.glob(pattern)
    if not matches:
        return None
    matches.sort(key=os.path.getmtime, reverse=True)
    return matches[0]


def run_app(framework, scene_arg, screenshot_scene, stage, pass_args, extra_args):
    exe_path, cwd = get_executable(framework)
    if not os.path.exists(exe_path):
        print(f"Executable not found: {exe_path}")
        print(f"Build the project first with: python build.py --framework {framework}")
        return False, None

    before_latest = find_newest_screenshot(framework, screenshot_scene)
    before_mtime = os.path.getmtime(before_latest) if before_latest else 0.0
    start_time = time.time()

    run_cmd = [
        exe_path,
        "--auto_screenshot",
        "true",
        "--pipeline",
        "gpu",
        "--asvgf",
        "true",
        "--asvgf_test_stage",
        stage,
    ] + pass_args + extra_args

    if scene_arg:
        run_cmd += ["--scene", scene_arg]

    print(f"Running: {' '.join(run_cmd)}")
    result = subprocess.run(run_cmd, cwd=cwd)
    if result.returncode != 0:
        return False, None

    newest = find_newest_screenshot(framework, screenshot_scene)
    if newest is None:
        return False, None

    newest_mtime = os.path.getmtime(newest)
    if newest_mtime <= max(before_mtime, start_time - 1.0):
        return False, None
    return True, newest


def capture_with_retry(framework, scene_arg, screenshot_scene, stage, view, spp, max_spp, runtime_args, extra_args,
                       retries):
    pass_args = [
        "--asvgf_debug_view",
        view,
        "--spp",
        str(spp),
        "--max_spp",
        str(max_spp),
    ] + runtime_args

    for attempt in range(retries + 1):
        ok, screenshot = run_app(framework, scene_arg, screenshot_scene, stage, pass_args, extra_args)
        if ok and screenshot:
            print(f"Captured {stage}/{view}: {screenshot}")
            return screenshot
        print(f"Capture failed for {stage}/{view} (attempt {attempt + 1}/{retries + 1})")
    return None


def load_rgb01(path):
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def center_seam_metrics(image_channel):
    # Vertical boundary scores: width-1 samples at x|x+1 boundary.
    vertical_boundaries = np.mean(np.abs(image_channel[:, 1:] - image_channel[:, :-1]), axis=0)
    # Horizontal boundary scores: height-1 samples at y|y+1 boundary.
    horizontal_boundaries = np.mean(np.abs(image_channel[1:, :] - image_channel[:-1, :]), axis=1)

    center_x_boundary = (image_channel.shape[1] // 2) - 1
    center_y_boundary = (image_channel.shape[0] // 2) - 1

    center_v = float(vertical_boundaries[center_x_boundary])
    center_h = float(horizontal_boundaries[center_y_boundary])
    median_v = float(np.median(vertical_boundaries))
    median_h = float(np.median(horizontal_boundaries))

    percentile_v = float(np.mean(vertical_boundaries <= center_v) * 100.0)
    percentile_h = float(np.mean(horizontal_boundaries <= center_h) * 100.0)

    ratio_v = center_v / max(median_v, 1e-6)
    ratio_h = center_h / max(median_h, 1e-6)

    return {
        "center_vertical": center_v,
        "center_horizontal": center_h,
        "percentile_vertical": percentile_v,
        "percentile_horizontal": percentile_h,
        "ratio_vertical": ratio_v,
        "ratio_horizontal": ratio_h,
    }


def validate_seam(metrics, percentile_threshold, ratio_threshold):
    v_bad = metrics["percentile_vertical"] > percentile_threshold and metrics["ratio_vertical"] > ratio_threshold
    h_bad = metrics["percentile_horizontal"] > percentile_threshold and metrics["ratio_horizontal"] > ratio_threshold
    return not (v_bad or h_bad)


def capture_or_fail(args, extra_args, screenshot_scene, stage, view, spp, max_spp, runtime_args):
    screenshot = capture_with_retry(
        framework=args.framework,
        scene_arg=args.scene,
        screenshot_scene=screenshot_scene,
        stage=stage,
        view=view,
        spp=spp,
        max_spp=max_spp,
        runtime_args=runtime_args,
        extra_args=extra_args,
        retries=max(args.capture_retries, 0),
    )
    if screenshot is None:
        raise RuntimeError(f"unable to capture {stage}/{view}")
    return screenshot


def channel_std(image):
    # Return std of the luminance-like average channel.
    return float(np.std(np.mean(image, axis=2)))


def run_raytrace_suite(args, extra_args, screenshot_scene):
    captures = {}
    for view in ("normal", "albedo", "depth"):
        captures[view] = capture_or_fail(
            args,
            extra_args,
            screenshot_scene,
            stage="raytrace",
            view=view,
            spp=args.spp,
            max_spp=args.max_spp,
            runtime_args=[],
        )

    normal_img = load_rgb01(captures["normal"])
    albedo_img = load_rgb01(captures["albedo"])
    depth_img = load_rgb01(captures["depth"])

    normal_std = channel_std(normal_img)
    albedo_std = channel_std(albedo_img)
    depth_std = channel_std(depth_img)

    print("ASVGF Raytrace Metrics")
    print(f"  normal std: {normal_std:.6f}")
    print(f"  albedo std: {albedo_std:.6f}")
    print(f"  depth std: {depth_std:.6f}")

    failed = False
    if normal_std < args.raytrace_normal_std_min:
        print("FAIL: normal debug output appears degenerate.")
        failed = True
    if albedo_std < args.raytrace_albedo_std_min:
        print("FAIL: albedo debug output appears degenerate.")
        failed = True
    if depth_std < args.raytrace_depth_std_min:
        print("FAIL: depth debug output appears degenerate.")
        failed = True
    return not failed


def run_reprojection_suite(args, extra_args, screenshot_scene):
    captures = {}
    for view in ("history_length", "history_length_raw", "reprojection_mask"):
        captures[view] = capture_or_fail(
            args,
            extra_args,
            screenshot_scene,
            stage="reprojection",
            view=view,
            spp=args.spp,
            max_spp=args.max_spp,
            runtime_args=[],
        )

    history_img = load_rgb01(captures["history_length"])
    history_raw_img = load_rgb01(captures["history_length_raw"])
    mask_img = load_rgb01(captures["reprojection_mask"])

    # history_length is grayscale in all channels; use R channel.
    history_r = history_img[:, :, 0]
    # history_length_raw encodes normalized history in R.
    history_raw_r = history_raw_img[:, :, 0]
    # reprojection mask debug view is grayscale in all channels; use R channel.
    mask_r = mask_img[:, :, 0]

    history_seam = center_seam_metrics(history_r)
    history_raw_seam = center_seam_metrics(history_raw_r)

    history_seam_ok = validate_seam(
        history_seam, args.seam_percentile_threshold, args.seam_ratio_threshold)
    history_raw_seam_ok = validate_seam(
        history_raw_seam, args.seam_percentile_threshold, args.seam_ratio_threshold)

    # Cross-view consistency: history_length_raw.R should match history_length.
    history_raw_red_mae = float(np.mean(np.abs(history_r - history_raw_r)))
    history_raw_consistent = history_raw_red_mae <= args.history_raw_red_mae_threshold

    # Guard against degenerate black/white-only outputs.
    history_mid_ratio = float(np.mean((history_r > 0.05) & (history_r < 0.95)))
    history_raw_mid_ratio = float(np.mean((history_raw_r > 0.05) & (history_raw_r < 0.95)))
    history_has_midtones = history_mid_ratio >= args.history_mid_ratio_min
    history_raw_has_midtones = history_raw_mid_ratio >= args.history_mid_ratio_min

    # Mask should have both valid and invalid regions on TestScene, static camera.
    valid_ratio = float(np.mean(mask_r > 0.5))
    mask_non_trivial = args.mask_min_valid_ratio <= valid_ratio <= args.mask_max_valid_ratio

    motion_valid_ratio = None
    motion_has_invalidation = True
    motion_keeps_majority = True
    if not args.disable_motion_reprojection_check:
        motion_mask = capture_or_fail(
            args,
            extra_args,
            screenshot_scene,
            stage="reprojection",
            view="reprojection_mask",
            spp=args.spp,
            max_spp=args.max_spp,
            runtime_args=[
                "--asvgf_test_camera_nudge_yaw",
                str(args.motion_yaw_deg),
                "--asvgf_test_camera_nudge_pitch",
                str(args.motion_pitch_deg),
                "--asvgf_test_post_nudge_frames",
                str(max(args.motion_post_nudge_frames, 1)),
            ],
        )
        motion_mask_r = load_rgb01(motion_mask)[:, :, 0]
        motion_valid_ratio = float(np.mean(motion_mask_r > 0.5))

        # Small move should invalidate some pixels but keep most history valid.
        motion_has_invalidation = motion_valid_ratio <= (valid_ratio - args.motion_min_drop)
        motion_keeps_majority = motion_valid_ratio >= (valid_ratio * args.motion_min_valid_fraction)

    growth_delta = None
    cap_delta = None
    growth_ok = True
    cap_ok = True
    if args.enable_tonemap_sensitive_checks:
        # Growth check: higher spp should increase average history when cap is high enough.
        history_small = capture_or_fail(
            args,
            extra_args,
            screenshot_scene,
            stage="reprojection",
            view="history_length",
            spp=args.spp,
            max_spp=args.growth_small_spp,
            runtime_args=["--asvgf_history_cap", str(args.growth_history_cap)],
        )
        history_large = capture_or_fail(
            args,
            extra_args,
            screenshot_scene,
            stage="reprojection",
            view="history_length",
            spp=args.spp,
            max_spp=args.growth_large_spp,
            runtime_args=["--asvgf_history_cap", str(args.growth_history_cap)],
        )
        mean_growth_small = float(np.mean(load_rgb01(history_small)[:, :, 0]))
        mean_growth_large = float(np.mean(load_rgb01(history_large)[:, :, 0]))
        growth_delta = mean_growth_large - mean_growth_small
        growth_ok = growth_delta >= args.growth_min_delta

        # Cap sensitivity check: lower history_cap should produce brighter normalized history at fixed spp.
        history_cap_small = capture_or_fail(
            args,
            extra_args,
            screenshot_scene,
            stage="reprojection",
            view="history_length",
            spp=args.spp,
            max_spp=args.cap_test_spp,
            runtime_args=["--asvgf_history_cap", str(args.cap_small)],
        )
        history_cap_large = capture_or_fail(
            args,
            extra_args,
            screenshot_scene,
            stage="reprojection",
            view="history_length",
            spp=args.spp,
            max_spp=args.cap_test_spp,
            runtime_args=["--asvgf_history_cap", str(args.cap_large)],
        )
        mean_cap_small = float(np.mean(load_rgb01(history_cap_small)[:, :, 0]))
        mean_cap_large = float(np.mean(load_rgb01(history_cap_large)[:, :, 0]))
        cap_delta = mean_cap_small - mean_cap_large
        cap_ok = cap_delta >= args.cap_min_delta

    print("ASVGF Reprojection Metrics")
    print(f"  history_length seam: {history_seam}")
    print(f"  history_length_raw seam: {history_raw_seam}")
    print(f"  history/raw red MAE: {history_raw_red_mae:.6f}")
    print(f"  history_length midtone ratio: {history_mid_ratio:.4f}")
    print(f"  history_length_raw midtone ratio: {history_raw_mid_ratio:.4f}")
    print(f"  reprojection valid ratio: {valid_ratio:.4f}")
    if not args.disable_motion_reprojection_check:
        print(
            f"  reprojection valid ratio (small camera nudge yaw={args.motion_yaw_deg}, "
            f"pitch={args.motion_pitch_deg}): {motion_valid_ratio:.4f}"
        )
    if args.enable_tonemap_sensitive_checks:
        print(
            f"  history growth delta (spp {args.growth_small_spp}->{args.growth_large_spp}, "
            f"cap {args.growth_history_cap}): {growth_delta:.6f}"
        )
        print(
            f"  history cap delta (cap {args.cap_small}->{args.cap_large}, "
            f"spp {args.cap_test_spp}): {cap_delta:.6f}"
        )
    else:
        print("  note: tonemap-sensitive growth/cap checks disabled (use --enable_tonemap_sensitive_checks to enable).")

    failed = False
    if not history_seam_ok:
        print("FAIL: history_length has a suspicious center-line seam.")
        failed = True
    if not history_raw_seam_ok:
        print("FAIL: history_length_raw has a suspicious center-line seam.")
        failed = True
    if not history_raw_consistent:
        print("FAIL: history_length and history_length_raw.R are inconsistent.")
        failed = True
    if not history_has_midtones:
        print("FAIL: history_length appears degenerate (insufficient midtone coverage).")
        failed = True
    if not history_raw_has_midtones:
        print("FAIL: history_length_raw appears degenerate (insufficient midtone coverage).")
        failed = True
    if not mask_non_trivial:
        print("FAIL: reprojection mask valid ratio is trivial or unexpected.")
        failed = True
    if not args.disable_motion_reprojection_check and not motion_has_invalidation:
        print("FAIL: small camera nudge did not invalidate enough reprojection pixels.")
        failed = True
    if not args.disable_motion_reprojection_check and not motion_keeps_majority:
        print("FAIL: small camera nudge invalidated too much reprojection history.")
        failed = True
    if args.enable_tonemap_sensitive_checks and not growth_ok:
        print("FAIL: history growth with higher spp is too small.")
        failed = True
    if args.enable_tonemap_sensitive_checks and not cap_ok:
        print("FAIL: history response to history_cap change is too small.")
        failed = True
    return not failed


def main():
    args, extra_args = parse_args()
    screenshot_scene = args.screenshot_scene
    if args.scene and screenshot_scene == DEFAULT_SCENE:
        screenshot_scene = os.path.splitext(os.path.basename(args.scene))[0]
    suite = args.suite
    failed = False

    try:
        if suite in ("raytrace", "all"):
            if not run_raytrace_suite(args, extra_args, screenshot_scene):
                failed = True

        if suite in ("reprojection", "all"):
            if not run_reprojection_suite(args, extra_args, screenshot_scene):
                failed = True
    except RuntimeError as error:
        print(f"FAIL: {error}")
        return 1

    if failed:
        print(f"ASVGF sanity ({suite}): FAIL")
        return 1

    print(f"ASVGF sanity ({suite}): PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
