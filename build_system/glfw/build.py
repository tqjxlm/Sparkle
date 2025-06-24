import os
import sys
import subprocess
import platform
import shutil

# Determine script directory
SCRIPT = os.path.abspath(__file__)
SCRIPTPATH = os.path.dirname(SCRIPT)

framework_args = "-DUSE_GLFW=ON"
is_windows = platform.system() == "Windows"


def find_llvm_path():
    """Find LLVM installation path automatically."""
    if is_windows:
        return None  # Windows uses clang-cl from Visual Studio

    # Check for user override first
    llvm_path_override = os.environ.get("LLVM")
    if llvm_path_override:
        if os.path.exists(llvm_path_override):
            clang_path = os.path.join(llvm_path_override, "bin", "clang")
            clangpp_path = os.path.join(llvm_path_override, "bin", "clang++")
            if os.path.exists(clang_path) and os.path.exists(clangpp_path):
                return llvm_path_override
        print(
            f"Warning: LLVM environment variable set to '{llvm_path_override}' but path is invalid.")
        print("Falling back to automatic detection...")

    # Try Homebrew paths first (macOS) - prioritize over system clang
    if platform.system() == "Darwin":
        homebrew_paths = [
            "/opt/homebrew/opt/llvm",  # Apple Silicon
            "/usr/local/opt/llvm",     # Intel Mac
        ]
        for llvm_path in homebrew_paths:
            clang_path = os.path.join(llvm_path, "bin", "clang")
            clangpp_path = os.path.join(llvm_path, "bin", "clang++")
            if os.path.exists(clang_path) and os.path.exists(clangpp_path):
                print(f"Found LLVM via Homebrew at: {llvm_path}")
                return llvm_path

    # Try to find clang in PATH (but skip Apple's system clang on macOS)
    try:
        clang_result = subprocess.run(
            ["which", "clang"], capture_output=True, text=True, timeout=5)
        if clang_result.returncode == 0:
            clang_path = clang_result.stdout.strip()
            # Skip Apple's system clang on macOS
            if platform.system() == "Darwin" and "/usr/bin/clang" in clang_path:
                pass  # Skip Apple's system clang
            else:
                # Extract LLVM root from clang path (e.g., /usr/local/bin/clang -> /usr/local)
                bin_dir = os.path.dirname(clang_path)
                llvm_root = os.path.dirname(bin_dir)
                clangpp_path = os.path.join(bin_dir, "clang++")
                if os.path.exists(clangpp_path):
                    print(f"Found LLVM via PATH at: {llvm_root}")
                    return llvm_root
    except (subprocess.TimeoutExpired, subprocess.SubprocessError, FileNotFoundError):
        pass

    # Try common Linux distribution paths
    common_paths = [
        "/usr/lib/llvm-18",
        "/usr/local/llvm",
        "/opt/llvm",
    ]

    for llvm_path in common_paths:
        clang_path = os.path.join(llvm_path, "bin", "clang")
        clangpp_path = os.path.join(llvm_path, "bin", "clang++")
        if os.path.exists(clang_path) and os.path.exists(clangpp_path):
            print(f"Found LLVM at: {llvm_path}")
            return llvm_path

    return None


def find_visual_studio_path():
    """Find Visual Studio installation path automatically."""
    if not is_windows:
        return None

    # Check for user override first
    vs_path_override = os.environ.get("VS_PATH")
    if vs_path_override:
        if os.path.exists(vs_path_override):
            return vs_path_override
        else:
            print(
                f"Warning: VS_PATH environment variable set to '{vs_path_override}' but path does not exist.")
            print("Falling back to automatic detection...")

    # Try vswhere.exe first (most reliable method)
    vswhere_paths = [
        r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe",
        r"C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe"
    ]

    for vswhere_path in vswhere_paths:
        if os.path.exists(vswhere_path):
            try:
                # Get both installation path and version to verify VS 2022
                result = subprocess.run([
                    vswhere_path, "-latest", "-products", "*",
                    "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                    "-version", "[17.0,18.0)",  # VS 2022 version range
                    "-property", "installationPath", "-format", "value"
                ], capture_output=True, text=True, timeout=10)

                if result.returncode == 0 and result.stdout.strip():
                    vs_path = result.stdout.strip()

                    # Double-check version by querying it separately
                    version_result = subprocess.run([
                        vswhere_path, "-path", vs_path,
                        "-property", "catalog_productDisplayVersion", "-format", "value"
                    ], capture_output=True, text=True, timeout=10)

                    if version_result.returncode == 0 and version_result.stdout.strip():
                        version = version_result.stdout.strip()
                        if not version.startswith("17."):
                            print(
                                f"Found Visual Studio {version} but VS 2022 (17.x) is required.")
                            continue

                    vcvars_path = os.path.join(
                        vs_path, "VC", "Auxiliary", "Build", "vcvars64.bat")
                    if os.path.exists(vcvars_path):
                        print(f"Found Visual Studio 2022 at: {vs_path}")
                        return vcvars_path
            except (subprocess.TimeoutExpired, subprocess.SubprocessError):
                continue

    # Fallback to common installation paths (VS 2022 only)
    common_paths = [
        r"C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
        r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    ]

    for path in common_paths:
        if os.path.exists(path):
            return path

    return None


