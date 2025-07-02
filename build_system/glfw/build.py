import os
import sys
import subprocess
import platform

from build_system.prerequisites import find_llvm_path, find_visual_studio_path, find_vcpkg, install_glfw
from build_system.utils import robust_rmtree

# Determine script directory
SCRIPT = os.path.abspath(__file__)
SCRIPTPATH = os.path.dirname(SCRIPT)

framework_args = ["-DUSE_GLFW=ON"]
is_windows = platform.system() == "Windows"


def get_toolchain_args():
    if not is_windows:
        return []

    vcpkg_path = find_vcpkg()
    if vcpkg_path:
        vcpkg_toolchain_file = os.path.join(
            vcpkg_path, "scripts", "buildsystems", "vcpkg.cmake")
        if os.path.exists(vcpkg_toolchain_file):
            return [f"-DCMAKE_TOOLCHAIN_FILE={vcpkg_toolchain_file}"]
        else:
            print(f"vcpkg.cmake not found at {vcpkg_toolchain_file}.")

    print("vcpkg tool chain is not used. you should make sure CMake can find dependency libraries.")

    return []


def clean_output_directory(output_dir):
    """Clean the output directory if it exists."""
    if os.path.exists(output_dir):
        print(f"Cleaning output directory: {output_dir}")
        robust_rmtree(output_dir)


def get_cmd_with_vcvars(vs_path, cmake_cmd):
    vcvars_path = os.path.join(
        vs_path, "VC", "Auxiliary", "Build", "vcvars64.bat")

    # guard each command with quotes in case there are spaces
    guarded_cmake_cmd = " ".join([f'"{cmd}"' for cmd in cmake_cmd])

    # visual studio requires vcvars to be called with actual command
    return f'CALL "{vcvars_path}" && ' + guarded_cmake_cmd


def configure(args, is_generate_sln):
    """Configure CMake project with appropriate settings for the platform."""

    install_glfw()

    if is_generate_sln:
        output_dir = os.path.join(SCRIPTPATH, "project")
    else:
        output_dir = os.path.join(SCRIPTPATH, "output")

    if args.get("clean", False):
        clean_output_directory(output_dir)

    os.makedirs(output_dir, exist_ok=True)
    os.chdir(output_dir)

    if is_generate_sln:
        # If using sln, compilers are determined by Visual Studio
        compiler_args = []
        # Use the native default generator for project generation
        generator_args = []
    elif is_windows:
        # Windows: use MSVC environment and bundled clang-cl
        compiler_args = ["-DCMAKE_CXX_COMPILER=clang-cl",
                         "-DCMAKE_C_COMPILER=clang-cl"]
        generator_args = ["-G Ninja",
                          f"-DCMAKE_BUILD_TYPE={args['config']}"]
    else:
        # Non-Windows: find LLVM installation
        LLVM = find_llvm_path()
        if not LLVM:
            print("Error: Could not find LLVM installation.")
            if platform.system() == "Darwin":
                print(
                    "Please install LLVM via Homebrew (Apple's system clang is not supported):")
                print("  brew install llvm")
            else:
                print(
                    "Please install LLVM via your package manager or set the LLVM environment variable.")
                print("  Ubuntu/Debian: sudo apt install clang")
                print("  Fedora: sudo dnf install clang")
            raise Exception()
        compiler_args = [
            f"-DCMAKE_C_COMPILER={LLVM}/bin/clang", f"-DCMAKE_CXX_COMPILER={LLVM}/bin/clang++"]
        generator_args = [f"-DCMAKE_BUILD_TYPE={args['config']}"]

    # Build CMake command
    cmake_cmd = [
        args["cmake_executable"],
        "../../..",
    ] + generator_args + args["cmake_options"] + compiler_args + framework_args + get_toolchain_args()

    if is_windows and not is_generate_sln:
        # for clang-cl builds, activate MSVC environment first
        vs_path = find_visual_studio_path()
        if not vs_path:
            print("Error: Could not find Visual Studio 2022 installation.")
            print("Please ensure Visual Studio 2022 is installed with C++ tools.")
            raise Exception()

        clang_cl_path = os.path.join(
            vs_path, "VC", "Tools", "Llvm", "x64", "bin", "clang-cl.exe")
        if not os.path.exists(clang_cl_path):
            print(f"Error: clang-cl.exe not found in Visual Studio installation.")
            print(
                "Please install 'C++ Clang Compiler for Windows' component in Visual Studio Installer.")
            raise Exception()

        shell_cmd = get_cmd_with_vcvars(vs_path, cmake_cmd)

        print("Running CMake configure:", shell_cmd)
        result = subprocess.run(shell_cmd, shell=True)
    else:
        print("Running CMake configure:", " ".join(cmake_cmd))
        result = subprocess.run(cmake_cmd)

    if result.returncode != 0:
        print("CMake configure failed.")
        raise Exception()

    return output_dir


def build(args):
    """Build the project using CMake."""

    if is_windows:
        build_threads = "64"
    else:
        build_threads = "16"

    cmake_cmd = [args["cmake_executable"], "--build", ".", "-j",
                 build_threads, "--config", args["config"]]

    if is_windows:
        # On Windows, activate MSVC environment first
        vs_path = find_visual_studio_path()
        if not vs_path:
            print("Error: Could not find Visual Studio 2022 installation.")
            print("Please ensure Visual Studio 2022 is installed with C++ tools.")
            raise Exception()

        shell_cmd = get_cmd_with_vcvars(vs_path, cmake_cmd)

        print("Running build:", shell_cmd)
        result = subprocess.run(shell_cmd, shell=True)
    else:
        print("Running build:", " ".join(cmake_cmd))
        result = subprocess.run(cmake_cmd)

    if result.returncode != 0:
        print("Build failed.")
        raise Exception()


def generate_project(args):
    """Generate IDE project files using CMake."""
    if not is_windows:
        print(
            "Project generation for glfw is only supported on Windows with Visual Studio.")
        raise Exception()

    output_dir = configure(args, True)

    print(
        f"Visutal Studio sln is generated at {output_dir}. Open with command:")
    print(f"start {output_dir}/sparkle.sln")


def run(args):
    exe_name = "sparkle.exe" if is_windows else "sparkle"

    exe_path = os.path.join(".", exe_name)
    run_cmd = [exe_path] + args["unknown_args"]
    print(f"Running executable: {run_cmd}")
    subprocess.run(run_cmd)


def build_and_run(args):
    """Build and optionally run the project."""
    configure(args, False)

    build(args)

    if args["run"]:
        run(args)
