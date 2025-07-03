import shutil
import urllib.request
import json
import platform
import os
import sys
import subprocess

from build_system.utils import download_file, extract_archive

SCRIPT = os.path.abspath(__file__)
SCRIPTPATH = os.path.dirname(SCRIPT)
is_windows = platform.system() == "Windows"
is_macos = platform.system() == "Darwin"


def install_cmake():
    """Install CMake automatically based on platform."""
    cmake_version = "3.30.5"

    # Create build_cache directory if it doesn't exist
    build_cache_dir = os.path.join(SCRIPTPATH, "..", "build_cache")
    os.makedirs(build_cache_dir, exist_ok=True)

    # Check if already installed in build_cache
    cmake_dir = os.path.join(build_cache_dir, "cmake")
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
                # Check if cmake is now available
                cmake_executable_path = shutil.which("cmake")
                if cmake_executable_path:
                    return cmake_executable_path
            else:
                print(f"Homebrew installation failed: {result.stderr}")
        except (subprocess.TimeoutExpired, subprocess.SubprocessError, FileNotFoundError):
            print(
                "Homebrew not available or failed. Falling back to manual installation...")

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
        print(
            f"Error: Unsupported platform '{system}' for automatic CMake installation.")
        print("Please install CMake manually using a package manager or download from: https://cmake.org/download/")
        raise Exception()

    try:
        download_path = os.path.join(build_cache_dir, filename)

        print(f"Downloading CMake {cmake_version}...")
        print(f"URL: {download_url}")

        download_file(download_url, download_path)

        print("Extracting CMake...")
        tmp_dir = os.path.join(build_cache_dir, "tmp")
        extract_archive(download_path, tmp_dir)

        # Find the extracted cmake directory
        if system == "darwin":
            extracted_dir = os.path.join(
                tmp_dir, f"cmake-{cmake_version}-macos-universal")
        elif system == "windows":
            extracted_dir = os.path.join(
                tmp_dir, f"cmake-{cmake_version}-windows-x86_64")
        else:  # linux
            extracted_dir = os.path.join(
                tmp_dir, f"cmake-{cmake_version}-linux-x86_64")

        # Move to final location
        if os.path.exists(extracted_dir):
            shutil.move(extracted_dir, cmake_dir)
        else:
            print(
                f"Error: Expected directory not found after extraction: {extracted_dir}")
            raise Exception()

        # Clean up download file
        os.remove(download_path)

        # Verify installation
        if os.path.exists(cmake_executable):
            print(f"CMake {cmake_version} installed successfully!")
            return cmake_executable
        else:
            print(
                f"Error: CMake executable not found after installation: {cmake_executable}")
            raise Exception()

    except Exception as e:
        print(f"Error installing CMake: {e}")
        print("Please install CMake manually from: https://cmake.org/download/")
        raise Exception()


def find_cmake():
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
            raise Exception()
    else:
        # Check if cmake is in system PATH
        cmake_executable = shutil.which("cmake")
        if cmake_executable:
            return cmake_executable
        else:
            # CMake not found - try to install automatically
            print("CMake not found in system PATH. Attempting automatic installation...")
            return install_cmake()


def find_and_set_vulkan_sdk():
    VULKAN_SDK = os.environ.get("VULKAN_SDK")

    # If VULKAN_SDK is set and exists, we're good
    if VULKAN_SDK and os.path.exists(VULKAN_SDK):
        return

    # If VULKAN_SDK is set but doesn't exist, show error
    if VULKAN_SDK and not os.path.exists(VULKAN_SDK):
        print(f"Error: VULKAN_SDK path {VULKAN_SDK} does not exist.")

    # VULKAN_SDK not set - try to install automatically
    print("VULKAN_SDK environment variable is not set. Attempting automatic installation...")

    # Create build_cache directory if it doesn't exist
    build_cache_dir = os.path.join(SCRIPTPATH, "..", "build_cache")
    os.makedirs(build_cache_dir, exist_ok=True)

    # Install Vulkan SDK to build_cache
    vulkan_sdk_path = install_vulkan_sdk(build_cache_dir)

    # Set VULKAN_SDK environment variable for this session
    os.environ["VULKAN_SDK"] = vulkan_sdk_path
    print(f"VULKAN_SDK set to: {vulkan_sdk_path}")
    print("Note: This is temporary for this session. To make it permanent, add to your shell profile:")
    print(f"export VULKAN_SDK={vulkan_sdk_path}")


