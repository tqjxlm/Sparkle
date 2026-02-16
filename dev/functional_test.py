"""Post-build functional test: run app, capture screenshot, compare against ground truth."""

import argparse
import glob
import math
import os
import platform
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)

GROUND_TRUTH_URL_BASE = "https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle"

SUPPORTED_FRAMEWORKS = ("glfw", "macos")
SUPPORTED_PIPELINES = ("gpu", "forward", "deferred")

DEFAULT_SCENE = "TestScene"
PSNR_THRESHOLD = 30.0


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run an already-built app, capture a screenshot, and compare against ground truth.")
    parser.add_argument("--framework", required=True,
                        choices=SUPPORTED_FRAMEWORKS)
    parser.add_argument("--pipeline", required=True,
                        choices=SUPPORTED_PIPELINES)
    parser.add_argument("--scene", default=DEFAULT_SCENE)
    parser.add_argument("--skip_run", action="store_true",
                        help="Skip running the app (use existing screenshot)")
    parser.add_argument("--software", action="store_true",
                        help="Use Mesa Lavapipe software Vulkan rendering (Windows only)")
    return parser.parse_args()


def get_executable(framework):
    if framework == "glfw":
        exe_name = "sparkle.exe" if platform.system() == "Windows" else "sparkle"
        build_dir = os.path.join(
            PROJECT_ROOT, "build_system", "glfw", "output", "build")
        exe_path = os.path.join(build_dir, exe_name)
        return exe_path, build_dir
    if framework == "macos":
        app_path = os.path.join(
            PROJECT_ROOT, "build_system", "macos", "output", "build", "sparkle.app")
        exe_path = os.path.join(app_path, "Contents", "MacOS", "sparkle")
        return exe_path, None
    raise ValueError(f"Unsupported framework: {framework}")


def run_app(framework, pipeline, scene, env=None):
    exe_path, cwd = get_executable(framework)
    if not os.path.exists(exe_path):
        print(f"Executable not found: {exe_path}")
        print("Build the project first with: python3 build.py --framework " + framework)
        sys.exit(1)

    run_cmd = [exe_path, "--auto_screenshot", "true", "--pipeline", pipeline]
    if scene != DEFAULT_SCENE:
        run_cmd += ["--scene", scene]

    if env is None:
        env = os.environ.copy()

    print(f"Running: {' '.join(run_cmd)}")
    result = subprocess.run(run_cmd, cwd=cwd, env=env)
    if result.returncode != 0:
        print(f"App exited with code {result.returncode}")
        sys.exit(1)


def get_screenshot_dir(framework):
    if framework == "glfw":
        return os.path.join(PROJECT_ROOT, "build_system", "glfw", "output", "build", "generated", "screenshots")
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    raise ValueError(f"Unsupported framework: {framework}")


def find_screenshot(framework, scene, pipeline):
    screenshot_dir = get_screenshot_dir(framework)
    pattern = os.path.join(screenshot_dir, f"{scene}_{pipeline}_*.png")
    matches = glob.glob(pattern)
    if not matches:
        print(f"No screenshot found matching: {pattern}")
        sys.exit(1)

    matches.sort(key=os.path.getmtime, reverse=True)
    chosen = matches[0]
    print(f"Found screenshot: {chosen}")
    return chosen


def download_ground_truth(framework, scene, pipeline):
    filename = f"{scene}_{pipeline}_{framework}.png"
    url = f"{GROUND_TRUTH_URL_BASE}/{filename}"

    sys.path.insert(0, PROJECT_ROOT)
    from build_system.utils import download_file

    dest = os.path.join(tempfile.gettempdir(),
                        f"sparkle_ground_truth_{filename}")
    download_file(url, dest)
    return dest


def compare_images(path_a, path_b):
    try:
        from PIL import Image
    except ImportError:
        print("Pillow not found, installing...")
        subprocess.check_call(
            [sys.executable, "-m", "pip", "install", "Pillow"])
        from PIL import Image

    img_a = Image.open(path_a).convert("RGB")
    img_b = Image.open(path_b).convert("RGB")

    if img_a.size != img_b.size:
        print(
            f"FAIL: image size mismatch — screenshot {img_a.size} vs ground truth {img_b.size}")
        sys.exit(1)

    pixels_a = img_a.tobytes()
    pixels_b = img_b.tobytes()
    num_pixels = len(pixels_a)

    # Per-channel difference stats
    min_diff = 255
    max_diff = 0
    sum_sq = 0
    for a, b in zip(pixels_a, pixels_b):
        d = abs(a - b)
        if d < min_diff:
            min_diff = d
        if d > max_diff:
            max_diff = d
        sum_sq += d * d

    mse = sum_sq / num_pixels

    # Variance of each image (per channel)
    sum_a = sum(pixels_a)
    sum_b = sum(pixels_b)
    mean_a = sum_a / num_pixels
    mean_b = sum_b / num_pixels
    var_a = sum(((v - mean_a) ** 2 for v in pixels_a)) / num_pixels
    var_b = sum(((v - mean_b) ** 2 for v in pixels_b)) / num_pixels

    psnr = float("inf") if mse == 0 else 10.0 * math.log10(255.0 * 255.0 / mse)

    print(f"  Min pixel diff: {min_diff}")
    print(f"  Max pixel diff: {max_diff}")
    print(f"  MSE:            {mse:.4f}")
    print(f"  PSNR:           {psnr:.2f} dB")
    print(f"  Variance (screenshot):    {var_a:.2f}")
    print(f"  Variance (ground truth):  {var_b:.2f}")

    return psnr


def main():
    args = parse_args()

    env = None
    if args.software:
        from run_without_gpu import setup_lavapipe
        env = setup_lavapipe()

    if not args.skip_run:
        run_app(args.framework, args.pipeline, args.scene, env=env)
    else:
        print("Skipping app run, using existing screenshot.")

    screenshot = find_screenshot(args.framework, args.scene, args.pipeline)

    print("Downloading ground truth...")
    ground_truth = download_ground_truth(
        args.framework, args.scene, args.pipeline)

    print("Comparing images...")
    psnr = compare_images(screenshot, ground_truth)

    if psnr >= PSNR_THRESHOLD:
        print("PASS")
        return 0

    print("FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
