"""Setup and run the app with Mesa Lavapipe software Vulkan rendering on Windows.

Downloads Mesa lavapipe if needed, configures the Vulkan loader to use it,
then runs the app executable. Importable as a module for the env setup.

Usage:
    python dev/run_without_gpu.py [app args...]
    python dev/run_without_gpu.py --auto_screenshot true --pipeline forward
"""

import os
import platform
import shutil
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)

MESA_VERSION = "25.3.5"
MESA_CACHE_DIR = os.path.join(PROJECT_ROOT, "build_cache", "mesa")


def _find_7z():
    if shutil.which("7z"):
        return "7z"
    for prog in ("ProgramFiles", "ProgramFiles(x86)"):
        candidate = os.path.join(os.environ.get(prog, ""), "7-Zip", "7z.exe")
        if os.path.isfile(candidate):
            return candidate
    return None


def setup_lavapipe(version=MESA_VERSION):
    """Download Mesa lavapipe and return a modified env dict that forces its use."""
    if platform.system() != "Windows":
        print("Lavapipe setup is only supported on Windows.")
        sys.exit(1)

    mesa_dir = os.path.join(MESA_CACHE_DIR, version)
    icd_json = os.path.join(mesa_dir, "x64", "lvp_icd.x86_64.json")

    if not os.path.isfile(icd_json):
        sz = _find_7z()
        if not sz:
            print("ERROR: 7z not found. Install 7-Zip or ensure it's on PATH.")
            sys.exit(1)

        archive_name = f"mesa3d-{version}-release-msvc.7z"
        url = f"https://github.com/pal1000/mesa-dist-win/releases/download/{version}/{archive_name}"

        os.makedirs(mesa_dir, exist_ok=True)
        archive_path = os.path.join(mesa_dir, archive_name)

        sys.path.insert(0, PROJECT_ROOT)
        from build_system.utils import download_file
        download_file(url, archive_path)

        print(f"Extracting to {mesa_dir} ...")
        subprocess.check_call([sz, "x", archive_path, f"-o{mesa_dir}", "-y"],
                              stdout=subprocess.DEVNULL)
        os.remove(archive_path)

        if not os.path.isfile(icd_json):
            print(f"ERROR: Expected ICD not found at {icd_json}")
            for root, _, files in os.walk(mesa_dir):
                for f in files:
                    print(f"  {os.path.join(root, f)}")
            sys.exit(1)

    print(f"Mesa lavapipe ready: {icd_json}")

    mesa_bin_dir = os.path.dirname(icd_json)
    env = os.environ.copy()
    env["VK_ICD_FILENAMES"] = icd_json
    env["VK_LOADER_DRIVERS_SELECT"] = "lvp_icd.x86_64.json"
    env["VK_LOADER_DRIVERS_DISABLE"] = "*"
    env["PATH"] = mesa_bin_dir + os.pathsep + env.get("PATH", "")
    return env


def main():
    if platform.system() != "Windows":
        print("This script is for Windows only.")
        sys.exit(1)

    env = setup_lavapipe()

    exe_name = "sparkle.exe"
    build_dir = os.path.join(
        PROJECT_ROOT, "build_system", "glfw", "output", "build")
    exe_path = os.path.join(build_dir, exe_name)
    if not os.path.exists(exe_path):
        print(f"Executable not found: {exe_path}")
        print("Build the project first with: python3 build.py --framework glfw")
        sys.exit(1)

    run_cmd = [exe_path] + sys.argv[1:]
    print(f"Running: {' '.join(run_cmd)}")
    sys.exit(subprocess.run(run_cmd, cwd=build_dir, env=env).returncode)


if __name__ == "__main__":
    main()
