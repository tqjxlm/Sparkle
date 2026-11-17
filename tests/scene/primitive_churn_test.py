"""Primitive churn evaluator.

--test_case primitive_churn screenshots a frame whose primitives were added and
removed within one render frame, then the same scene rebuilt over separate
frames. The two screenshots must match: a render pass that drew a slot with the
wrong mesh, or left one out, renders them differently.
"""

import argparse
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))

sys.path.insert(0, os.path.join(PROJECT_ROOT, "tests", "rendering"))
import render_test_support  # noqa: E402

CHURN_NAME = "primitive_churn.png"
REFERENCE_NAME = "primitive_churn_reference.png"

# both screenshots render the same scene state, so only a wrongly drawn mesh tells them apart
FLIP_THRESHOLD = 0.001


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", required=True,
                        choices=render_test_support.SUPPORTED_FRAMEWORKS)
    args = parser.parse_args()

    render_test_support.install_dependencies()

    screenshot_dir = render_test_support.get_screenshot_dir(args.framework)
    churn, reference = (os.path.join(screenshot_dir, name)
                        for name in (CHURN_NAME, REFERENCE_NAME))
    for path in (churn, reference):
        if not os.path.isfile(path):
            print(f"FAIL: screenshot not found: {path}", flush=True)
            return 1

    flip = render_test_support.compare_images(reference, churn)
    if flip <= FLIP_THRESHOLD:
        print("PASS", flush=True)
        return 0

    print(f"FAIL: churned vs rebuilt mean FLIP error {flip:.4f} > {FLIP_THRESHOLD}; "
          "a mesh pass drew the primitives changed within one render frame wrongly", flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
