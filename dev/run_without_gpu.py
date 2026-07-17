"""Setup and run the app with Mesa Lavapipe software Vulkan rendering.

On Windows, downloads Mesa lavapipe if needed. On Linux, uses the lavapipe ICD from
the distribution's mesa-vulkan-drivers package. Either way the Vulkan loader is
configured to use lavapipe exclusively, then the app executable runs. Importable as
a module for the env setup.

Usage:
    python dev/run_without_gpu.py [app args...]
    python dev/run_without_gpu.py --test_case screenshot --pipeline forward
"""

import glob
import os
import platform
import shutil
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)

MESA_VERSION = "25.3.5"
MESA_CACHE_DIR = os.path.join(PROJECT_ROOT, "build_cache", "mesa")

# where distributions drop their Vulkan driver manifests
LINUX_ICD_DIRS = ("/usr/share/vulkan/icd.d", "/usr/local/share/vulkan/icd.d")


def _find_7z():
    if shutil.which("7z"):
        return "7z"
    for prog in ("ProgramFiles", "ProgramFiles(x86)"):
        candidate = os.path.join(os.environ.get(prog, ""), "7-Zip", "7z.exe")
        if os.path.isfile(candidate):
            return candidate
    return None


def _lavapipe_env(icd_json, extra_path=None):
    """Force the Vulkan loader to use exactly the lavapipe ICD."""
    env = os.environ.copy()
    env["VK_ICD_FILENAMES"] = icd_json
    env["VK_LOADER_DRIVERS_SELECT"] = os.path.basename(icd_json)
    env["VK_LOADER_DRIVERS_DISABLE"] = "*"
    if extra_path:
        env["PATH"] = extra_path + os.pathsep + env.get("PATH", "")
    return env


def _setup_lavapipe_windows(version):
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
    return _lavapipe_env(icd_json, extra_path=os.path.dirname(icd_json))


def _setup_lavapipe_linux():
    for icd_dir in LINUX_ICD_DIRS:
        candidates = sorted(glob.glob(os.path.join(icd_dir, "lvp_icd.*.json")))
        if candidates:
            icd_json = candidates[0]
            print(f"Mesa lavapipe ready: {icd_json}")
            return _lavapipe_env(icd_json)

    print("ERROR: lavapipe ICD not found. Install it with:"
          " sudo apt install mesa-vulkan-drivers")
    sys.exit(1)


def setup_lavapipe(version=MESA_VERSION):
    """Locate (or install) Mesa lavapipe and return a modified env dict that forces its use."""
    if platform.system() == "Windows":
        return _setup_lavapipe_windows(version)
    if platform.system() == "Linux":
        return _setup_lavapipe_linux()
    print("Lavapipe setup is only supported on Windows and Linux.")
    sys.exit(1)


def main():
    env = setup_lavapipe()

    exe_name = "sparkle.exe" if platform.system() == "Windows" else "sparkle"
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