def get_latest_vulkan_sdk_version(system, platform_name):
    print(f"Fetching latest Vulkan SDK version for {system}...")
    version_url = f"https://vulkan.lunarg.com/sdk/versions/{platform_name}.json"
    with urllib.request.urlopen(version_url) as response:
        response_data = response.read().decode()
        versions = json.loads(response_data)

    if not versions:
        print("Error: No Vulkan SDK versions found.")
        raise Exception()

    # The JSON structure might be different than expected
    # Try different approaches to get the version
    if isinstance(versions, list) and len(versions) > 0:
        if isinstance(versions[0], dict) and "version" in versions[0]:
            latest_version = versions[0]["version"]
        else:
            latest_version = str(versions[0])
    elif isinstance(versions, dict):
        # If it's a dict, try to find version info
        if "version" in versions:
            latest_version = versions["version"]
        elif "latest" in versions:
            latest_version = versions["latest"]
        else:
            # Use the first key that looks like a version
            version_keys = [k for k in versions.keys() if any(c.isdigit()
                                                              for c in k)]
            if version_keys:
                latest_version = version_keys[0]
            else:
                print("Error: Could not determine Vulkan SDK version from response.")
                print(f"Response: {response_data}")
                raise Exception()
    else:
        print("Error: Unexpected JSON response format.")
        print(f"Response: {response_data}")
        raise Exception()

    return latest_version


def validate_vulkan_sdk(vulkan_sdk_path):
    # Verify it's a complete installation
    if is_macos:
        vulkan_include = os.path.join(
            vulkan_sdk_path, "macOS", "include", "vulkan", "vulkan.h")
    else:
        vulkan_include = os.path.join(
            vulkan_sdk_path, "include", "vulkan", "vulkan.h")
    return os.path.exists(vulkan_include)


