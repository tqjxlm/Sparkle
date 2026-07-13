"""Validate an existing static render against the published ground truth."""

import argparse
import os
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))

GROUND_TRUTH_URL_BASE = "https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle"
SUPPORTED_FRAMEWORKS = ("glfw", "macos")
DEFAULT_SCENE = "TestScene"
FLIP_THRESHOLD = 0.1


def get_screenshot_dir(framework):
    if framework == "glfw":
        return os.path.join(PROJECT_ROOT, "build_system", "glfw", "output", "build", "generated", "screenshots")
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    raise ValueError(f"Unsupported framework: {framework}")


def find_screenshot(framework):
    path = os.path.join(get_screenshot_dir(framework), "screenshot.png")
    if not os.path.isfile(path):
        raise FileNotFoundError(f"Screenshot not found: {path}")

    print(f"Found screenshot: {path}", flush=True)
    return path


def download_ground_truth(framework, scene, pipeline):
    filename = f"{scene}_{pipeline}_{framework}.png"
    url = f"{GROUND_TRUTH_URL_BASE}/{filename}"

    sys.path.insert(0, PROJECT_ROOT)
    from build_system.utils import download_file

    destination = os.path.join(tempfile.gettempdir(), f"sparkle_ground_truth_{filename}")
    download_file(url, destination)
    return destination


def load_image(path):
    import numpy as np
    from PIL import Image

    return np.array(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def compare_images(ground_truth, screenshot):
    print(f"Loading ground truth: {ground_truth}", flush=True)
    ground_truth_image = load_image(ground_truth)
    print(f"  Shape: {ground_truth_image.shape}", flush=True)
    print(f"Loading screenshot: {screenshot}", flush=True)
    screenshot_image = load_image(screenshot)
    print(f"  Shape: {screenshot_image.shape}", flush=True)
    if ground_truth_image.shape != screenshot_image.shape:
        raise ValueError(
            f"Image size mismatch: {ground_truth_image.shape} vs {screenshot_image.shape}")

    print("Calling nbflip.evaluate()...", flush=True)
    from flip_evaluator import nbflip
    _, mean_flip, _ = nbflip.evaluate(
        ground_truth_image, screenshot_image, False, True, False, True, {})
    print(f"  Mean FLIP error: {mean_flip:.4f}", flush=True)
    return mean_flip


def _requirements_satisfied(requirements):
    import re
    from importlib.metadata import PackageNotFoundError, version

    with open(requirements) as requirements_file:
        for line in requirements_file:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            name = re.split(r"[<>=!~; \[]", line, maxsplit=1)[0]
            try:
                version(name)
            except PackageNotFoundError:
                return False
    return True


def install_dependencies():
    requirements = os.path.join(PROJECT_ROOT, "tests", "requirements.txt")
    if _requirements_satisfied(requirements):
        return

    import subprocess

    command = [sys.executable, "-m", "pip", "install", "-r", requirements]
    try:
        subprocess.check_call(command, stdout=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        print("pip install refused (externally-managed env); retrying with --break-system-packages", flush=True)
        subprocess.check_call(
            command + ["--break-system-packages"], stdout=subprocess.DEVNULL)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", required=True,
                        choices=SUPPORTED_FRAMEWORKS)
    parser.add_argument("--pipeline", default="forward")
    parser.add_argument("--scene", default=DEFAULT_SCENE)
    args = parser.parse_args()

    install_dependencies()

    try:
        screenshot = find_screenshot(args.framework)
        print("Downloading ground truth...", flush=True)
        ground_truth = download_ground_truth(
            args.framework, args.scene, args.pipeline)
        print(f"Ground truth downloaded to: {ground_truth}", flush=True)
        mean_flip = compare_images(ground_truth, screenshot)
    except (FileNotFoundError, ValueError) as error:
        print(f"FAIL: {error}", flush=True)
        return 1

    if mean_flip <= FLIP_THRESHOLD:
        print("PASS", flush=True)
        return 0

    print(f"FAIL: mean FLIP error {mean_flip:.4f} > {FLIP_THRESHOLD}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
