import os
import subprocess
import sys
import argparse
import shutil


def construct_additional_cmake_options(parsed_args):
    profile_settings = "-DENABLE_PROFILER=ON" if parsed_args.profile else "-DENABLE_PROFILER=OFF"
    shader_debug_settings = "-DSHADER_DEBUG=ON" if parsed_args.shader_debug else "-DSHADER_DEBUG=OFF"
    asan_settings = "-DENABLE_ASAN=ON" if parsed_args.asan else "-DENABLE_ASAN=OFF"
    cmake_options = f"{profile_settings} {shader_debug_settings} {asan_settings}"

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
    parser.add_argument("--vcpkg_path", default="D:/SDKs/vcpkg",
                        help="Windows-only: vcpkg installation root path for external dependencies")
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
        "vcpkg_path": parsed_args.vcpkg_path,
        "generate_only": parsed_args.generate_only,
        "setup_only": parsed_args.setup_only,
        "clangd": parsed_args.clangd,
        "clean": parsed_args.clean,
    }


def check_cmake():
    """Check if CMake is available in PATH or CMAKE_PATH environment variable."""
    cmake_path = os.environ.get("CMAKE_PATH")

    if cmake_path:
        # Check if CMAKE_PATH points to cmake executable or directory containing cmake
        if os.path.isfile(cmake_path):
            cmake_executable = cmake_path
        else:
            cmake_executable = os.path.join(cmake_path, "cmake")
            if sys.platform == "win32":
                cmake_executable += ".exe"

        if os.path.isfile(cmake_executable) and os.access(cmake_executable, os.X_OK):
            return cmake_executable
        else:
            print(
                f"Error: CMAKE_PATH is set to '{cmake_path}' but cmake executable not found or not executable.")
            print(
                "Please ensure CMAKE_PATH points to the cmake executable or directory containing cmake.")
            sys.exit(1)
    else:
        # Check if cmake is in system PATH
        cmake_executable = shutil.which("cmake")
        if cmake_executable:
            return cmake_executable
        else:
            print("Error: CMake not found in system PATH.")
            print(
                "Please install CMake and ensure it's in your PATH, or set the CMAKE_PATH environment variable.")
            print("CMAKE_PATH can point to:")
            print("  - The cmake executable directly: export CMAKE_PATH=/path/to/cmake")
            print(
                "  - The directory containing cmake: export CMAKE_PATH=/path/to/cmake/bin")
            sys.exit(1)


def check_environment(args):
    # Check CMake availability (skip for Android as it uses NDK's built-in CMake)
    cmake_executable = None
    if args["framework"] != "android":
        cmake_executable = check_cmake()
        print(f"Using CMake: {cmake_executable}")

    VULKAN_SDK = os.environ.get("VULKAN_SDK")
    if not VULKAN_SDK:
        print("Error: VULKAN_SDK environment variable is not set.")
        sys.exit(1)

    if not os.path.exists(VULKAN_SDK):
        print(f"Error: VULKAN_SDK path {VULKAN_SDK} does not exist.")
        sys.exit(1)

    # Exit if framework is macos or ios but not running on macOS
    if args["framework"] in ("macos", "ios") and sys.platform != "darwin":
        print(
            f"Error: Framework '{args['framework']}' requires macOS, but current system is not macOS.")
        sys.exit(1)

    return cmake_executable


def run_git_submodule_update():
    print("Updating git submodules...")
    subprocess.run(
        ["git", "submodule", "update", "--init", "--recursive"],
        check=True
    )


def setup(script_dir):
    from resources.setup import setup_resources

    run_git_submodule_update()
    setup_resources(script_dir)
    print("Setup complete.")


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
            builder.build_and_run(args)

    elif args["framework"] == "macos":
        import build_system.macos.build as builder

        if args["clangd"]:
            builder.configure_for_clangd(args)
        elif args["generate_only"]:
            builder.generate_project(args)
        else:
            builder.build_and_run(args)

    elif args["framework"] == "ios":
        import build_system.ios.build as builder

        if args["clangd"]:
            builder.configure_for_clangd(args)
        elif args["generate_only"]:
            builder.generate_project(args)
        else:
            builder.build_and_run(args)

    elif args["framework"] == "android":
        import build_system.android.build as builder

        if args["clangd"] or args["generate_only"]:
            # gradle sync will do both in the same time
            builder.sync_only(args)
        else:
            builder.build_and_run(args)


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)

    args = parse_args()

    try:
        # Always run setup first
        setup(script_dir)

        # If --setup-only flag is used, exit after setup
        if args["setup_only"]:
            print("Setup-only mode: Skipping build.")
            return

        # Check environment before building
        args["cmake_executable"] = check_environment(args)

        # Run build process
        build_project(args)

    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
