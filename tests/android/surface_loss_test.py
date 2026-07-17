"""Compare the screenshots taken before and after a native window loss.

The app run (--test_case surface_loss_recovery) produces the screenshots; the
harness destroys and restores the window in between. Both frames come from the
same run and configuration, so the comparison threshold is near zero.
"""

import argparse
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))

sys.path.insert(0, os.path.join(PROJECT_ROOT, "tests", "rendering"))
import render_test_support  # noqa: E402

BEFORE_NAME = "surface_loss_before.png"
AFTER_NAME = "surface_loss_after.png"

FLIP_THRESHOLD = 0.01


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", required=True,
                        choices=render_test_support.SUPPORTED_FRAMEWORKS)
    args = parser.parse_args()

    render_test_support.install_dependencies()

    screenshot_dir = render_test_support.get_screenshot_dir(args.framework)
    paths = [os.path.join(screenshot_dir, name)
             for name in (BEFORE_NAME, AFTER_NAME)]
    for path in paths:
        if not os.path.isfile(path):
            print(f"FAIL: screenshot not found: {path}", flush=True)
            return 1
        print(f"Found screenshot: {path}", flush=True)

    mean_flip = render_test_support.compare_images(*paths)
    if mean_flip <= FLIP_THRESHOLD:
        print("PASS", flush=True)
        return 0

    print(f"FAIL: before vs after mean FLIP error {mean_flip:.4f} > {FLIP_THRESHOLD}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
