import os
import sys
import subprocess
import shutil

from build_system.utils import run_command_with_logging

SCRIPT = os.path.abspath(__file__)
SCRIPTPATH = os.path.dirname(SCRIPT)


def clean_output_directory(output_dir):
    """Clean the output directory if it exists."""
    if os.path.exists(output_dir):
        print(f"Cleaning output directory: {output_dir}")
        shutil.rmtree(output_dir)


def configure_for_clangd(args):
    """
    Configure CMake for clangd in 'clangd' directory.
    """
    output_dir = os.path.join(SCRIPTPATH, "clangd")

    if args.get("clean", False):
        clean_output_directory(output_dir)

    os.makedirs(output_dir, exist_ok=True)
    os.chdir(output_dir)

    compiler_args = "-D CMAKE_C_COMPILER=/usr/bin/clang -D CMAKE_CXX_COMPILER=/usr/bin/clang++"
    generator_args = f"-DCMAKE_BUILD_TYPE={args['config']}"

    cmake_cmd = [
        args["cmake_executable"],
        "../../..",
        generator_args,
        compiler_args,
        args["cmake_options"],
    ]
    cmake_cmd_flat = []
    for part in cmake_cmd:
        if part:
            cmake_cmd_flat.extend(part.split())

    print("Configuring CMake for clangd with command:")
    print(" ".join(cmake_cmd_flat))
    result = subprocess.run(cmake_cmd_flat)
    if result.returncode != 0:
        print("CMake configure failed.")
        sys.exit(1)

    print(f"Configuration complete in {output_dir}")


def generate_project(args):
    """
    Generate Xcode project in 'project' directory (no build).
    """
    output_dir = os.path.join(SCRIPTPATH, "project")

    if args.get("clean", False):
        clean_output_directory(output_dir)

    os.makedirs(output_dir, exist_ok=True)
    os.chdir(output_dir)

    generator_args = "-G Xcode"

    cmake_cmd = [
        args["cmake_executable"],
        "../../..",
        generator_args,
        args["cmake_options"],
    ]
    cmake_cmd_flat = []
    for part in cmake_cmd:
        if part:
            cmake_cmd_flat.extend(part.split())

    print("Generating Xcode project with command:")
    print(" ".join(cmake_cmd_flat))
    result = subprocess.run(cmake_cmd_flat)
    if result.returncode != 0:
        print("CMake project generation failed.")
        sys.exit(1)

    print(f"Xcode project is generated at {output_dir}. Open with command:")
    print(f"open {output_dir}/sparkle.xcodeproj")

    return output_dir


def build_and_run(args):
    """
    Build the project and optionally run it.
    """
    output_dir = generate_project(args)

    # Build specific target instead of ALL_BUILD
    build_cmd = [args["cmake_executable"], "--build", ".", "--config",
                 args["config"], "--target", "sparkle"]
    log_file = os.path.join(output_dir, "build.log")

    run_command_with_logging(build_cmd, log_file, "Building macOS project")

    # Run if requested
    if args["run"]:
        app_name = "sparkle.app"
        if args["config"] == "Debug":
            app_path = os.path.join(output_dir, "Debug", app_name)
        else:
            app_path = os.path.join(output_dir, "Release", app_name)

        if os.path.exists(app_path):
            executable_path = os.path.join(
                app_path, "Contents", "MacOS", "sparkle")
            if os.path.exists(executable_path):
                print(f"Running application: {executable_path}")
                run_cmd = [executable_path] + args["unknown_args"]
                subprocess.run(run_cmd)
            else:
                print(f"Error: Executable not found at {executable_path}")
                sys.exit(1)
        else:
            print(f"Error: Application bundle not found at {app_path}")
            sys.exit(1)

    return output_dir