def get_toolchain_args(args):
    toolchain_args = ""
    if os.path.exists(args["vcpkg_path"]):
        vcpkg_toolchain_file = os.path.join(
            args["vcpkg_path"], "scripts", "buildsystems", "vcpkg.cmake")
        if not os.path.exists(vcpkg_toolchain_file):
            print(f"vcpkg.cmake not found at {vcpkg_toolchain_file}.")
            sys.exit(1)
        toolchain_args = f" -DCMAKE_TOOLCHAIN_FILE={vcpkg_toolchain_file}"
    return toolchain_args


def clean_output_directory(output_dir):
    """Clean the output directory if it exists."""
    if os.path.exists(output_dir):
        print(f"Cleaning output directory: {output_dir}")
        shutil.rmtree(output_dir)


def configure(args, is_project):
    """Configure CMake project with appropriate settings for the platform."""

    if is_project:
        output_dir = os.path.join(SCRIPTPATH, "project")
    else:
        output_dir = os.path.join(SCRIPTPATH, "output")

    if args.get("clean", False):
        clean_output_directory(output_dir)

    os.makedirs(output_dir, exist_ok=True)
    os.chdir(output_dir)

    if is_windows:
        # Windows: use MSVC environment and bundled clang-cl
        compiler_args = "-D CMAKE_CXX_COMPILER=clang-cl -D CMAKE_C_COMPILER=clang-cl"
        generator_args = f"-G Ninja -DCMAKE_BUILD_TYPE={args['config']}"
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
            sys.exit(1)
        compiler_args = f"-D CMAKE_C_COMPILER={LLVM}/bin/clang -D CMAKE_CXX_COMPILER={LLVM}/bin/clang++"
        generator_args = f"-DCMAKE_BUILD_TYPE={args['config']}"

    # Use the native default generator for project generation
    if is_project:
        generator_args = ""

    # Build CMake command
    cmake_cmd = [
        args["cmake_executable"],
        "../../..",
        generator_args,
        args["cmake_options"],
        compiler_args,
        framework_args,
        get_toolchain_args(args),
    ]

    # Flatten command arguments
    cmake_cmd_flat = []
    for part in cmake_cmd:
        if part:
            cmake_cmd_flat.extend(part.split())

    print("Running CMake configure:")
    print(" ".join(cmake_cmd_flat))

    if is_windows:
        # On Windows, activate MSVC environment first
        vcvars_path = find_visual_studio_path()
        if not vcvars_path:
            print("Error: Could not find Visual Studio 2022 installation.")
            print("Please ensure Visual Studio 2022 is installed with C++ tools.")
            sys.exit(1)

        # Check clang-cl only for builds (not project generation)
        if not is_project:
            vs_path = vcvars_path.replace(
                r"\VC\Auxiliary\Build\vcvars64.bat", "")
            clang_cl_path = os.path.join(
                vs_path, "VC", "Tools", "Llvm", "x64", "bin", "clang-cl.exe")
            if not os.path.exists(clang_cl_path):
                print(f"Error: clang-cl.exe not found in Visual Studio installation.")
                print(
                    "Please install 'C++ Clang Compiler for Windows' component in Visual Studio Installer.")
                sys.exit(1)

        print(f"Using Visual Studio at: {vcvars_path}")
        shell_cmd = f'CALL "{vcvars_path}" && ' + " ".join(cmake_cmd_flat)
        result = subprocess.run(shell_cmd, shell=True)
    else:
        result = subprocess.run(cmake_cmd_flat)

    if result.returncode != 0:
        print("CMake configure failed.")
        sys.exit(1)

    return output_dir


def build(args):
    """Build the project using CMake."""

    if is_windows:
        build_threads = "64"
    else:
        build_threads = "16"

    build_cmd = [args["cmake_executable"], "--build", ".", "-j",
                 build_threads, "--config", args["config"]]

    print("Running build:")
    print(" ".join(build_cmd))

    if is_windows:
        # On Windows, activate MSVC environment first
        vcvars_path = find_visual_studio_path()
        if not vcvars_path:
            print("Error: Could not find Visual Studio 2022 installation.")
            print("Please ensure Visual Studio 2022 is installed with C++ tools.")
            sys.exit(1)

        shell_cmd = f'CALL "{vcvars_path}" && ' + " ".join(build_cmd)
        result = subprocess.run(shell_cmd, shell=True)
    else:
        result = subprocess.run(build_cmd)

    if result.returncode != 0:
        print("Build failed.")
        sys.exit(1)


def generate_project(args):
    """Generate IDE project files using CMake."""
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
