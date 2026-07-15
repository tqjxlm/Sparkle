"""Build once and run the complete desktop validation suite.

The suite owns application orchestration. Individual Python files under tests/ evaluate
specialized outputs, such as screenshot and USD round-trip comparisons.
"""

import argparse
import glob
import os
import subprocess
import sys
import venv

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUPPORTED_FRAMEWORKS = ("glfw", "macos")
DEFAULT_PIPELINES = ("forward", "deferred")


def venv_python():
    venv_dir = os.path.join(REPO, "build_cache", "venv", "test-suite")
    python = os.path.join(venv_dir, "Scripts" if os.name == "nt" else "bin",
                          "python.exe" if os.name == "nt" else "python")
    if not os.path.isfile(python):
        venv.create(venv_dir, with_pip=True)
    return python


def build_command(framework, config):
    return [sys.executable, os.path.join(REPO, "build.py"),
            "--framework", framework, "--config", config]


def preflight_steps():
    return [
        (
            "python unit tests",
            [sys.executable, "-m", "unittest", "discover",
             "-s", "tests/build_system", "-p", "test_*.py"],
        ),
    ]


def app_command(framework, config, software, test_case, test_args):
    if software:
        command = [sys.executable, os.path.join(
            REPO, "dev", "run_without_gpu.py")]
    else:
        command = build_command(framework, config)
        command += ["--skip_build", "--run"]
    return command + ["--test_case", test_case] + list(test_args)


def test_steps(framework, config, software, headless, pipelines, scene,
               other_args, test_python, require_cooked):
    common_args = ["--headless", "true"] if headless else []
    steps = []
    ground_truth_scene = os.path.splitext(os.path.basename(scene))[0] if scene else None

    for pipeline in pipelines:
        screenshot_args = ["--clear_screenshots", "true",
                           "--pipeline", pipeline]
        if scene:
            screenshot_args += ["--scene", scene]
        screenshot_args += common_args + list(other_args)
        steps.append((
            f"screenshot ({pipeline})",
            app_command(framework, config, software,
                        "screenshot", screenshot_args),
        ))

        compare_command = [
            test_python,
            "tests/screenshot/static_render_test.py",
            "--framework",
            framework,
            "--pipeline",
            pipeline,
        ]
        if ground_truth_scene:
            compare_command += ["--scene", ground_truth_scene]
        steps.append((f"compare ({pipeline})", compare_command))

    camera_args = ["--clear_screenshots", "true"] + common_args
    camera_compare = [
        test_python,
        "tests/camera/camera_nudge_test.py",
        "--framework",
        framework,
        "--skip_run",
    ]
    if scene:
        camera_args += ["--scene", scene]

    usd_args = ["--clear_screenshots", "true",
                "--pipeline", "forward"] + common_args
    round_trip_compare = [
        test_python,
        "tests/usd/usd_roundtrip_test.py",
        "--framework",
        framework,
        "--skip_run",
    ]
    if scene:
        usd_args += ["--scene", scene]
        round_trip_compare += ["--scene", scene]

    steps += [
        (
            "camera nudge",
            app_command(framework, config, software,
                        "camera_nudge_return", camera_args),
        ),
        (
            "compare (camera nudge)",
            camera_compare,
        ),
        (
            "usd round trip",
            app_command(
                framework,
                config,
                software,
                "usd_round_trip",
                usd_args,
            ),
        ),
        (
            "compare (round trip)",
            round_trip_compare,
        ),
        (
            "cooker request",
            app_command(framework, config, software,
                        "cooker_request", common_args),
        ),
        (
            "scene load failure",
            app_command(framework, config, software,
                        "scene_load_failure", common_args),
        ),
        (
            "render target pool",
            app_command(framework, config, software,
                        "render_target_pool", common_args),
        ),
        (
            "pipeline switch pool",
            app_command(framework, config, software,
                        "pipeline_switch_pool", common_args),
        ),
    ]

    # parity needs a physical GPU producer (a software rasterizer would compare CPU to
    # CPU), and its deliberate artifact recook would trip the --require_cooked gate
    if framework == "macos" and not software and not require_cooked:
        steps.append((
            "ibl parity",
            app_command(framework, config, software,
                        "ibl_parity", common_args),
        ))

    return steps


