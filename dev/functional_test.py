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
DEFAULT_SCENE = "TestScene"

PSNR_THRESHOLD = 30.0
SSIM_THRESHOLD = 0.95
SSIM_LOW_THRESHOLD = 0.85


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


def compute_ssim(img_a, img_b, window_size=11):
    """Compute mean SSIM between two images using windowed statistics."""
    import numpy as np

    C1 = (0.01 * 255) ** 2
    C2 = (0.03 * 255) ** 2

    a = img_a.astype(np.float64)
    b = img_b.astype(np.float64)
    w = window_size

    def box_sum(img):
        s = np.cumsum(np.cumsum(img, axis=0), axis=1)
        padded = np.zeros((s.shape[0] + 1, s.shape[1] + 1))
        padded[1:, 1:] = s
        return (padded[w:, w:]
                - padded[:-w, w:]
                - padded[w:, :-w]
                + padded[:-w, :-w])

    n = w * w
    mu_a = box_sum(a) / n
    mu_b = box_sum(b) / n

    sigma_a_sq = box_sum(a * a) / n - mu_a * mu_a
    sigma_b_sq = box_sum(b * b) / n - mu_b * mu_b
    sigma_ab = box_sum(a * b) / n - mu_a * mu_b

    ssim_map = ((2 * mu_a * mu_b + C1) * (2 * sigma_ab + C2)) / \
               ((mu_a ** 2 + mu_b ** 2 + C1) * (sigma_a_sq + sigma_b_sq + C2))

    return float(np.mean(ssim_map)), float(np.percentile(ssim_map, 1))


def compare_images(path_a, path_b):
    from PIL import Image
    import numpy as np

    img_a = Image.open(path_a).convert("RGB")
    img_b = Image.open(path_b).convert("RGB")

    if img_a.size != img_b.size:
        print(
            f"FAIL: image size mismatch — screenshot {img_a.size} vs ground truth {img_b.size}")
        sys.exit(1)

    arr_a = np.array(img_a, dtype=np.float64)
    arr_b = np.array(img_b, dtype=np.float64)

    diff = np.abs(arr_a - arr_b)
    min_diff = int(diff.min())
    max_diff = int(diff.max())
    mse = float(np.mean((arr_a - arr_b) ** 2))
    psnr = float("inf") if mse == 0 else 10.0 * math.log10(255.0 * 255.0 / mse)

    # Per-channel SSIM, averaged across R, G, B
    ssim_means = []
    ssim_lows = []
    for ch in range(3):
        mean_val, low_val = compute_ssim(arr_a[:, :, ch], arr_b[:, :, ch])
        ssim_means.append(mean_val)
        ssim_lows.append(low_val)
    ssim = sum(ssim_means) / len(ssim_means)
    ssim_low = min(ssim_lows)

    print(f"  Min pixel diff: {min_diff}")
    print(f"  Max pixel diff: {max_diff}")
    print(f"  MSE:            {mse:.4f}")
    print(f"  PSNR:           {psnr:.2f} dB")
    print(f"  SSIM:           {ssim:.4f}  (p1: {ssim_low:.4f})")

    return psnr, ssim, ssim_low


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

    print("Downloading ground truth...")
    ground_truth = download_ground_truth(
        args.framework, scene, args.pipeline)

    print("Comparing images...")
    psnr, ssim, ssim_low = compare_images(screenshot, ground_truth)

    passed = True
    if psnr < PSNR_THRESHOLD:
        print(f"FAIL: PSNR {psnr:.2f} dB < {PSNR_THRESHOLD} dB")
        passed = False
    if ssim < SSIM_THRESHOLD:
        print(f"FAIL: SSIM {ssim:.4f} < {SSIM_THRESHOLD}")
        passed = False
    if ssim_low < SSIM_LOW_THRESHOLD:
        print(f"FAIL: SSIM p1 {ssim_low:.4f} < {SSIM_LOW_THRESHOLD}")
        passed = False

    if passed:
        print("PASS")
        return 0

    print("FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
