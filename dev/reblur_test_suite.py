"""Dedicated ReBLUR test suite.

Phase 0 covers bootstrap checks:
- R0.1 Vanilla GPU functional regression (baseline GPU pipeline must pass `dev/functional_test.py`)
- S0.1 Entry-point smoke (denoiser-enabled GPU path runs and exits cleanly)
- S0.2 Pass-through equivalence (denoiser off vs on output difference is near zero)
- S0.3 Resize/reset smoke (denoiser-enabled path survives alternate render resolution)

Phase 1 adds Module A checks:
- A1 Roundtrip encoding error for normal/roughness/radiance/hit-distance packing
- A2 Motion-vector reprojection error on deterministic camera-motion fixtures
- A3 Guide validity ratio check for finite valid viewZ and normalized hit distance

Phase 2 starts with Module B checks:
- B1 Tile-mask precision/recall against CPU reference depth classifier
- B2 Determinism check via repeated tile-mask hash stability

Phase 2 Module C adds hit-distance reconstruction checks:
- C1 Reconstruction RMSE on masked invalid pixels
- C2 Preservation error on valid pixels
- C3 3x3 vs 5x5 monotonicity on sparse fixtures

Phase 2 Module D adds pre-pass checks:
- D1 Variance reduction ratio on flat regions
- D2 Cross-edge leakage guard
- D3 Spec hit-distance tracking jitter reduction

Phase 2 Module G adds blur checks:
- G1 High-frequency energy reduction on static noisy patches
- G2 Edge-preservation MSE guard near depth discontinuities
- G3 Effective blur-radius decrease as history increases
"""

import argparse
import glob
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

from denoiser_module_tests import (
    run_module_a_tests,
    run_module_b_tests,
    run_module_c_tests,
    run_module_d_tests,
    run_module_g_tests,
)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run the dedicated ReBLUR test suite.")
    parser.add_argument("--framework", default="glfw",
                        choices=("glfw", "macos"))
    parser.add_argument("--config", default="Release",
                        choices=("Release", "Debug"))
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--skip_build", action="store_true")
    parser.add_argument("--software", action="store_true",
                        help="Use Mesa lavapipe software Vulkan rendering (Windows only)")
    return parser.parse_args()


def run_command(command, env=None):
    print(f"Running: {' '.join(command)}", flush=True)
    result = subprocess.run(command, cwd=PROJECT_ROOT, env=env)
    if result.returncode != 0:
        print(f"FAILED ({result.returncode}): {' '.join(command)}", flush=True)
        sys.exit(result.returncode)


def get_screenshot_dir(framework):
    if framework == "glfw":
        return os.path.join(PROJECT_ROOT, "build_system", "glfw", "output", "build", "generated", "screenshots")
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    raise ValueError(f"Unsupported framework: {framework}")


def find_latest_screenshot(framework, scene, pipeline):
    screenshot_dir = get_screenshot_dir(framework)
    pattern = os.path.join(screenshot_dir, f"{scene}_{pipeline}_*.png")
    matches = glob.glob(pattern)
    if not matches:
        print(f"No screenshot found matching: {pattern}", flush=True)
        sys.exit(1)
    matches.sort(key=os.path.getmtime, reverse=True)
    return matches[0]


def load_image(path):
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32)


def compare_images(off_path, on_path):
    off_img = load_image(off_path)
    on_img = load_image(on_path)
    if off_img.shape != on_img.shape:
        print(
            f"Image size mismatch: {off_img.shape} vs {on_img.shape}", flush=True)
        sys.exit(1)

    abs_diff = np.abs(off_img - on_img)
    max_abs_diff = float(abs_diff.max())
    mean_abs_diff = float(abs_diff.mean())
    rmse = float(np.sqrt(np.mean((off_img - on_img) ** 2)))
    return max_abs_diff, mean_abs_diff, rmse


