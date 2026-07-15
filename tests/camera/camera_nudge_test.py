"""Camera nudge-and-return functional test.

Runs the app with --test_case camera_nudge_return, which screenshots the freshly loaded
scene, drags the camera a little and back to its starting point through the mouse input
path, and screenshots again. Both screenshots come from the same run and configuration,
so the ground truth is the before image and the comparison threshold is near zero.
"""

import argparse
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))

sys.path.insert(0, os.path.join(PROJECT_ROOT, "tests", "rendering"))
import render_test_support  # noqa: E402

BEFORE_NAME = "camera_nudge_before.png"
AFTER_NAME = "camera_nudge_after.png"

FLIP_THRESHOLD = 0.01


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run the camera nudge-and-return test case and compare the renders.")
    parser.add_argument("--framework", required=True,
                        choices=render_test_support.SUPPORTED_FRAMEWORKS)
    parser.add_argument("--scene")
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--skip_build", action="store_true")
    parser.add_argument("--skip_run", action="store_true",
                        help="Skip running the app (use existing screenshots)")

    return parser.parse_known_args()


def build_and_run(args, other_args):
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    run_cmd = [sys.executable, build_py, "--framework", args.framework]
    if args.skip_build:
        run_cmd.append("--skip_build")
    run_cmd += ["--run",
                "--test_case", "camera_nudge_return",
                "--clear_screenshots", "true"] + other_args
    if args.headless:
        run_cmd += ["--headless", "true"]
    if args.scene:
        run_cmd += ["--scene", args.scene]

    print(f"Running: {' '.join(run_cmd)}", flush=True)
    result = subprocess.run(run_cmd, cwd=PROJECT_ROOT)
    print(f"App exited with code {result.returncode}", flush=True)
    if result.returncode != 0:
        sys.exit(1)


def find_screenshots(framework):
    screenshot_dir = render_test_support.get_screenshot_dir(framework)
    paths = [os.path.join(screenshot_dir, name)
             for name in (BEFORE_NAME, AFTER_NAME)]
    for path in paths:
        if not os.path.isfile(path):
            print(f"Screenshot not found: {path}", flush=True)
            sys.exit(1)
        print(f"Found screenshot: {path}", flush=True)
    return paths


def main():
    render_test_support.install_dependencies()
    args, unknown_args = parse_args()

    if not args.skip_run:
        build_and_run(args, unknown_args)
    else:
        print("Skipping app run, using existing screenshots.")

    before, after = find_screenshots(args.framework)

    nudge_flip = render_test_support.compare_images(before, after)
    if nudge_flip <= FLIP_THRESHOLD:
        print("PASS", flush=True)
        return 0

    print(f"FAIL: before vs after mean FLIP error {nudge_flip:.4f} > {FLIP_THRESHOLD}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
