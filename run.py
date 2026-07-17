"""Build and launch the app: the development entry point for running sparkle.

Runs the pipeline stages a launch consumes — build, cook, and for device
frameworks package, whose product the run installs — then launches the app.
Every other option passes through: build options to build.py, the rest to
the app. With --skip_build no stage runs and the existing binary launches.
"""

import argparse
import os
import sys

import build


def launch_stages(framework):
    """The stages a launch consumes: desktop runs read the cook output from the
    build tree directly, device runs install the packaged product."""
    stages = ["build"]
    if build.cooker_framework(framework) is not None:
        stages.append("cook")
    if framework in build.DEVICE_FRAMEWORKS:
        stages.append("package")
    return stages


def parse_run_args(args=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip_build", action="store_true",
                        help="Launch the existing binary without running any pipeline stage")
    parsed_args, forwarded = parser.parse_known_args(args)

    if any(arg == "--stage" or arg.startswith("--stage=") for arg in forwarded):
        parser.error("--stage belongs to build.py; run.py always runs the stages a launch needs")

    return parsed_args, forwarded


def main():
    os.chdir(build.SCRIPTPATH)

    run_args, forwarded = parse_run_args()

    args = build.parse_args(forwarded)
    args["stages"] = [] if run_args.skip_build else launch_stages(args["framework"])

    build.setup(args)
    build.check_environment(args)

    if args["stages"]:
        build.build_project(args)

    from build_system.builder_factory import create_builder

    print("Running...")
    exit_code = create_builder(args["framework"]).run(args)
    sys.exit(exit_code or 0)


if __name__ == "__main__":
    main()
