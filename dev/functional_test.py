"""Post-build functional test: run app, capture screenshot, compare against ground truth."""

import argparse
import glob
import os
import platform
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)

GROUND_TRUTH_URL_BASE = "https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle"

SUPPORTED_FRAMEWORKS = ("glfw", "macos")
DEFAULT_SCENE = "TestScene"

FLIP_THRESHOLD = 0.1


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run an already-built app, capture a screenshot, and compare against ground truth.")
    parser.add_argument("--framework", required=True,
                        choices=SUPPORTED_FRAMEWORKS)
    parser.add_argument("--pipeline", default="forward")
    parser.add_argument("--scene")
    parser.add_argument("--skip_run", action="store_true",
                        help="Skip running the app (use existing screenshot)")
    parser.add_argument("--software", action="store_true",
                        help="Use Mesa Lavapipe software Vulkan rendering (Windows only)")

    return parser.parse_known_args()


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


def run_app(framework, pipeline, scene, other_args, env=None):
    exe_path, cwd = get_executable(framework)
    if not os.path.exists(exe_path):
        print(f"Executable not found: {exe_path}")
        print("Build the project first with: python3 build.py --framework " + framework)
        sys.exit(1)

    run_cmd = [exe_path, "--auto_screenshot", "true",
               "--pipeline", pipeline] + other_args

    if scene:
        run_cmd += ["--scene", scene]

    if env is None:
        env = os.environ.copy()

    print(f"Running: {' '.join(run_cmd)}", flush=True)
    result = subprocess.run(run_cmd, cwd=cwd, env=env)
    print(f"App exited with code {result.returncode}", flush=True)
    if result.returncode != 0:
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
        print(f"No screenshot found matching: {pattern}", flush=True)
        sys.exit(1)

    matches.sort(key=os.path.getmtime, reverse=True)
    chosen = matches[0]
    print(f"Found screenshot: {chosen}", flush=True)
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


def load_image(path):
    import numpy as np
    from PIL import Image
    # Load as float32 HxWxC in [0,1], RGB (sRGB) as expected by nbflip LDR mode
    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def compare_images(ground_truth, screenshot):
    print(f"Loading ground truth: {ground_truth}", flush=True)
    gt_img = load_image(ground_truth)
    print(f"  Shape: {gt_img.shape}", flush=True)
    print(f"Loading screenshot: {screenshot}", flush=True)
    ss_img = load_image(screenshot)
    print(f"  Shape: {ss_img.shape}", flush=True)
    if gt_img.shape != ss_img.shape:
        print(f"Image size mismatch: {gt_img.shape} vs {ss_img.shape}", flush=True)
        sys.exit(1)
    print("Calling nbflip.evaluate()...", flush=True)
    from flip_evaluator import nbflip
    _, mean_flip, _ = nbflip.evaluate(gt_img, ss_img, False, True, False, True, {})
    print(f"  Mean FLIP error: {mean_flip:.4f}", flush=True)
    return mean_flip


def install_dependencies():
    requirements = os.path.join(SCRIPT_DIR, "requirements.txt")
    subprocess.check_call(
        [sys.executable, "-m", "pip", "install", "-r", requirements],
        stdout=subprocess.DEVNULL)


def main():
    install_dependencies()
    args, unknown_args = parse_args()

    env = None
    if args.software:
        from run_without_gpu import setup_lavapipe
        env = setup_lavapipe()

    if not args.skip_run:
        run_app(args.framework, args.pipeline,
                args.scene, unknown_args, env=env)
    else:
        print("Skipping app run, using existing screenshot.")

    scene = args.scene or DEFAULT_SCENE

    screenshot = find_screenshot(args.framework, scene, args.pipeline)

    print("Downloading ground truth...", flush=True)
    ground_truth = download_ground_truth(
        args.framework, scene, args.pipeline)
    print(f"Ground truth downloaded to: {ground_truth}", flush=True)

    print("Comparing images...", flush=True)
    mean_flip = compare_images(ground_truth, screenshot)

    if mean_flip <= FLIP_THRESHOLD:
        print("PASS", flush=True)
        return 0

    print(f"FAIL: mean FLIP error {mean_flip:.4f} > {FLIP_THRESHOLD}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
