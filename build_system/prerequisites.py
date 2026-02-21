import shutil
import json
import platform
import os
import sys
import subprocess

from build_system.utils import download_file, extract_archive, extract_zip

SCRIPT = os.path.abspath(__file__)
SCRIPTPATH = os.path.dirname(SCRIPT)
_BUILD_CACHE_DIR = os.path.join(SCRIPTPATH, "..", "build_cache")

is_windows = platform.system() == "Windows"
is_macos = platform.system() == "Darwin"


def load_prerequisites_versions():
    """Load version information from prerequisites.json."""
    prerequisites_path = os.path.join(SCRIPTPATH, "..", "prerequisites.json")
    try:
        with open(prerequisites_path, 'r') as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError) as e:
        print(f"Warning: Could not load prerequisites.json: {e}")
        return {}


def install_cmake():
    """Install CMake automatically based on platform."""
    prerequisites = load_prerequisites_versions()
    cmake_version = prerequisites.get("cmake", "3.30.5")

    os.makedirs(_BUILD_CACHE_DIR, exist_ok=True)

    cmake_dir = os.path.join(_BUILD_CACHE_DIR, "cmake")
    cmake_executable = os.path.join(cmake_dir, "bin", "cmake")
    if is_windows:
        cmake_executable += ".exe"

    if os.path.exists(cmake_executable):
        print("CMake already installed in build_cache.")
        return cmake_executable

    system = platform.system().lower()

    if system == "darwin":
        # Try installing via Homebrew first
        try:
            print("Installing CMake via Homebrew...")
            result = subprocess.run(
                ["brew", "install", "cmake"], capture_output=True, text=True, timeout=300)
            if result.returncode == 0:
                print("Successfully installed CMake via Homebrew")
                cmake_executable_path = shutil.which("cmake")
                if cmake_executable_path:
                    return cmake_executable_path
            else:
                print(f"Homebrew installation failed: {result.stderr}")
        except (subprocess.TimeoutExpired, subprocess.SubprocessError, FileNotFoundError):
            print("Homebrew not available or failed. Falling back to manual installation...")

        # Fallback to manual installation
        print("Downloading CMake for macOS...")
        filename = f"cmake-{cmake_version}-macos-universal.tar.gz"
        download_url = f"https://github.com/Kitware/CMake/releases/download/v{cmake_version}/{filename}"

    elif system == "windows":
        print("Downloading CMake for Windows...")
        filename = f"cmake-{cmake_version}-windows-x86_64.zip"
        download_url = f"https://github.com/Kitware/CMake/releases/download/v{cmake_version}/{filename}"

    elif system == "linux":
        print("Downloading CMake for Linux...")
        filename = f"cmake-{cmake_version}-linux-x86_64.tar.gz"
        download_url = f"https://github.com/Kitware/CMake/releases/download/v{cmake_version}/{filename}"

    else:
        print("Please install CMake manually: https://cmake.org/download/")
        raise RuntimeError(f"Unsupported platform '{system}' for automatic CMake installation.")

    download_path = os.path.join(_BUILD_CACHE_DIR, filename)

    print(f"Downloading CMake {cmake_version} from: {download_url}")
    download_file(download_url, download_path)

    print("Extracting CMake...")
    tmp_dir = os.path.join(_BUILD_CACHE_DIR, "tmp")
    extract_archive(download_path, tmp_dir)

    platform_suffix = {"darwin": "macos-universal", "windows": "windows-x86_64", "linux": "linux-x86_64"}
    extracted_dir = os.path.join(tmp_dir, f"cmake-{cmake_version}-{platform_suffix[system]}")

    if not os.path.exists(extracted_dir):
        raise RuntimeError(f"Expected directory not found after extraction: {extracted_dir}")

    shutil.move(extracted_dir, cmake_dir)
    os.remove(download_path)

    if not os.path.exists(cmake_executable):
        raise RuntimeError(f"CMake executable not found after installation: {cmake_executable}")

    print(f"CMake {cmake_version} installed successfully!")
    return cmake_executable