def install_vulkan_sdk(build_cache_dir):
    """Install Vulkan SDK to build_cache directory and return the path."""

    # Detect platform
    system = platform.system().lower()
    if system == "darwin":
        platform_name = "mac"
    elif system == "windows":
        platform_name = "windows"
    elif system == "linux":
        platform_name = "linux"
    else:
        print(
            f"Error: Unsupported platform '{system}' for automatic Vulkan SDK installation.")
        raise Exception()

    tmp_dir = os.path.join(build_cache_dir, "tmp")

    try:
        latest_version = "1.4.313.0"
        print(f"Use Vulkan SDK version: {latest_version}")

        # Check if already installed in build_cache
        vulkan_sdk_path = os.path.join(
            build_cache_dir, "VulkanSDK", latest_version)

        if os.path.exists(vulkan_sdk_path):
            if validate_vulkan_sdk(vulkan_sdk_path):
                print(
                    f"Vulkan SDK {latest_version} already installed in build_cache.")
                return vulkan_sdk_path
            else:
                print(
                    f"Incomplete Vulkan SDK installation found. Removing {vulkan_sdk_path}")
                shutil.rmtree(vulkan_sdk_path)

        # Determine download info based on platform
        if platform_name == "mac":
            filename = f"vulkansdk-macos-{latest_version}.zip"
            download_url = f"https://sdk.lunarg.com/sdk/download/{latest_version}/mac/{filename}"
        elif platform_name == "windows":
            filename = f"vulkansdk-windows-X64-{latest_version}.exe"
            download_url = f"https://sdk.lunarg.com/sdk/download/{latest_version}/windows/{filename}"
        elif platform_name == "linux":
            filename = f"vulkansdk-linux-x86_64-{latest_version}.tar.xz"
            download_url = f"https://sdk.lunarg.com/sdk/download/{latest_version}/linux/{filename}"
        else:
            print(
                f"Error: Unsupported platform '{platform_name}' for download.")
            raise Exception()

        download_path = os.path.join(build_cache_dir, filename)

        print(f"Downloading Vulkan SDK {latest_version}...")
        print(f"URL: {download_url}")

        # download if not exists
        if os.path.exists(download_path):
            # TODO: check hash
            print(f"Vulkan SDK {latest_version} already downloaded.")
        else:
            download_file(download_url, download_path)

        # Extract/Install SDK
        if platform_name == "linux":
            print("Extracting Vulkan SDK...")
            extract_archive(download_path, tmp_dir)

            # Find the extracted directory structure: tmp/{SDKVERSION}/x86_64
            extracted_dir = os.path.join(tmp_dir, latest_version)
            if os.path.exists(extracted_dir):
                x86_64_dir = os.path.join(extracted_dir, "x86_64")
                if os.path.exists(x86_64_dir):
                    # Move contents from x86_64 directory to target SDK path
                    os.makedirs(vulkan_sdk_path, exist_ok=True)
                    for item in os.listdir(x86_64_dir):
                        src = os.path.join(x86_64_dir, item)
                        dst = os.path.join(vulkan_sdk_path, item)
                        shutil.copytree(src, dst)
                else:
                    raise Exception(
                        f"Expected x86_64 directory not found in {extracted_dir}")
            else:
                raise Exception(
                    f"Extraction failed. Expected path is {tmp_dir}")

            # Verify installation by checking for key files
            vulkan_include = os.path.join(
                vulkan_sdk_path, "include", "vulkan", "vulkan.h")
            if not os.path.exists(vulkan_include):
                raise Exception(
                    f"Vulkan SDK installation verification failed: {vulkan_include} not found")

        elif platform_name == "mac":
            print("Extracting Vulkan SDK...")
            extract_archive(download_path, tmp_dir)

            # Install SDK components
            print("Installing Vulkan SDK core and iOS components...")
            installer_path = os.path.join(
                tmp_dir, f"vulkansdk-macOS-{latest_version}.app", "Contents", "MacOS", f"vulkansdk-macOS-{latest_version}")

            # Make installer executable
            os.chmod(installer_path, 0o755)

            # Install core and iOS components to target directory
            install_cmd = [
                installer_path,
                "install",
                "com.lunarg.vulkan.core",
                "com.lunarg.vulkan.ios",
                "--root", vulkan_sdk_path,
                "--accept-licenses",
                "--default-answer",
                "--confirm-command"
            ]

            print(f"Running installer command: {' '.join(install_cmd)}")

            result = subprocess.run(
                install_cmd, capture_output=True, text=True)
            if result.returncode != 0:
                raise Exception(
                    f"Vulkan SDK installation failed: {result.stderr}")

        elif platform_name == "windows":
            # Install SDK components
            print("Installing Vulkan SDK core...")
            installer_path = download_path

            install_cmd = [
                installer_path,
                "install",
                "com.lunarg.vulkan.core",
                "--root", vulkan_sdk_path,
                "--accept-licenses",
                "--default-answer",
                "--confirm-command"
            ]

            print(f"Running installer command: {' '.join(install_cmd)}")

            result = subprocess.run(
                install_cmd, capture_output=True, text=True)
            if result.returncode != 0:
                raise Exception(
                    "Vulkan SDK installation failed. Please run as administrator.")

        # Clean up download file
        os.remove(download_path)

        print(f"Vulkan SDK {latest_version} installed successfully!")
        return vulkan_sdk_path

    except Exception as e:
        print(f"Error installing Vulkan SDK: {e}")

        print("Please install the Vulkan SDK manually from: https://vulkan.lunarg.com/sdk/home")

        raise Exception()


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

    homebrew_paths = [
        "/opt/homebrew/opt/llvm",  # Apple Silicon
        "/usr/local/opt/llvm",     # Intel Mac
    ]

    # Try Homebrew paths first (macOS) - prioritize over system clang
    if platform.system() == "Darwin":
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

    # Still no? try to install it
    if is_macos:
        print("LLVM not found. Attempting to install via Homebrew...")
        try:
            result = subprocess.run(
                ["brew", "install", "llvm"], capture_output=True, text=True, timeout=300)
            if result.returncode == 0:
                print("Successfully installed LLVM via Homebrew")
                # Re-check Homebrew paths after installation
                for llvm_path in homebrew_paths:
                    clang_path = os.path.join(llvm_path, "bin", "clang")
                    clangpp_path = os.path.join(llvm_path, "bin", "clang++")
                    if os.path.exists(clang_path) and os.path.exists(clangpp_path):
                        print(f"Found LLVM via Homebrew at: {llvm_path}")
                        return llvm_path
            else:
                raise Exception(result.stderr)
        except Exception as e:
            print("Failed to install LLVM via Homebrew. Please install manually.")
            print(f"Error: {e}")
    else:
        print("LLVM auto-installation not supported on this platform. Please make sure it is installed manually, preferably via a native package manager.")

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
        r"C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe",
        r"C:\\Program Files\\Microsoft Visual Studio\\Installer\\vswhere.exe"
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
                        return vs_path
            except (subprocess.TimeoutExpired, subprocess.SubprocessError):
                continue

    # Fallback to common installation paths (VS 2022 only)
    common_paths = [
        r"C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional",
        r"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community",
        r"C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise"
    ]

    for path in common_paths:
        if os.path.exists(path):
            return path

    return None


