import os
import platform
import subprocess
import sys
import argparse
import shutil
from pathlib import Path

from build_system.prerequisites import find_cmake, find_and_set_vulkan_sdk

SCRIPT = os.path.abspath(__file__)
SCRIPTPATH = os.path.dirname(SCRIPT)


def construct_additional_cmake_options(parsed_args):
    profile_settings = "-DENABLE_PROFILER=ON" if parsed_args.profile else "-DENABLE_PROFILER=OFF"
    shader_debug_settings = "-DSHADER_DEBUG=ON" if parsed_args.shader_debug else "-DSHADER_DEBUG=OFF"
    asan_settings = "-DENABLE_ASAN=ON" if parsed_args.asan else "-DENABLE_ASAN=OFF"
    cmake_options = [profile_settings, shader_debug_settings, asan_settings]

    return cmake_options


def parse_args(args=None):
    parser = argparse.ArgumentParser(
        description="Parse build arguments for sparkle.")

    parser.add_argument("--framework", default="glfw",
                        choices=["glfw", "macos", "ios", "android"], help="Build framework")
    parser.add_argument("--config", default="Debug",
                        choices=["Release", "Debug"], help="Build configuration")
    parser.add_argument("--asan", action="store_true",
                        help="Enable AddressSanitizer")
    parser.add_argument("--profile", action="store_true",
                        help="Enable profiler")
    parser.add_argument("--shader_debug", action="store_true",
                        help="Enable shader debug")
    parser.add_argument("--generate_only", action="store_true",
                        help="Generate native project (vs sln, xcode project, etc..) without building or running")
    parser.add_argument("--clangd", action="store_true",
                        help="Generate compile_commands.json for clangd")
    parser.add_argument("--run", action="store_true",
                        help="Run the built executable after building")
    parser.add_argument("--setup-only", action="store_true",
                        help="Run setup only without building")
    parser.add_argument("--clean", action="store_true",
                        help="Clean output directory before configure")

    # Unknown args will be used for --run the built executable
    parsed_args, unknown_args = parser.parse_known_args(args)

    return {
        "framework": parsed_args.framework,
        "config": parsed_args.config,
        "run": parsed_args.run,
        "cmake_options": construct_additional_cmake_options(parsed_args),
        "unknown_args": unknown_args,
        "generate_only": parsed_args.generate_only,
        "setup_only": parsed_args.setup_only,
        "clangd": parsed_args.clangd,
        "clean": parsed_args.clean,
    }


def check_environment(args):
    # Check CMake availability (skip for Android as it uses NDK's built-in CMake)
    if args["framework"] != "android":
        cmake_executable = find_cmake()
        print(f"Using CMake: {cmake_executable}")
        args["cmake_executable"] = cmake_executable

    find_and_set_vulkan_sdk()

    # Exit if framework is macos or ios but not running on macOS
    if args["framework"] in ("macos", "ios") and sys.platform != "darwin":
        print(
            f"Error: Framework '{args['framework']}' requires macOS, but current system is not macOS.")
        raise Exception()


def run_git_submodule_update():
    print("Updating git submodules...")
    subprocess.run(
        ["git", "submodule", "update", "--init", "--recursive"],
        check=True
    )


def setup():
    from resources.setup import setup_resources
    from ide.setup import setup_ide

    run_git_submodule_update()
    setup_resources()
    setup_ide()

    print("Setup complete.")


def copy_build_products(product_archive_path, args):
    """Copy build products to the product directory for the specified framework."""
    product_dir = os.path.join(
        SCRIPTPATH, "build_system", args['framework'], "product")

    os.makedirs(product_dir, exist_ok=True)

    if platform.system() == "Windows":
        system_name = "windows"
    elif platform.system() == "Linux":
        system_name = "linux"
    elif platform.system() == "Darwin":
        system_name = "macos"
    else:
        raise Exception()
    extension = ''.join(Path(product_archive_path).suffixes)
    product_final_name = f"{system_name}-{args['framework']}-{args['config']}{extension}"

    product_final_path = os.path.join(product_dir, product_final_name)

    if os.path.exists(product_final_path):
        os.remove(product_final_path)

    shutil.copy(product_archive_path, product_final_path)
    print(f"Build products copied to {product_final_path}")


def build_project(args):
    """Build the project after setup is complete."""
    print("Starting build process...")

    if args["framework"] == "glfw":
        import build_system.glfw.build as builder

        if args["clangd"]:
            builder.configure(args, False)
        elif args["generate_only"]:
            builder.generate_project(args)
        else:
            product_path = builder.build_and_run(args)
            copy_build_products(product_path, args)

    elif args["framework"] == "macos":
        import build_system.macos.build as builder

        if args["clangd"]:
            builder.configure_for_clangd(args)
        elif args["generate_only"]:
            builder.generate_project(args)
        else:
            product_path = builder.build_and_run(args)
            copy_build_products(product_path, args)

    elif args["framework"] == "ios":
        import build_system.ios.build as builder

        if args["clangd"]:
            builder.configure_for_clangd(args)
        elif args["generate_only"]:
            builder.generate_project(args)
        else:
            product_path = builder.build_and_run(args)
            copy_build_products(product_path, args)

    elif args["framework"] == "android":
        import build_system.android.build as builder

        if args["clangd"] or args["generate_only"]:
            # gradle sync will do both in the same time
            builder.sync_only(args)
        else:
            product_path = builder.build_and_run(args)
            copy_build_products(product_path, args)


def main():
    os.chdir(SCRIPTPATH)

    args = parse_args()

    # Always run setup first
    setup()

    # If --setup-only flag is used, exit after setup
    if args["setup_only"]:
        print("Setup-only mode: Skipping build.")
        return

    check_environment(args)

    # Run build process
    build_project(args)


if __name__ == "__main__":
    main()