def main():
    args = parse_args()
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    functional_test_py = os.path.join(
        PROJECT_ROOT, "dev", "functional_test.py")
    scene_name = "TestScene"
    pipeline_name = "gpu"

    env = None
    if args.software:
        from run_without_gpu import setup_lavapipe
        env = setup_lavapipe()

    build_cmd = [
        sys.executable,
        build_py,
        "--framework",
        args.framework,
        "--config",
        args.config,
    ]
    if not args.skip_build:
        run_command(build_cmd, env=env)

    # After optional one-time build step, all suite tests run with --skip_build.
    base_cmd = build_cmd + ["--skip_build"]

    # R0.1 Baseline gate: vanilla GPU pipeline still passes functional test.
    functional_cmd = [
        sys.executable,
        functional_test_py,
        "--framework",
        args.framework,
        "--pipeline",
        pipeline_name,
        "--spatial_denoise",
        "false",
        "--config",
        args.config,
    ]
    if args.headless:
        functional_cmd.append("--headless")
    functional_cmd.append("--skip_build")
    if args.software:
        functional_cmd.append("--software")
    run_command(functional_cmd, env=env)

    # Module A quantitative checks (Phase 1).
    module_a_results = run_module_a_tests()
    print(
        "A1 metrics: "
        f"normal_rmse={module_a_results.a1_normal_rmse:.8f}, "
        f"roughness_rmse={module_a_results.a1_roughness_rmse:.8f}, "
        f"radiance_rmse={module_a_results.a1_radiance_rmse:.8f}, "
        f"norm_hit_rmse={module_a_results.a1_norm_hit_rmse:.8f}",
        flush=True,
    )
    print(
        "A2 metrics: "
        f"mean_reprojection_error_px={module_a_results.a2_mean_reprojection_error_px:.6f}, "
        f"max_reprojection_error_px={module_a_results.a2_max_reprojection_error_px:.6f}",
        flush=True,
    )
    print(
        f"A3 metrics: guide_valid_ratio={module_a_results.a3_guide_valid_ratio:.6%}", flush=True)
    if not module_a_results.passed:
        print("Module A quantitative checks FAILED", flush=True)
        return 1

    # Module B quantitative checks (Phase 2).
    module_b_results = run_module_b_tests()
    print(
        "B1 metrics: "
        f"tile_precision={module_b_results.b1_precision:.6f}, "
        f"tile_recall={module_b_results.b1_recall:.6f}",
        flush=True,
    )
    print(
        "B2 metrics: "
        f"unique_hash_count={module_b_results.b2_unique_hash_count}, "
        f"hash={module_b_results.b2_reference_hash}",
        flush=True,
    )
    if not module_b_results.passed:
        print("Module B quantitative checks FAILED", flush=True)
        return 1

    # Module C quantitative checks (Phase 2).
    module_c_results = run_module_c_tests()
    print(
        "C1 metrics: "
        f"invalid_rmse_norm={module_c_results.c1_invalid_rmse_norm:.6f}",
        flush=True,
    )
    print(
        "C2 metrics: "
        f"valid_luma_abs_error={module_c_results.c2_valid_luma_abs_error:.8f}",
        flush=True,
    )
    print(
        "C3 metrics: "
        f"rmse_3x3_norm={module_c_results.c3_rmse_3x3_norm:.6f}, "
        f"rmse_5x5_norm={module_c_results.c3_rmse_5x5_norm:.6f}",
        flush=True,
    )
    if not module_c_results.passed:
        print("Module C quantitative checks FAILED", flush=True)
        return 1

    # Module D quantitative checks (Phase 2).
    module_d_results = run_module_d_tests()
    print(
        "D1 metrics: "
        f"variance_reduction_ratio={module_d_results.d1_variance_reduction_ratio:.6f}",
        flush=True,
    )
    print(
        "D2 metrics: "
        f"edge_leakage={module_d_results.d2_edge_leakage:.6f}",
        flush=True,
    )
    print(
        "D3 metrics: "
        f"jitter_reduction_ratio={module_d_results.d3_jitter_reduction_ratio:.6f}, "
        f"baseline_jitter={module_d_results.d3_baseline_jitter:.6f}, "
        f"tracking_jitter={module_d_results.d3_tracking_jitter:.6f}",
        flush=True,
    )
    if not module_d_results.passed:
        print("Module D quantitative checks FAILED", flush=True)
        return 1

    # Module G quantitative checks (Phase 2).
    module_g_results = run_module_g_tests()
    print(
        "G1 metrics: "
        f"high_frequency_reduction_ratio={module_g_results.g1_high_frequency_reduction_ratio:.6f}",
        flush=True,
    )
    print(
        "G2 metrics: "
        f"edge_mse={module_g_results.g2_edge_mse:.6f}",
        flush=True,
    )
    print(
        "G3 metrics: "
        f"low_history_radius={module_g_results.g3_low_history_radius:.6f}, "
        f"high_history_radius={module_g_results.g3_high_history_radius:.6f}",
        flush=True,
    )
    if not module_g_results.passed:
        print("Module G quantitative checks FAILED", flush=True)
        return 1

    # S0.1 Entry-point smoke.
    smoke_cmd = base_cmd + [
        "--run",
        "--pipeline",
        pipeline_name,
        "--spatial_denoise",
        "true",
        "--test_case",
        "smoke",
    ]
    if args.headless:
        smoke_cmd += ["--headless", "true"]
    run_command(smoke_cmd, env=env)

    # S0.2 Pass-through equivalence.
    screenshot_off_cmd = base_cmd + [
        "--run",
        "--test_case",
        "screenshot",
        "--clear_screenshots",
        "true",
        "--pipeline",
        pipeline_name,
        "--spatial_denoise",
        "false",
    ]
    if args.headless:
        screenshot_off_cmd += ["--headless", "true"]
    run_command(screenshot_off_cmd, env=env)
    off_path = find_latest_screenshot(
        args.framework, scene_name, pipeline_name)
    off_copy = os.path.join(tempfile.gettempdir(), "sparkle_reblur_s0_off.png")
    Image.open(off_path).save(off_copy)

    screenshot_on_cmd = base_cmd + [
        "--run",
        "--test_case",
        "screenshot",
        "--clear_screenshots",
        "true",
        "--pipeline",
        pipeline_name,
        "--spatial_denoise",
        "true",
    ]
    if args.headless:
        screenshot_on_cmd += ["--headless", "true"]
    run_command(screenshot_on_cmd, env=env)
    on_path = find_latest_screenshot(args.framework, scene_name, pipeline_name)
    max_abs_diff, mean_abs_diff, rmse = compare_images(off_copy, on_path)

    print(f"S0.2 metrics: max_abs_diff={max_abs_diff:.6f}, mean_abs_diff={mean_abs_diff:.6f}, rmse={rmse:.6f}",
          flush=True)
    if max_abs_diff > 0.0:
        print("S0.2 FAILED: pass-through output is not bit-exact", flush=True)
        return 1

    # S0.3 Resize/reset smoke (alternate resolution run).
    resize_cmd = base_cmd + [
        "--run",
        "--pipeline",
        pipeline_name,
        "--spatial_denoise",
        "true",
        "--test_case",
        "smoke",
        "--width",
        "960",
        "--height",
        "540",
    ]
    if args.headless:
        resize_cmd += ["--headless", "true"]
    run_command(resize_cmd, env=env)

    print("ReBLUR test suite PASS", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
