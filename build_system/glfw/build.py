import os
import subprocess
import platform

from build_system.prerequisites import find_llvm_path, find_or_install_ninja, find_visual_studio_path, find_vcpkg, install_glfw
from build_system.utils import compress_zip, robust_rmtree
from build_system.builder_interface import FrameworkBuilder

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


def get_cmd_with_vcvars(vs_path, cmake_cmd):
    vcvars_path = os.path.join(vs_path, "VC", "Auxiliary", "Build", "vcvars64.bat")
    guarded_cmake_cmd = " ".join([f'"{cmd}"' for cmd in cmake_cmd])
    return f'CALL "{vcvars_path}" && ' + guarded_cmake_cmd


def get_output_dir(is_generate_sln):
    if is_generate_sln:
        return os.path.join(SCRIPTPATH, "project")
    else:
        return os.path.join(SCRIPTPATH, "output")


def _require_vs_path():
    vs_path = find_visual_studio_path()
    if not vs_path:
        raise RuntimeError(
            "Could not find Visual Studio 2022 installation. "
            "Please ensure Visual Studio 2022 is installed with C++ tools.")
    return vs_path


def configure(args, is_generate_sln):
    """Configure CMake project with appropriate settings for the platform."""

    install_glfw()

    output_dir = get_output_dir(is_generate_sln)

    if args.get("clean", False):
        robust_rmtree(output_dir)

    os.makedirs(output_dir, exist_ok=True)
    os.chdir(output_dir)

    if is_generate_sln:
        # If using sln, compilers are determined by Visual Studio
        compiler_args = []
        generator_args = []
    elif is_windows:
        ninja_path = find_or_install_ninja()
        vs_path = _require_vs_path()

        clang_cl_path = os.path.join(vs_path, "VC", "Tools", "Llvm", "x64", "bin", "clang-cl.exe")
        if not os.path.exists(clang_cl_path):
            raise RuntimeError(
                "clang-cl.exe not found in Visual Studio installation. "
                "Please install 'C++ Clang Compiler for Windows' component in Visual Studio Installer.")

        compiler_args = ["-DCMAKE_CXX_COMPILER=clang-cl", "-DCMAKE_C_COMPILER=clang-cl"]
        generator_args = [
            "-G Ninja", f"-DCMAKE_BUILD_TYPE={args['config']}", f"-DCMAKE_MAKE_PROGRAM={ninja_path}"]
    else:
        ninja_path = find_or_install_ninja()

        if platform.system() == "Darwin":
            if not os.path.exists("/usr/bin/clang") or not os.path.exists("/usr/bin/clang++"):
                raise RuntimeError(
                    "Xcode clang compilers not found at /usr/bin/. "
                    "Please install Xcode Command Line Tools: xcode-select --install")
            print("Using Xcode system clang/clang++ compilers")
            compiler_args = [
                "-DCMAKE_C_COMPILER=/usr/bin/clang", "-DCMAKE_CXX_COMPILER=/usr/bin/clang++"]
        else:
            LLVM = find_llvm_path()
            if not LLVM:
                raise RuntimeError(
                    "Could not find LLVM installation. "
                    "Please install clang via your package manager (apt install clang / dnf install clang) "
                    "or set the LLVM environment variable.")
            compiler_args = [
                f"-DCMAKE_C_COMPILER={LLVM}/bin/clang", f"-DCMAKE_CXX_COMPILER={LLVM}/bin/clang++"]

        generator_args = [
            "-G Ninja", f"-DCMAKE_BUILD_TYPE={args['config']}", f"-DCMAKE_MAKE_PROGRAM={ninja_path}"]

    cmake_cmd = [
        args["cmake_executable"],
        "../../..",
    ] + generator_args + args["cmake_options"] + compiler_args + framework_args + get_toolchain_args()

    if is_windows and not is_generate_sln:
        shell_cmd = get_cmd_with_vcvars(vs_path, cmake_cmd)
        print("Running CMake configure:", shell_cmd)
        result = subprocess.run(shell_cmd, shell=True)
    else:
        print("Running CMake configure:", " ".join(cmake_cmd))
        result = subprocess.run(cmake_cmd, env=os.environ.copy())

    if result.returncode != 0:
        raise RuntimeError("CMake configure failed.")


class GlfwBuilder(FrameworkBuilder):
    """GLFW framework builder implementation."""

    def __init__(self):
        super().__init__("glfw")

    def configure_for_clangd(self, args):
        """Configure CMake for clangd support."""
        configure(args, False)

    def generate_project(self, args):
        """Generate IDE project files."""
        if not is_windows:
            raise RuntimeError(
                "Project generation for glfw is only supported on Windows with Visual Studio.")

        configure(args, True)

        output_dir = get_output_dir(True)
        print(f"Visual Studio sln is generated at {output_dir}. Open with command:")
        print(f"start {output_dir}/sparkle.sln")

    def build(self, args):
        """Build the project."""
        configure(args, False)

        build_threads = "64" if is_windows else "16"
        cmake_cmd = [args["cmake_executable"], "--build", ".", "-j",
                     build_threads, "--config", args["config"]]

        if is_windows:
            vs_path = _require_vs_path()
            shell_cmd = get_cmd_with_vcvars(vs_path, cmake_cmd)
            print("Running build:", shell_cmd)
            result = subprocess.run(shell_cmd, shell=True)
        else:
            print("Running build:", " ".join(cmake_cmd))
            result = subprocess.run(cmake_cmd, env=os.environ.copy())

        if result.returncode != 0:
            raise RuntimeError("Build failed.")

    def archive(self, args):
        """Archive the built project."""
        output_dir = get_output_dir(False)
        archive_path = os.path.join(output_dir, "product.zip")
        compress_zip(os.path.join(output_dir, "build"), archive_path)
        return archive_path

    def run(self, args):
        """Run the built project."""
        exe_name = "sparkle.exe" if is_windows else "sparkle"
        output_dir = get_output_dir(False)
        run_cmd = [os.path.join(output_dir, "build", exe_name)] + args["unknown_args"]
        print(f"Running executable: {run_cmd}")
        subprocess.run(args=run_cmd, cwd=os.path.join(output_dir, "build"), env=os.environ.copy())
