"""Next event estimation parity evaluator.

The nee_parity test case screenshots the converged gpu path tracer without and with next
event estimation. Both estimate the same image, so their energy must agree. Block means of
the linear radiance average the Monte Carlo noise away and keep a bias, which a per-pixel
metric would bury under the noise of a low sample count. The mean over the blocks gates a
bias of the whole image (the sky's light), their 99th percentile one confined to a few
blocks (the surroundings of an emitter).
"""

import argparse
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)
import render_test_support  # noqa: E402

REFERENCE_NAME = "nee_parity_off.png"
NEE_NAME = "nee_parity_on.png"

BLOCK_SIZE = 16
# 8 bit levels from here on sit on the tone curve's shoulder, where one level spans too much radiance
SATURATED = 250 / 255
# the open NEE items in docs/TODO.md leave about 0.015 and 0.06
MAX_MEAN_DEVIATION = 0.04
MAX_LOCAL_DEVIATION = 0.12


def to_linear(srgb):
    """Undo the sRGB encoding and the tone curve of shaders/screen/tone_mapping.ps.slang at exposure 1."""
    import numpy as np

    y = np.where(srgb <= 0.04045, srgb / 12.92, ((srgb + 0.055) / 1.055) ** 2.4)
    a, b, c, d, e = 2.51, 0.03, 2.43, 0.59, 0.14
    return (-(b - d * y) + np.sqrt((b - d * y) ** 2 + 4 * (a - c * y) * e * y)) / (2 * (a - c * y))


def block_means(image):
    height = image.shape[0] // BLOCK_SIZE * BLOCK_SIZE
    width = image.shape[1] // BLOCK_SIZE * BLOCK_SIZE
    blocks = image[:height, :width].reshape(height // BLOCK_SIZE, BLOCK_SIZE, width // BLOCK_SIZE, BLOCK_SIZE, -1)
    return blocks.mean(axis=(1, 3, 4))


def block_deviations(reference, nee):
    """Relative energy difference of every block that is unsaturated in both images."""
    import numpy as np

    valid = (block_means(reference) < SATURATED) & (block_means(nee) < SATURATED)
    return np.abs(block_means(to_linear(nee))[valid] / block_means(to_linear(reference))[valid] - 1)


def main():
    render_test_support.install_dependencies()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", required=True, choices=render_test_support.SUPPORTED_FRAMEWORKS)
    args = parser.parse_args()

    import numpy as np
    from PIL import Image

    screenshot_dir = render_test_support.get_screenshot_dir(args.framework)
    images = []
    for name in (REFERENCE_NAME, NEE_NAME):
        path = os.path.join(screenshot_dir, name)
        if not os.path.isfile(path):
            print(f"FAIL: screenshot not found: {path}", flush=True)
            return 1
        images.append(np.asarray(Image.open(path).convert("RGB")).astype(np.float64) / 255)

    deviations = block_deviations(*images)
    mean, local = float(deviations.mean()), float(np.percentile(deviations, 99))
    print(f"block energy deviation: mean {mean:.4f} (max {MAX_MEAN_DEVIATION}), "
          f"99th percentile {local:.4f} (max {MAX_LOCAL_DEVIATION})", flush=True)
    if mean <= MAX_MEAN_DEVIATION and local <= MAX_LOCAL_DEVIATION:
        print("PASS", flush=True)
        return 0

    print("FAIL: next event estimation changes the image's energy", flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
