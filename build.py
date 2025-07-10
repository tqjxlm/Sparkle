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
    from build_system.builder_factory import get_supported_frameworks

    parser = argparse.ArgumentParser(
        description="Parse build arguments for sparkle.")

    parser.add_argument("--framework", default="glfw",
                        choices=get_supported_frameworks(), help="Build framework")
    parser.add_argument("--config", default="Debug",
                        choices=["Release", "Debug"], help="Build configuration")
    parser.add_argument("--archive", action="store_true",
                        help="Archive the app for distribution")
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
    parser.add_argument("--setup_only", action="store_true",
                        help="Run setup only without building")
    parser.add_argument("--clean", action="store_true",
                        help="Clean output directory before configure")
    parser.add_argument("--apple_auto_sign", action="store_true",
                        help="Enable automatic code signing for Apple platforms. Requires APPLE_DEVELOPER_TEAM_ID to be set."
                        "See https://developer.apple.com/help/account/manage-your-team/locate-your-team-id/")

    # Unknown args will be used for --run the built executable
    parsed_args, unknown_args = parser.parse_known_args(args)

    return {
        "framework": parsed_args.framework,
        "config": parsed_args.config,
        "archive": parsed_args.archive,
        "run": parsed_args.run,
        "cmake_options": construct_additional_cmake_options(parsed_args),
        "unknown_args": unknown_args,
        "generate_only": parsed_args.generate_only,
        "setup_only": parsed_args.setup_only,
        "clangd": parsed_args.clangd,
        "clean": parsed_args.clean,
        "apple_auto_sign": parsed_args.apple_auto_sign,
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

    from build_system.builder_factory import create_builder

    builder = create_builder(args["framework"])

    if args["clangd"]:
        print("Configuring...")
        builder.configure_for_clangd(args)
    elif args["generate_only"]:
        print("Generating project...")
        builder.generate_project(args)
    else:
        print("Building...")
        builder.build(args)

        if args["archive"]:
            print("Archiving...")
            archive_path = builder.archive(args)
            copy_build_products(archive_path, args)

        if args["run"]:
            print("Running...")
            builder.run(args)


def main():
    os.chdir(SCRIPTPATH)

    args = parse_args()

    # Always run setup first
    setup()

    # If --setup_only flag is used, exit after setup
    if args["setup_only"]:
        print("Setup-only mode: Skipping build.")
        return

    check_environment(args)

    # Run build process
    build_project(args)


if __name__ == "__main__":
    main()
