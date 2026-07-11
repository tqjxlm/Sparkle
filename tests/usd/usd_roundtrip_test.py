"""USD round-trip functional test.

Runs the app with --test_case usd_round_trip, which renders the loaded scene (TestScene by
default), exports it to USD, loads the exported file back and renders it again. The original
render must match the dev/functional_test.py ground truth (a self-comparison alone would pass
if both renders broke the same way, e.g. all black), and the reimported render must match the
original.
"""

import argparse
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))

sys.path.insert(0, os.path.join(PROJECT_ROOT, "dev"))
import functional_test  # noqa: E402  (reuses screenshot dir/image helpers)

ORIGINAL_NAME = "usd_round_trip_original.png"
REIMPORTED_NAME = "usd_round_trip_reimported.png"

# self-comparison in the same run and configuration, so the bar is stricter than the
# cross-machine ground-truth threshold in dev/functional_test.py
FLIP_THRESHOLD = 0.05


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run the USD round-trip test case and compare original vs reimported renders.")
    parser.add_argument("--framework", required=True,
                        choices=functional_test.SUPPORTED_FRAMEWORKS)
    parser.add_argument("--pipeline", default="forward")
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
                "--test_case", "usd_round_trip",
                "--clear_screenshots", "true",
                "--pipeline", args.pipeline] + other_args
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
    screenshot_dir = functional_test.get_screenshot_dir(framework)
    paths = [os.path.join(screenshot_dir, name)
             for name in (ORIGINAL_NAME, REIMPORTED_NAME)]
    for path in paths:
        if not os.path.isfile(path):
            print(f"Screenshot not found: {path}", flush=True)
            sys.exit(1)
        print(f"Found screenshot: {path}", flush=True)
    return paths


def main():
    functional_test.install_dependencies()
    args, unknown_args = parse_args()

    if not args.skip_run:
        build_and_run(args, unknown_args)
    else:
        print("Skipping app run, using existing screenshots.")

    original, reimported = find_screenshots(args.framework)

    if args.scene:
        print(f"No ground truth for custom scene {args.scene}, skipping ground truth check.", flush=True)
    else:
        print("Downloading ground truth...", flush=True)
        ground_truth = functional_test.download_ground_truth(
            args.framework, functional_test.DEFAULT_SCENE, args.pipeline)
        gt_flip = functional_test.compare_images(ground_truth, original)
        if gt_flip > functional_test.FLIP_THRESHOLD:
            print(f"FAIL: original render vs ground truth mean FLIP error "
                  f"{gt_flip:.4f} > {functional_test.FLIP_THRESHOLD}")
            return 1

    round_trip_flip = functional_test.compare_images(original, reimported)
    if round_trip_flip <= FLIP_THRESHOLD:
        print("PASS", flush=True)
        return 0

    print(f"FAIL: original vs reimported mean FLIP error {round_trip_flip:.4f} > {FLIP_THRESHOLD}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