def find_cmake():
    """Check if CMake is available in PATH or CMAKE_PATH environment variable."""
    cmake_path = os.environ.get("CMAKE_PATH")

    if cmake_path:
        if os.path.isfile(cmake_path):
            cmake_executable = cmake_path
        else:
            cmake_executable = os.path.join(cmake_path, "cmake")
            if sys.platform == "win32":
                cmake_executable += ".exe"

        if os.path.isfile(cmake_executable) and os.access(cmake_executable, os.X_OK):
            return cmake_executable
        raise RuntimeError(
            f"CMAKE_PATH is set to '{cmake_path}' but cmake executable not found or not executable.")
    else:
        cmake_executable = shutil.which("cmake")
        if cmake_executable:
            return cmake_executable
        print("CMake not found in system PATH. Attempting automatic installation...")
        return install_cmake()


def validate_vulkan_sdk(vulkan_sdk_path):
    vulkan_include = os.path.join(
        vulkan_sdk_path, "include", "vulkan", "vulkan.h")
    return os.path.exists(vulkan_include)


def set_vulkan_layer_path(vulkan_sdk_path):
    """Set VK_LAYER_PATH environment variable for validation layers."""
    layer_path = os.path.join(vulkan_sdk_path, "share", "vulkan", "explicit_layer.d")

    if os.path.exists(layer_path):
        existing = os.environ.get("VK_LAYER_PATH")
        if existing:
            if layer_path not in existing.split(os.pathsep):
                os.environ["VK_LAYER_PATH"] = f"{layer_path}{os.pathsep}{existing}"
        else:
            os.environ["VK_LAYER_PATH"] = layer_path
        print(f"VK_LAYER_PATH set to include: {layer_path}")
    else:
        print(f"Warning: Validation layer directory not found at {layer_path}")


def set_vulkan_icd_filenames(vulkan_sdk_path):
    """Set VK_ICD_FILENAMES environment variable for driver discovery."""
    # For macOS, we specifically look for MoltenVK
    if not is_macos:
        return

    icd_path = os.path.join(vulkan_sdk_path, "share", "vulkan", "icd.d", "MoltenVK_icd.json")

    if os.path.exists(icd_path):
        existing = os.environ.get("VK_ICD_FILENAMES")
        if existing:
            if icd_path not in existing.split(os.pathsep):
                os.environ["VK_ICD_FILENAMES"] = f"{icd_path}{os.pathsep}{existing}"
        else:
            os.environ["VK_ICD_FILENAMES"] = icd_path
        print(f"VK_ICD_FILENAMES set to include: {icd_path}")
    else:
        print(f"Warning: MoltenVK ICD file not found at {icd_path}")


def find_and_set_vulkan_sdk():
    VULKAN_SDK = os.environ.get("VULKAN_SDK")

    if VULKAN_SDK and os.path.exists(VULKAN_SDK):
        if validate_vulkan_sdk(VULKAN_SDK):
            set_vulkan_layer_path(VULKAN_SDK)
            set_vulkan_icd_filenames(VULKAN_SDK)
            return
        else:
            VULKAN_SDK = os.path.join(VULKAN_SDK, "macOS")
            if validate_vulkan_sdk(VULKAN_SDK):
                os.environ["VULKAN_SDK"] = VULKAN_SDK
                print(f"VULKAN_SDK set to: {VULKAN_SDK}")
                set_vulkan_layer_path(VULKAN_SDK)
                set_vulkan_icd_filenames(VULKAN_SDK)
                return

    if VULKAN_SDK and not os.path.exists(VULKAN_SDK):
        print(f"Error: VULKAN_SDK path {VULKAN_SDK} does not exist.")

    print("VULKAN_SDK environment variable is not set. Attempting automatic installation...")

    os.makedirs(_BUILD_CACHE_DIR, exist_ok=True)
    vulkan_sdk_path = install_vulkan_sdk(_BUILD_CACHE_DIR)

    os.environ["VULKAN_SDK"] = vulkan_sdk_path
    print(f"VULKAN_SDK set to: {vulkan_sdk_path}")
    print("Note: This is temporary for this session. To make it permanent, add to your shell profile:")
    print(f"export VULKAN_SDK={vulkan_sdk_path}")

    set_vulkan_layer_path(vulkan_sdk_path)
    set_vulkan_icd_filenames(vulkan_sdk_path)