def log_pattern(framework):
    if framework == "glfw":
        return os.path.join(REPO, "build_system", "glfw", "output", "build",
                            "generated", "logs", "*.log")
    if framework == "macos":
        return os.path.expanduser("~/Documents/sparkle/logs/*.log")
    raise ValueError(f"Unsupported framework: {framework}")


def snapshot_logs(framework):
    return {path: os.path.getsize(path)
            for path in glob.glob(log_pattern(framework))}


def cook_gate(framework, previous_sizes):
    sys.path.insert(0, os.path.join(REPO, "dev"))
    from cook_log import count_cook_activity

    logs = glob.glob(log_pattern(framework))
    if not logs:
        return False, "no app logs found"

    cooked = 0
    hits = 0
    for log in logs:
        offset = previous_sizes.get(log, 0)
        if os.path.getsize(log) < offset:
            offset = 0
        with open(log, errors="replace") as log_file:
            log_file.seek(offset)
            log_hits, log_cooked = count_cook_activity(log_file)
            hits += log_hits
            cooked += log_cooked

    if cooked:
        return False, f"{cooked} on-the-fly cook(s): the packaged cooked content was not used"
    return True, f"{hits} cook artifact hits, no on-the-fly cooking"


def run_step(name, command):
    print(f"\n=== {name}: {' '.join(command)}", flush=True)
    code = subprocess.run(command, cwd=REPO).returncode
    return name, code == 0, f"exit {code}"


def print_summary(results):
    print("\n=== summary")
    failed = 0
    for name, passed, detail in results:
        print(f"  {'PASS' if passed else 'FAIL'}  {name} ({detail})")
        failed += not passed
    return failed


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", default="glfw",
                        choices=SUPPORTED_FRAMEWORKS)
    parser.add_argument("--config", default="Release",
                        choices=("Debug", "Release"))
    parser.add_argument("--pipeline", dest="pipelines", action="append",
                        choices=("forward", "deferred", "gpu", "cpu"),
                        help="screenshot pipeline to test; repeat to test multiple pipelines")
    parser.add_argument("--scene",
                        help="scene file path; its filename selects screenshot ground truth")
    parser.add_argument("--software", action="store_true",
                        help="use Mesa Lavapipe on Windows")
    parser.add_argument("--headless", action="store_true",
                        help="run application test cases without a window")
    parser.add_argument("--skip_build", action="store_true",
                        help="test an existing build without rebuilding")
    parser.add_argument("--require_cooked", action="store_true",
                        help="fail if any test cooks on the fly")
    parser.add_argument("--case", dest="cases", action="append",
                        help="run only the named suite steps; repeat to select multiple")
    return parser.parse_known_args()


def main():
    args, other_args = parse_args()
    if args.software and args.framework != "glfw":
        print("ERROR: --software is only supported with --framework glfw")
        return 1

    results = []
    for name, command in preflight_steps():
        result = run_step(name, command)
        results.append(result)
        if not result[1]:
            print_summary(results)
            return 1

    if not args.skip_build:
        build_result = run_step(
            "build", build_command(args.framework, args.config))
        results.append(build_result)
        if not build_result[1]:
            print_summary(results)
            return 1

    previous_log_sizes = snapshot_logs(args.framework)
    pipelines = args.pipelines or list(DEFAULT_PIPELINES)
    steps = test_steps(
        framework=args.framework,
        config=args.config,
        software=args.software,
        headless=args.headless,
        pipelines=pipelines,
        scene=args.scene,
        other_args=other_args,
        test_python=venv_python(),
        require_cooked=args.require_cooked,
    )

    if args.cases:
        step_names = [name for name, _ in steps]
        unknown = [case for case in args.cases if case not in step_names]
        if unknown:
            print(f"ERROR: unknown --case {unknown}; available: {step_names}")
            return 1
        steps = [(name, command) for name, command in steps if name in args.cases]

    for name, command in steps:
        results.append(run_step(name, command))

    if args.require_cooked:
        passed, detail = cook_gate(args.framework, previous_log_sizes)
        print(f"\n=== cook gate: {detail}", flush=True)
        results.append(("cook gate", passed, detail))

    return 1 if print_summary(results) else 0


if __name__ == "__main__":
    sys.exit(main())