def install_vcpkg(build_cache_dir):
    """Install vcpkg to build_cache directory and return the path."""

    vcpkg_dir = os.path.join(build_cache_dir, "vcpkg")

    # Check if already installed
    vcpkg_executable = "vcpkg.exe" if is_windows else "vcpkg"
    vcpkg_exe_path = os.path.join(vcpkg_dir, vcpkg_executable)

    if os.path.exists(vcpkg_exe_path):
        print("vcpkg already installed in build_cache.")
        return vcpkg_dir

    try:
        print("Cloning vcpkg repository...")

        # Clone vcpkg repository
        clone_cmd = [
            "git", "clone",
            "https://github.com/Microsoft/vcpkg.git",
            vcpkg_dir
        ]

        result = subprocess.run(clone_cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise Exception(f"Failed to clone vcpkg: {result.stderr}")

        print("Building vcpkg...")

        # Build vcpkg
        if is_windows:
            bootstrap_script = os.path.join(vcpkg_dir, "bootstrap-vcpkg.bat")
            build_cmd = [bootstrap_script]
        else:
            bootstrap_script = os.path.join(vcpkg_dir, "bootstrap-vcpkg.sh")
            # Make bootstrap script executable
            os.chmod(bootstrap_script, 0o755)
            build_cmd = [bootstrap_script]

        result = subprocess.run(build_cmd, cwd=vcpkg_dir,
                                capture_output=True, text=True)
        if result.returncode != 0:
            raise Exception(f"Failed to build vcpkg: {result.stderr}")

        # Verify vcpkg was built successfully
        if not os.path.exists(vcpkg_exe_path):
            raise Exception(
                f"vcpkg executable not found after build: {vcpkg_exe_path}")

        print(f"vcpkg installed successfully to: {vcpkg_dir}")
        return vcpkg_dir

    except Exception as e:
        print(f"Error installing vcpkg: {e}")
        print("Please install vcpkg manually from: https://github.com/Microsoft/vcpkg")
        raise Exception()


def find_vcpkg():
    if not is_windows:
        return None

    vcpkg_executable = "vcpkg.exe" if is_windows else "vcpkg"

    # Check for user override first
    vcpkg_dir = os.environ.get("VCPKG_PATH")
    if vcpkg_dir:
        if os.path.exists(os.path.join(vcpkg_dir, vcpkg_executable)):
            return vcpkg_dir
        print(
            f"VCPKG_PATH is set to '{vcpkg_dir}' but path does not exist. try auto-detecting...")

    # Check if already installed in build_cache
    build_cache_dir = os.path.join(SCRIPTPATH, "..", "build_cache")
    vcpkg_dir = os.path.join(build_cache_dir, "vcpkg")
    if os.path.exists(os.path.join(vcpkg_dir, vcpkg_executable)):
        print("Use locally cached vcpkg installation.")
        return vcpkg_dir

    # Check for vcpkg in common system locations
    system_vcpkg = shutil.which(vcpkg_executable)
    if system_vcpkg:
        return os.path.dirname(system_vcpkg)

    # Still no? Try to install locally
    print("vcpkg not found. Installing locally to build_cache/vcpkg...")
    os.makedirs(build_cache_dir, exist_ok=True)
    vcpkg_install_path = install_vcpkg(build_cache_dir)

    return vcpkg_install_path


def find_or_install_ninja():
    """Find ninja executable in PATH or install it to build_cache and return the path."""
    
    # Check if ninja is already in PATH
    ninja_executable = shutil.which("ninja")
    if ninja_executable:
        return ninja_executable
    
    # Ninja not found in PATH, try to install to build_cache
    ninja_version = "1.12.1"
    
    # Create build_cache directory if it doesn't exist
    build_cache_dir = os.path.join(SCRIPTPATH, "..", "build_cache")
    os.makedirs(build_cache_dir, exist_ok=True)
    
    # Check if already installed in build_cache
    ninja_dir = os.path.join(build_cache_dir, "ninja")
    ninja_exe_path = os.path.join(ninja_dir, "ninja")
    if is_windows:
        ninja_exe_path += ".exe"
    
    if os.path.exists(ninja_exe_path):
        print("Ninja already installed in build_cache.")
        return ninja_exe_path
    
    # Determine download URL based on platform
    system = platform.system().lower()
    
    if system == "windows":
        filename = f"ninja-win.zip"
        download_url = f"https://github.com/ninja-build/ninja/releases/download/v{ninja_version}/{filename}"
    elif system == "darwin":
        filename = f"ninja-mac.zip"
        download_url = f"https://github.com/ninja-build/ninja/releases/download/v{ninja_version}/{filename}"
    elif system == "linux":
        filename = f"ninja-linux.zip"
        download_url = f"https://github.com/ninja-build/ninja/releases/download/v{ninja_version}/{filename}"
    else:
        print(f"Error: Unsupported platform '{system}' for automatic Ninja installation.")
        print("Please install Ninja manually from: https://github.com/ninja-build/ninja/releases")
        raise Exception()
    
    try:
        download_path = os.path.join(build_cache_dir, filename)
        
        print(f"Downloading Ninja {ninja_version}...")
        print(f"URL: {download_url}")
        
        download_file(download_url, download_path)
        
        print("Extracting Ninja...")
        extract_archive(download_path, ninja_dir)
        
        # Make ninja executable on Unix systems
        if not is_windows:
            os.chmod(ninja_exe_path, 0o755)
        
        # Clean up download file
        os.remove(download_path)
        
        # Verify installation
        if os.path.exists(ninja_exe_path):
            print(f"Ninja {ninja_version} installed successfully!")
            return ninja_exe_path
        else:
            print(f"Error: Ninja executable not found after installation: {ninja_exe_path}")
            raise Exception()
            
    except Exception as e:
        print(f"Error installing Ninja: {e}")
        print("Please install Ninja manually from: https://github.com/ninja-build/ninja/releases")
        raise Exception()


def install_glfw():
    if is_windows:
        print("Installing GLFW via vcpkg...")
        vcpkg_path = find_vcpkg()
        if vcpkg_path:
            vcpkg_executable = "vcpkg.exe" if is_windows else "vcpkg"
            vcpkg_exe_path = os.path.join(vcpkg_path, vcpkg_executable)
            install_cmd = [vcpkg_exe_path, "install", "glfw3:x64-windows"]
            result = subprocess.run(
                install_cmd, capture_output=True, text=True)
            if result.returncode != 0:
                print(f"Failed to install GLFW via vcpkg: {result.stderr}")
                raise Exception()
            print("GLFW installed successfully via vcpkg!")
        else:
            print("Failed to install GLFW via vcpkg. vcpkg not found.")
            raise Exception()
    elif is_macos:
        print("Installing GLFW via Homebrew...")
        try:
            result = subprocess.run(
                ["brew", "install", "glfw"], capture_output=True, text=True, timeout=300)
            if result.returncode == 0:
                print("Successfully installed GLFW via Homebrew")
            else:
                raise Exception(result.stderr)
        except Exception as e:
            print(
                "Failed to install GLFW via Homebrew. Please make sure it is installed manually.")
            print(f"Error: {e}")
    else:
        print("GLFW auto-installation not supported on this platform. Please make sure it is installed manually, preferably via a native package manager.")