def install_vulkan_sdk(build_cache_dir):
    """Install Vulkan SDK to build_cache directory and return the path."""

    system = platform.system().lower()
    platform_map = {"darwin": "mac", "windows": "windows", "linux": "linux"}
    if system not in platform_map:
        raise RuntimeError(f"Unsupported platform '{system}' for automatic Vulkan SDK installation.")
    platform_name = platform_map[system]

    tmp_dir = os.path.join(build_cache_dir, "tmp")

    prerequisites = load_prerequisites_versions()
    latest_version = prerequisites.get("VulkanSDK", "1.4.313.0")
    print(f"Use Vulkan SDK version: {latest_version}")

    vulkan_sdk_path = os.path.join(build_cache_dir, "VulkanSDK", latest_version)

    if os.path.exists(vulkan_sdk_path):
        if validate_vulkan_sdk(vulkan_sdk_path):
            print(f"Vulkan SDK {latest_version} already installed in build_cache.")
            return vulkan_sdk_path
        elif is_macos and validate_vulkan_sdk(os.path.join(vulkan_sdk_path, "macOS")):
            print(f"Vulkan SDK {latest_version} already installed in build_cache.")
            return os.path.join(vulkan_sdk_path, "macOS")
        else:
            print(f"Incomplete Vulkan SDK installation found. Removing {vulkan_sdk_path}")
            shutil.rmtree(vulkan_sdk_path)

    if platform_name == "mac":
        filename = f"vulkansdk-macos-{latest_version}.zip"
        download_url = f"https://sdk.lunarg.com/sdk/download/{latest_version}/mac/{filename}"
    elif platform_name == "windows":
        filename = f"vulkansdk-windows-X64-{latest_version}.exe"
        download_url = f"https://sdk.lunarg.com/sdk/download/{latest_version}/windows/{filename}"
    else:  # linux
        filename = f"vulkansdk-linux-x86_64-{latest_version}.tar.xz"
        download_url = f"https://sdk.lunarg.com/sdk/download/{latest_version}/linux/{filename}"

    download_path = os.path.join(build_cache_dir, filename)

    print(f"Downloading Vulkan SDK {latest_version} from: {download_url}")

    if os.path.exists(download_path):
        # TODO: check hash
        print(f"Vulkan SDK {latest_version} already downloaded.")
    else:
        download_file(download_url, download_path)

    if platform_name == "linux":
        print("Extracting Vulkan SDK...")
        extract_archive(download_path, tmp_dir)

        extracted_dir = os.path.join(tmp_dir, latest_version)
        x86_64_dir = os.path.join(extracted_dir, "x86_64")
        if not os.path.exists(x86_64_dir):
            raise RuntimeError(f"Expected x86_64 directory not found in {extracted_dir}")

        os.makedirs(vulkan_sdk_path, exist_ok=True)
        for item in os.listdir(x86_64_dir):
            shutil.copytree(os.path.join(x86_64_dir, item), os.path.join(vulkan_sdk_path, item))

        vulkan_include = os.path.join(vulkan_sdk_path, "include", "vulkan", "vulkan.h")
        if not os.path.exists(vulkan_include):
            raise RuntimeError(f"Vulkan SDK verification failed: {vulkan_include} not found")

    elif platform_name == "mac":
        print("Extracting Vulkan SDK...")
        extract_archive(download_path, tmp_dir)

        print("Installing Vulkan SDK core and iOS components...")
        installer_path = os.path.join(
            tmp_dir, f"vulkansdk-macOS-{latest_version}.app", "Contents", "MacOS",
            f"vulkansdk-macOS-{latest_version}")

        os.chmod(installer_path, 0o755)

        install_cmd = [
            installer_path, "install", "com.lunarg.vulkan.core",
            "--root", vulkan_sdk_path,
            "--accept-licenses", "--default-answer", "--confirm-command"
        ]

        print(f"Running installer command: {' '.join(install_cmd)}")
        result = subprocess.run(install_cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(f"Vulkan SDK installation failed: {result.stderr}")

    elif platform_name == "windows":
        print("Installing Vulkan SDK core...")
        install_cmd = [
            download_path, "install", "com.lunarg.vulkan.core",
            "--root", vulkan_sdk_path,
            "--accept-licenses", "--default-answer", "--confirm-command"
        ]

        print(f"Running installer command: {' '.join(install_cmd)}")
        result = subprocess.run(install_cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError("Vulkan SDK installation failed. Please run as administrator.")

    os.remove(download_path)

    if is_macos:
        vulkan_sdk_path = os.path.join(vulkan_sdk_path, "macOS")

    print(f"Vulkan SDK {latest_version} installed successfully!")
    return vulkan_sdk_path


def install_slangc(build_cache_dir):
    """Install slangc (Slang shader compiler) to build_cache directory."""
    prerequisites = load_prerequisites_versions()
    slang_version = prerequisites.get("slang", "2026.1.1")

    slang_dir = os.path.join(build_cache_dir, "slang")
    slangc_executable = os.path.join(slang_dir, "bin", "slangc")
    if is_windows:
        slangc_executable += ".exe"

    if os.path.exists(slangc_executable):
        print("slangc already installed in build_cache.")
        return slangc_executable

    system = platform.system().lower()
    machine = platform.machine().lower()

    if machine in ("amd64", "x86_64"):
        arch = "x86_64"
    elif machine in ("arm64", "aarch64"):
        arch = "aarch64"
    else:
        raise RuntimeError(f"Unsupported architecture '{machine}' for slangc installation.")

    platform_name_map = {"windows": "windows", "darwin": "macos", "linux": "linux"}
    if system not in platform_name_map:
        raise RuntimeError(f"Unsupported platform '{system}' for slangc installation.")
    platform_name = platform_name_map[system]

    filename = f"slang-{slang_version}-{platform_name}-{arch}.zip"
    download_url = f"https://github.com/shader-slang/slang/releases/download/v{slang_version}/{filename}"

    download_path = os.path.join(build_cache_dir, filename)

    print(f"Downloading slangc {slang_version} from: {download_url}")
    download_file(download_url, download_path)

    print("Extracting slangc...")
    os.makedirs(slang_dir, exist_ok=True)
    extract_zip(download_path, slang_dir)

    if not is_windows:
        os.chmod(slangc_executable, 0o755)

    os.remove(download_path)

    if not os.path.exists(slangc_executable):
        raise RuntimeError(f"slangc executable not found after installation: {slangc_executable}")

    print(f"slangc {slang_version} installed successfully!")
    return slangc_executable


def find_slangc():
    """Find slangc or install it automatically. Returns the path to the slangc executable."""
    slang_dir = os.path.join(_BUILD_CACHE_DIR, "slang")
    slangc_executable = os.path.join(slang_dir, "bin", "slangc")
    if is_windows:
        slangc_executable += ".exe"

    if os.path.exists(slangc_executable):
        return slangc_executable

    print("slangc not found. Installing to build_cache...")
    os.makedirs(_BUILD_CACHE_DIR, exist_ok=True)
    return install_slangc(_BUILD_CACHE_DIR)


def find_llvm_path():
    """Find LLVM installation path automatically."""
    if is_windows:
        return None  # Windows uses clang-cl from Visual Studio

    llvm_path_override = os.environ.get("LLVM")
    if llvm_path_override:
        if os.path.exists(llvm_path_override):
            if (os.path.exists(os.path.join(llvm_path_override, "bin", "clang")) and
                    os.path.exists(os.path.join(llvm_path_override, "bin", "clang++"))):
                return llvm_path_override
        print(
            f"Warning: LLVM environment variable set to '{llvm_path_override}' but path is invalid.")
        print("Falling back to automatic detection...")

    if platform.system() == "Darwin":
        # 1. Try Xcode system clang first
        if os.path.exists("/usr/bin/clang") and os.path.exists("/usr/bin/clang++"):
            print("Found Xcode clang at: /usr")
            return "/usr"

        # 2. Try Homebrew LLVM paths as fallback
        homebrew_paths = [
            "/opt/homebrew/opt/llvm",  # Apple Silicon
            "/usr/local/opt/llvm",     # Intel Mac
        ]
        for llvm_path in homebrew_paths:
            if (os.path.exists(os.path.join(llvm_path, "bin", "clang")) and
                    os.path.exists(os.path.join(llvm_path, "bin", "clang++"))):
                print(f"Found LLVM via Homebrew at: {llvm_path}")
                return llvm_path

        # 3. Try to install Homebrew LLVM if nothing found
        print("LLVM not found. Attempting to install via Homebrew...")
        try:
            result = subprocess.run(
                ["brew", "install", "llvm"], capture_output=True, text=True, timeout=300)
            if result.returncode == 0:
                print("Successfully installed LLVM via Homebrew")
                for llvm_path in homebrew_paths:
                    if (os.path.exists(os.path.join(llvm_path, "bin", "clang")) and
                            os.path.exists(os.path.join(llvm_path, "bin", "clang++"))):
                        print(f"Found LLVM via Homebrew at: {llvm_path}")
                        return llvm_path
            else:
                raise RuntimeError(result.stderr)
        except Exception as e:
            print(f"Failed to install LLVM via Homebrew. Please install manually. Error: {e}")

        return None

    # Try to find clang in PATH (non-macOS)
    try:
        clang_result = subprocess.run(
            ["which", "clang"], capture_output=True, text=True, timeout=5)
        if clang_result.returncode == 0:
            clang_path = clang_result.stdout.strip()
            bin_dir = os.path.dirname(clang_path)
            llvm_root = os.path.dirname(bin_dir)
            if os.path.exists(os.path.join(bin_dir, "clang++")):
                print(f"Found LLVM via PATH at: {llvm_root}")
                return llvm_root
    except (subprocess.TimeoutExpired, subprocess.SubprocessError, FileNotFoundError):
        pass

    for llvm_path in ("/usr/local/llvm", "/opt/llvm"):
        if (os.path.exists(os.path.join(llvm_path, "bin", "clang")) and
                os.path.exists(os.path.join(llvm_path, "bin", "clang++"))):
            print(f"Found LLVM at: {llvm_path}")
            return llvm_path

    print("LLVM auto-installation not supported on this platform. Please make sure it is installed manually, preferably via a native package manager.")
    return None


def find_visual_studio_path():
    """Find Visual Studio installation path automatically."""
    if not is_windows:
        return None

    vs_path_override = os.environ.get("VS_PATH")
    if vs_path_override:
        if os.path.exists(vs_path_override):
            return vs_path_override
        print(
            f"Warning: VS_PATH environment variable set to '{vs_path_override}' but path does not exist.")
        print("Falling back to automatic detection...")

    vswhere_paths = [
        r"C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe",
        r"C:\\Program Files\\Microsoft Visual Studio\\Installer\\vswhere.exe"
    ]

    for vswhere_path in vswhere_paths:
        if os.path.exists(vswhere_path):
            try:
                result = subprocess.run([
                    vswhere_path, "-latest", "-products", "*",
                    "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                    "-version", "[17.0,18.0)",  # VS 2022 version range
                    "-property", "installationPath", "-format", "value"
                ], capture_output=True, text=True, timeout=10)

                if result.returncode == 0 and result.stdout.strip():
                    vs_path = result.stdout.strip()

                    version_result = subprocess.run([
                        vswhere_path, "-path", vs_path,
                        "-property", "catalog_productDisplayVersion", "-format", "value"
                    ], capture_output=True, text=True, timeout=10)

                    if version_result.returncode == 0 and version_result.stdout.strip():
                        version = version_result.stdout.strip()
                        if not version.startswith("17."):
                            print(f"Found Visual Studio {version} but VS 2022 (17.x) is required.")
                            continue

                    vcvars_path = os.path.join(vs_path, "VC", "Auxiliary", "Build", "vcvars64.bat")
                    if os.path.exists(vcvars_path):
                        print(f"Found Visual Studio 2022 at: {vs_path}")
                        return vs_path
            except (subprocess.TimeoutExpired, subprocess.SubprocessError):
                continue

    for path in (
        r"C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional",
        r"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community",
        r"C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise",
    ):
        if os.path.exists(path):
            return path

    return None


def install_vcpkg(build_cache_dir):
    """Install vcpkg to build_cache directory and return the path."""

    vcpkg_dir = os.path.join(build_cache_dir, "vcpkg")
    vcpkg_exe_path = os.path.join(vcpkg_dir, "vcpkg.exe" if is_windows else "vcpkg")

    if os.path.exists(vcpkg_exe_path):
        print("vcpkg already installed in build_cache.")
        return vcpkg_dir

    print("Cloning vcpkg repository...")
    result = subprocess.run(
        ["git", "clone", "https://github.com/Microsoft/vcpkg.git", vcpkg_dir],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"Failed to clone vcpkg: {result.stderr}")

    print("Building vcpkg...")
    if is_windows:
        bootstrap_script = os.path.join(vcpkg_dir, "bootstrap-vcpkg.bat")
    else:
        bootstrap_script = os.path.join(vcpkg_dir, "bootstrap-vcpkg.sh")
        os.chmod(bootstrap_script, 0o755)

    result = subprocess.run([bootstrap_script], cwd=vcpkg_dir, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"Failed to build vcpkg: {result.stderr}")

    if not os.path.exists(vcpkg_exe_path):
        raise RuntimeError(f"vcpkg executable not found after build: {vcpkg_exe_path}")

    print(f"vcpkg installed successfully to: {vcpkg_dir}")
    return vcpkg_dir


def find_vcpkg():
    if not is_windows:
        return None

    vcpkg_executable = "vcpkg.exe"

    vcpkg_dir = os.environ.get("VCPKG_PATH")
    if vcpkg_dir:
        if os.path.exists(os.path.join(vcpkg_dir, vcpkg_executable)):
            return vcpkg_dir
        print(f"VCPKG_PATH is set to '{vcpkg_dir}' but path does not exist. try auto-detecting...")

    vcpkg_dir = os.path.join(_BUILD_CACHE_DIR, "vcpkg")
    if os.path.exists(os.path.join(vcpkg_dir, vcpkg_executable)):
        print("Use locally cached vcpkg installation.")
        return vcpkg_dir

    system_vcpkg = shutil.which(vcpkg_executable)
    if system_vcpkg:
        return os.path.dirname(system_vcpkg)

    print("vcpkg not found. Installing locally to build_cache/vcpkg...")
    os.makedirs(_BUILD_CACHE_DIR, exist_ok=True)
    return install_vcpkg(_BUILD_CACHE_DIR)


def find_or_install_ninja():
    """Find ninja executable in PATH or install it to build_cache and return the path."""

    ninja_executable = shutil.which("ninja")
    if ninja_executable:
        return ninja_executable

    prerequisites = load_prerequisites_versions()
    ninja_version = prerequisites.get("ninja", "1.12.1")

    os.makedirs(_BUILD_CACHE_DIR, exist_ok=True)

    ninja_dir = os.path.join(_BUILD_CACHE_DIR, "ninja")
    ninja_exe_path = os.path.join(ninja_dir, "ninja")
    if is_windows:
        ninja_exe_path += ".exe"

    if os.path.exists(ninja_exe_path):
        print("Ninja already installed in build_cache.")
        return ninja_exe_path

    system = platform.system().lower()
    platform_suffix = {"windows": "win", "darwin": "mac", "linux": "linux"}
    if system not in platform_suffix:
        print("Please install Ninja manually: https://github.com/ninja-build/ninja/releases")
        raise RuntimeError(f"Unsupported platform '{system}' for automatic Ninja installation.")

    filename = f"ninja-{platform_suffix[system]}.zip"
    download_url = f"https://github.com/ninja-build/ninja/releases/download/v{ninja_version}/{filename}"
    download_path = os.path.join(_BUILD_CACHE_DIR, filename)

    print(f"Downloading Ninja {ninja_version} from: {download_url}")
    download_file(download_url, download_path)

    print("Extracting Ninja...")
    extract_archive(download_path, ninja_dir)

    if not is_windows:
        os.chmod(ninja_exe_path, 0o755)

    os.remove(download_path)

    if not os.path.exists(ninja_exe_path):
        raise RuntimeError(f"Ninja executable not found after installation: {ninja_exe_path}")

    print(f"Ninja {ninja_version} installed successfully!")
    return ninja_exe_path


def install_glfw():
    if is_windows:
        print("Installing GLFW via vcpkg...")
        vcpkg_path = find_vcpkg()
        if vcpkg_path:
            vcpkg_exe_path = os.path.join(vcpkg_path, "vcpkg.exe")
            result = subprocess.run(
                [vcpkg_exe_path, "install", "glfw3:x64-windows"], capture_output=True, text=True)
            if result.returncode != 0:
                raise RuntimeError(f"Failed to install GLFW via vcpkg: {result.stderr}")
            print("GLFW installed successfully via vcpkg!")
        else:
            raise RuntimeError("Failed to install GLFW via vcpkg. vcpkg not found.")
    elif is_macos:
        print("Installing GLFW via Homebrew...")
        try:
            result = subprocess.run(
                ["brew", "install", "glfw"], capture_output=True, text=True, timeout=300)
            if result.returncode == 0:
                print("Successfully installed GLFW via Homebrew")
            else:
                raise RuntimeError(result.stderr)
        except Exception as e:
            print(
                "Failed to install GLFW via Homebrew. Please make sure it is installed manually.")
            print(f"Error: {e}")
    else:
        print("GLFW auto-installation not supported on this platform. Please make sure it is installed manually, preferably via a native package manager.")


def setup_android_validation(script_dir):
    """Setup Android Vulkan validation layer binaries with version from prerequisites.json."""
    prerequisites = load_prerequisites_versions()
    vulkan_version = prerequisites.get("VulkanSDK", "1.4.313.0")

    android_dir = script_dir
    app_jni_dir = os.path.join(android_dir, "app", "src", "main", "jniLibs")
    zip_path = os.path.join(android_dir, "android-validation-binaries.zip")
    url = f"https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/download/vulkan-sdk-{vulkan_version}/android-binaries-{vulkan_version}.zip"

    if os.path.exists(zip_path):
        print("Android validation binaries are up-to-date, skipping setup.")
        return

    print(f"Setting up Android validation binaries for Vulkan SDK {vulkan_version}...")

    download_file(url, zip_path)

    if os.path.exists(app_jni_dir):
        print(f"Removing existing directory: {app_jni_dir}")
        shutil.rmtree(app_jni_dir)
    os.makedirs(app_jni_dir, exist_ok=True)

    try:
        extract_zip(zip_path, app_jni_dir)
    except Exception as e:
        print(f"Failed to extract zip file: {e}")
        if os.path.exists(zip_path):
            os.remove(zip_path)
        raise

    # Flatten directory structure (move all subdirs/files up one level)
    subdirs = [d for d in os.listdir(app_jni_dir)
               if os.path.isdir(os.path.join(app_jni_dir, d))]
    if subdirs:
        parent_dir = os.path.join(app_jni_dir, subdirs[0])
        for item in os.listdir(parent_dir):
            src = os.path.join(parent_dir, item)
            dst = os.path.join(app_jni_dir, item)
            if os.path.exists(dst):
                if os.path.isdir(dst):
                    shutil.rmtree(dst)
                else:
                    os.remove(dst)
            shutil.move(src, dst)
        shutil.rmtree(parent_dir)

    print("Android validation binaries setup completed.")
