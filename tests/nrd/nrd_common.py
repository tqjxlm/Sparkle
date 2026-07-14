"""Shared helpers for the NRD gate scripts: one sweep-launch protocol + image utilities.

Every gate threshold is calibrated against load_image()'s [0,1] float decode and lum()'s Rec.601
weights; change them here (and recalibrate), never per script.
"""

import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, os.path.join(PROJECT_ROOT, "tests", "rendering"))
import render_test_support  # noqa: E402,F401

NUM_FRAMES = 16  # must match DenoiserSweepTest.cpp NumFrames


def run_sweep(framework, extra, skip_build=False, headless=False):
    cmd = [sys.executable, os.path.join(PROJECT_ROOT, "build.py"), "--framework", framework]
    if skip_build:
        cmd.append("--skip_build")
    cmd += ["--run", "--test_case", "denoiser_sweep", "--clear_screenshots", "true", "--pipeline", "gpu"]
    cmd += list(extra)
    if headless:
        cmd += ["--headless", "true"]
    print("Running:", " ".join(cmd), flush=True)
    if subprocess.run(cmd, cwd=PROJECT_ROOT).returncode != 0:
        sys.exit(1)


def run_test_case(framework, test_case, extra, skip_build=False, headless=False):
    cmd = [sys.executable, os.path.join(PROJECT_ROOT, "build.py"),
           "--framework", framework]
    if skip_build:
        cmd.append("--skip_build")
    cmd += ["--run", "--test_case", test_case] + list(extra)
    if headless:
        cmd += ["--headless", "true"]
    print("Running:", " ".join(cmd), flush=True)
    if subprocess.run(cmd, cwd=PROJECT_ROOT).returncode != 0:
        sys.exit(1)


def load_image(path):
    import numpy as np
    from PIL import Image

    return np.asarray(Image.open(path).convert("RGB"), np.float64) / 255.0


def load_sweep_frames(directory, prefix="denoiser_sweep"):
    import numpy as np

    return np.stack([load_image(os.path.join(directory, f"{prefix}_{k}.png")) for k in range(NUM_FRAMES)])


def lum(rgb):
    return 0.299 * rgb[..., 0] + 0.587 * rgb[..., 1] + 0.114 * rgb[..., 2]
