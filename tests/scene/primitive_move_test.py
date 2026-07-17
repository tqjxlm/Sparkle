"""Primitive move functional test.

Runs the app with --test_case primitive_move, which screenshots the freshly loaded scene,
moves one mesh, and screenshots again. The images must differ: identical screenshots mean
the renderer kept using stale acceleration structures for the moved primitive.
"""

import argparse
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))

sys.path.insert(0, os.path.join(PROJECT_ROOT, "tests", "rendering"))
import render_test_support  # noqa: E402

BEFORE_NAME = "primitive_move_before.png"
AFTER_NAME = "primitive_move_after.png"

FLIP_THRESHOLD = 0.005


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run the primitive move test case and require a visible difference.")
    parser.add_argument("--framework", required=True,
                        choices=render_test_support.SUPPORTED_FRAMEWORKS)
    parser.add_argument("--scene")
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--skip_build", action="store_true")
    parser.add_argument("--skip_run", action="store_true",
                        help="Skip running the app (use existing screenshots)")

    return parser.parse_known_args()


def build_and_run(args, other_args):
    run_py = os.path.join(PROJECT_ROOT, "run.py")
    run_cmd = [sys.executable, run_py, "--framework", args.framework]
    if args.skip_build:
        run_cmd.append("--skip_build")
    run_cmd += ["--test_case", "primitive_move",
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

    move_flip = render_test_support.compare_images(before, after)
    if move_flip > FLIP_THRESHOLD:
        print("PASS", flush=True)
        return 0

    print(f"FAIL: before vs after mean FLIP error {move_flip:.4f} <= {FLIP_THRESHOLD}; "
          "the moved primitive did not change the render")
    return 1


if __name__ == "__main__":
    sys.exit(main())
