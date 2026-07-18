"""Neutral environment and image helpers shared by rendering test evaluators."""

import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))

SUPPORTED_FRAMEWORKS = ("glfw", "macos", "android", "ios")


def get_screenshot_dir(framework):
    if framework == "glfw":
        return os.path.join(PROJECT_ROOT, "build_system", "glfw", "output", "build", "generated", "screenshots")
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/screenshots")
    if framework == "android":
        # the android runner pulls the device screenshots here after each run
        return os.path.join(PROJECT_ROOT, "build_system", "android", "output", "device", "screenshots")
    if framework == "ios":
        # the ios simulator runner copies the app screenshots here after each run
        return os.path.join(PROJECT_ROOT, "build_system", "ios", "output", "device", "screenshots")
    raise ValueError(f"Unsupported framework: {framework}")


def find_screenshot(framework):
    path = os.path.join(get_screenshot_dir(framework), "screenshot.png")
    if not os.path.isfile(path):
        raise FileNotFoundError(f"Screenshot not found: {path}")

    print(f"Found screenshot: {path}", flush=True)
    return path


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

    with open(requirements, encoding="utf-8") as requirements_file:
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

    command = [sys.executable, "-m", "pip", "install", "-r", requirements]
    try:
        subprocess.check_call(command, stdout=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        print("pip install refused (externally-managed env); retrying with --break-system-packages", flush=True)
        subprocess.check_call(
            command + ["--break-system-packages"], stdout=subprocess.DEVNULL)
