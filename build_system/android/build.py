import os
import re
import subprocess
import sys
import urllib.request
import platform
from pathlib import Path

from build_system.utils import run_command_with_logging, download_file, extract_zip, robust_rmtree
from build_system.builder_interface import FrameworkBuilder
from build_system.prerequisites import setup_android_validation

SCRIPT = os.path.abspath(__file__)
SCRIPTPATH = os.path.dirname(SCRIPT)
REPO_ROOT = os.path.dirname(os.path.dirname(SCRIPTPATH))


def download_gradle_wrapper():
    """Download gradle-wrapper.jar if it doesn't exist."""
    wrapper_jar_path = os.path.join(
        SCRIPTPATH, "gradle", "wrapper", "gradle-wrapper.jar")

    if os.path.exists(wrapper_jar_path):
        return

    print("Gradle wrapper JAR not found, downloading...")

    properties_path = os.path.join(
        SCRIPTPATH, "gradle", "wrapper", "gradle-wrapper.properties")
    if not os.path.exists(properties_path):
        raise RuntimeError(f"gradle-wrapper.properties not found at {properties_path}")

    gradle_version = None
    with open(properties_path, 'r') as f:
        for line in f:
            if line.startswith('distributionUrl='):
                match = re.search(r'gradle-([0-9.]+)-', line)
                if match:
                    gradle_version = match.group(1)
                    break

    if not gradle_version:
        raise RuntimeError("Could not determine Gradle version from gradle-wrapper.properties")

    wrapper_url = f"https://raw.githubusercontent.com/gradle/gradle/v{gradle_version}/gradle/wrapper/gradle-wrapper.jar"

    try:
        os.makedirs(os.path.dirname(wrapper_jar_path), exist_ok=True)
        print(f"Downloading gradle-wrapper.jar for Gradle {gradle_version}...")
        urllib.request.urlretrieve(wrapper_url, wrapper_jar_path)
        print("Gradle wrapper downloaded successfully.")
    except Exception as e:
        raise RuntimeError(f"Failed to download gradle-wrapper.jar: {e}. "
                           "Please download it manually or ensure internet connection.")


def prepare_environment(args=None):
    """Check if Android development environment is properly set up."""

    cmake_file = Path('CMakeLists.txt').resolve()
    print("Touching to force CMake re-run:", cmake_file)
    cmake_file.touch()

    java_home = os.environ.get("JAVA_HOME")

    if not java_home:
        if platform.system() == "Darwin":
            possible_paths = [
                "/Applications/Android Studio.app/Contents/jbr/Contents/Home",
                "/Applications/Android Studio.app/Contents/jre/Contents/Home",
            ]
        elif platform.system() == "Windows":
            possible_paths = [
                "C:/Program Files/Android/Android Studio/jbr",
                "C:/Program Files/Android/Android Studio/jre",
            ]
        elif platform.system() == "Linux":
            possible_paths = [
                "/usr/lib/jvm/temurin-17-jdk-amd64",
                "/usr/lib/jvm/java-17-openjdk-amd64",
                "/usr/lib/jvm/java-17",
                "/opt/android-studio/jbr",
                os.path.expanduser("~/android-studio/jbr"),
                "/snap/android-studio/current/android-studio/jbr",
            ]
        else:
            possible_paths = []

        for path in possible_paths:
            if os.path.exists(path):
                java_home = path
                break

    if java_home and os.path.exists(java_home):
        os.environ["JAVA_HOME"] = java_home
        java_bin = os.path.join(java_home, "bin")
        current_path = os.environ.get("PATH", "")
        if java_bin not in current_path:
            os.environ["PATH"] = f"{java_bin}:{current_path}"
        print(f"Using JAVA_HOME: {java_home}")
    else:
        print("Error: Java environment not found!")
        print("Please install Java or set JAVA_HOME environment variable.")
        print("For Android development, you can:")
        print("1. Install Android Studio (includes JBR)")
        print("2. Install JDK 17 or later")
        print("3. Set JAVA_HOME to point to your Java installation")
        if platform.system() == "Darwin":
            print(
                "   Example: export JAVA_HOME='/Applications/Android Studio.app/Contents/jbr/Contents/Home'")
        elif platform.system() == "Linux":
            print(
                "   Example: export JAVA_HOME='/usr/lib/jvm/java-17-openjdk-amd64'")
        raise RuntimeError("Java environment not found")

    download_gradle_wrapper()

    gradlew_path = os.path.join(
        SCRIPTPATH, "gradlew" if platform.system() != "Windows" else "gradlew.bat")
    if not os.path.exists(gradlew_path):
        raise RuntimeError(f"gradlew not found at {gradlew_path}")

    if platform.system() != "Windows":
        os.chmod(gradlew_path, 0o755)

    output_dir = os.path.join(SCRIPTPATH, "output")

    if args and args.get("clean", False):
        robust_rmtree(output_dir)

    os.makedirs(output_dir, exist_ok=True)
    os.chdir(SCRIPTPATH)

    return gradlew_path, output_dir


def get_apk_path(args):
    """Get the path to the APK based on build configuration."""
    config = args["config"]
    variant = "debug" if config == "Debug" else "release"
    apk_name = f"app-{variant}.apk"
    return os.path.join(SCRIPTPATH, "app", "build", "outputs", "apk", variant, apk_name)


def find_android_sdk():
    sdk = os.environ.get("ANDROID_HOME")
    if sdk and os.path.isdir(sdk):
        return sdk

    if platform.system() == "Darwin":
        default = os.path.expanduser("~/Library/Android/sdk")
    elif platform.system() == "Windows":
        default = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk")
    else:
        default = os.path.expanduser("~/Android/Sdk")
    if os.path.isdir(default):
        return default

    raise RuntimeError("Android SDK not found; set ANDROID_HOME")


