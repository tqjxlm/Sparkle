"""Validate an existing static render against the published ground truth."""

import argparse
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, os.path.join(PROJECT_ROOT, "tests", "rendering"))
from render_test_support import (  # noqa: E402
    SUPPORTED_FRAMEWORKS,
    compare_images,
    find_screenshot,
    install_dependencies,
)
from static_render_reference import (  # noqa: E402
    DEFAULT_SCENE,
    FLIP_THRESHOLD,
    download_ground_truth,
)


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
