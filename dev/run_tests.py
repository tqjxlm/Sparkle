"""Build once and run the coverage-selected desktop validation suite.

tests/registry.json registers the runnable cases: each names a C++ TestCase, the
exact arguments to run it with, and an optional Python evaluator. tests/coverage.json
assigns each CI triplet (host-framework-config) the registry cases it must run. The
suite runs the current triplet's assignment; --case runs registry cases by name
regardless of coverage.
"""

import argparse
import glob
import json
import os
import subprocess
import sys
import venv

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGISTRY = os.path.join(REPO, "tests", "registry.json")
COVERAGE = os.path.join(REPO, "tests", "coverage.json")
SUPPORTED_FRAMEWORKS = ("glfw", "macos")
HOST_NAMES = {"darwin": "macos", "win32": "windows"}


def venv_python():
    venv_dir = os.path.join(REPO, "build_cache", "venv", "test-suite")
    python = os.path.join(venv_dir, "Scripts" if os.name == "nt" else "bin",
                          "python.exe" if os.name == "nt" else "python")
    if not os.path.isfile(python):
        venv.create(venv_dir, with_pip=True)
    return python


def build_command(framework, config):
    # explicit stages: the suite needs a runnable build with cooked content, not a package
    return [sys.executable, os.path.join(REPO, "build.py"),
            "--framework", framework, "--config", config,
            "--stage", "build", "--stage", "cook"]


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
        command = [sys.executable, os.path.join(REPO, "run.py"),
                   "--framework", framework, "--config", config, "--skip_build"]
    return command + ["--test_case", test_case] + list(test_args)


def load_registry():
    with open(REGISTRY) as registry_file:
        return json.load(registry_file)


def load_coverage():
    with open(COVERAGE) as coverage_file:
        return json.load(coverage_file)


def current_triplet(framework, config):
    host = HOST_NAMES.get(sys.platform, "linux")
    return f"{host}-{framework}-{config.lower()}"


def covered_cases(registry, coverage, triplet):
    by_name = {case["name"]: case for case in registry}
    return [by_name[name] for name in coverage.get(triplet, [])]


def expand(template_args, context):
    return [arg.format(**context) for arg in template_args]


def test_steps(cases, framework, config, software, headless, scene,
               other_args, test_python):
    context = {
        "framework": framework,
        "scene": scene,
        "scene_stem": os.path.splitext(os.path.basename(scene))[0] if scene else None,
    }
    common_args = ["--headless", "true"] if headless else []
    steps = []

    for case in cases:
        app_args = expand(case.get("app_args", []), context)
        if scene:
            app_args += expand(case.get("scene_args", []), context)
        app_args += common_args + list(other_args)
        steps.append((
            case["name"],
            app_command(framework, config, software,
                        case["test_case"], app_args),
        ))

        evaluator = case.get("evaluator")
        if evaluator:
            command = [test_python, evaluator["script"]]
            command += expand(evaluator["args"], context)
            if scene:
                command += expand(evaluator.get("scene_args", []), context)
            steps.append((f"{case['name']} (compare)", command))

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
                        help="run only the named registry cases regardless of coverage;"
                        " repeat to select multiple")
    return parser.parse_known_args()


def select_cases(registry, coverage, args):
    if args.cases:
        known = [case["name"] for case in registry]
        unknown = [case for case in args.cases if case not in known]
        if unknown:
            print(f"ERROR: unknown --case {unknown}; available: {known}")
            return None
        cases = [case for case in registry if case["name"] in args.cases]
    else:
        triplet = current_triplet(args.framework, args.config)
        cases = covered_cases(registry, coverage, triplet)
        if not cases:
            print(f"ERROR: triplet {triplet} has no coverage;"
                  f" covered: {list(coverage)}; use --case to run registry cases explicitly")
            return None
        print(f"=== triplet {triplet}: {[case['name'] for case in cases]}")

    if args.require_cooked:
        recooking = [case["name"] for case in cases if case.get("recooks")]
        if recooking:
            print(f"skipping {recooking}: their deliberate recook would trip the cook gate")
            cases = [case for case in cases if not case.get("recooks")]

    if not cases:
        print("ERROR: no test cases left to run")
        return None
    return cases


def main():
    args, other_args = parse_args()
    if args.software and args.framework != "glfw":
        print("ERROR: --software is only supported with --framework glfw")
        return 1

    cases = select_cases(load_registry(), load_coverage(), args)
    if cases is None:
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
    steps = test_steps(
        cases=cases,
        framework=args.framework,
        config=args.config,
        software=args.software,
        headless=args.headless,
        scene=args.scene,
        other_args=other_args,
        test_python=venv_python(),
    )

    for name, command in steps:
        results.append(run_step(name, command))

    if args.require_cooked:
        passed, detail = cook_gate(args.framework, previous_log_sizes)
        print(f"\n=== cook gate: {detail}", flush=True)
        results.append(("cook gate", passed, detail))

    return 1 if print_summary(results) else 0


if __name__ == "__main__":
    sys.exit(main())