def newest_build_tools(sdk):
    tools_root = os.path.join(sdk, "build-tools")
    versions = [entry for entry in os.listdir(tools_root)
                if os.path.isdir(os.path.join(tools_root, entry))] if os.path.isdir(tools_root) else []
    if not versions:
        raise RuntimeError(f"no Android build-tools under {tools_root}")

    def version_key(name):
        return [int(part) for part in re.findall(r"\d+", name)]

    return os.path.join(tools_root, max(versions, key=version_key))


def resign_apk(apk_path):
    """Injection appends entries to the apk, which breaks zip alignment and the
    signature; restore both with the debug key gradle signs with, so an installed
    build still upgrades in place."""
    keystore = os.path.expanduser("~/.android/debug.keystore")
    if not os.path.isfile(keystore):
        raise RuntimeError(f"debug keystore not found at {keystore};"
                           " build any android app once to create it")

    tools = newest_build_tools(find_android_sdk())
    windows = platform.system() == "Windows"
    zipalign = os.path.join(tools, "zipalign.exe" if windows else "zipalign")
    apksigner = os.path.join(tools, "apksigner.bat" if windows else "apksigner")

    aligned_path = apk_path + ".aligned"
    subprocess.run([zipalign, "-f", "4", apk_path, aligned_path], check=True)
    subprocess.run([apksigner, "sign", "--ks", keystore, "--ks-pass", "pass:android",
                    "--out", apk_path, aligned_path], check=True)
    os.remove(aligned_path)
    print(f"re-signed {apk_path} with the debug key")


def report_cook_activity(log_path):
    sys.path.insert(0, os.path.join(REPO_ROOT, "dev"))
    from cook_log import count_cook_activity

    with open(log_path, errors="replace") as log_file:
        hits, cooked = count_cook_activity(log_file)
    if cooked:
        print(f"WARNING: {cooked} on-the-fly cook(s): the installed package"
              " did not provide this content pre-cooked")
    else:
        print(f"{hits} cook artifact hit(s), no on-the-fly cooking")


def run_on_device(args):
    """Install APK and run the application."""
    package_name = "io.tqjxlm.sparkle"
    activity_name = f"{package_name}/{package_name}.VulkanActivity"

    apk_path = args.get("product_path")
    if not apk_path:
        apk_path = get_apk_path(args)
        print("Installing the raw build output; without the package stage"
              " it carries no cooked content and the device cooks on the fly.")

    print("Installing APK...")
    result = subprocess.run(["adb", "install", apk_path])
    if result.returncode != 0:
        print("APK installation failed!")
        return

    print("Starting application...")
    result = subprocess.run(["adb", "shell", "am", "start", "-n", activity_name])
    if result.returncode != 0:
        print("Failed to start application!")
        return

    print("Monitoring application logs (Ctrl+C to stop)...")
    try:
        subprocess.run(["adb", "logcat", "-s", "sparkle"])
    except KeyboardInterrupt:
        print("\nStopping log monitoring...")

    log_destination = os.path.join(os.getcwd(), "output.log")
    print(f"Pulling application output log to {log_destination}...")
    pulled = subprocess.run(["adb", "pull",
                             f"/sdcard/Android/data/{package_name}/files/logs/output.log", "."])
    if pulled.returncode == 0:
        report_cook_activity(log_destination)


def sync_only(args):
    """Trigger Gradle sync without building to generate CMake files."""
    gradlew_path, output_dir = prepare_environment(args)

    config = args["config"]
    cmake_args = args["cmake_options"]
    gradle_task = "generateJsonModelDebug" if config == "Debug" else "generateJsonModelRelease"

    sync_cmd = [
        gradlew_path,
        gradle_task,
        f"-PcmakeArgs={' '.join(cmake_args)}",
        "--info",
    ]

    log_file = os.path.join(output_dir, "sync.log")
    run_command_with_logging(sync_cmd, log_file, "Syncing Android project")
    print("Gradle sync successful! CMake files generated.")


class AndroidBuilder(FrameworkBuilder):
    """Android framework builder implementation."""

    def __init__(self):
        super().__init__("android")

    def configure_for_clangd(self, args):
        """Configure project for clangd (via Gradle sync)."""
        sync_only(args)

    def generate_project(self, args):
        """Generate IDE project files (via Gradle sync)."""
        sync_only(args)

    def configure_only(self, args):
        """Gradle sync; the CMake configure itself only runs inside the gradle build."""
        sync_only(args)

    def build(self, args):
        """Build the project."""
        setup_android_validation(SCRIPTPATH)

        gradlew_path, output_dir = prepare_environment(args)

        config = args["config"]
        cmake_args = args["cmake_options"]
        gradle_task = "assembleDebug" if config == "Debug" else "assembleRelease"

        build_cmd = [
            gradlew_path,
            gradle_task,
            f"-PcmakeArgs={' '.join(cmake_args)}",
            "--info"
        ]

        log_file = os.path.join(output_dir, "build.log")
        run_command_with_logging(build_cmd, log_file, "Building Android APK")

        apk_path = get_apk_path(args)
        print(f"Build successful! APK created at: {apk_path}")

    def archive(self, args):
        """Archive the built project."""
        return get_apk_path(args)

    def resign_package(self, package_path):
        resign_apk(package_path)

    def run(self, args):
        """Run the built project."""
        run_on_device(args)
