import os
import subprocess
import platform
from pathlib import Path
import os
import shutil

from build_system.utils import run_command_with_logging, download_file, extract_zip, robust_rmtree

SCRIPT = os.path.abspath(__file__)
SCRIPTPATH = os.path.dirname(SCRIPT)


def setup_android_validation(script_dir):
    android_dir = script_dir
    app_jni_dir = os.path.join(android_dir, "app", "src", "main", "jniLibs")
    zip_path = os.path.join(android_dir, "android-validation-binaries.zip")
    url = "https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/download/vulkan-sdk-1.4.313.0/android-binaries-1.4.313.0.zip"

    if os.path.exists(zip_path):
        print("Android validation binaries are up-to-date, skipping setup.")
        return

    print("Setting up Android validation binaries...")

    # Download validation binaries
    download_file(url, zip_path)

    # Remove existing jniLibs directory
    if os.path.exists(app_jni_dir):
        print(f"Removing existing directory: {app_jni_dir}")
        shutil.rmtree(app_jni_dir)
    os.makedirs(app_jni_dir, exist_ok=True)

    # Extract zip to jniLibs
    try:
        extract_zip(zip_path, app_jni_dir)
    except Exception as e:
        print(f"Failed to extract zip file: {e}")
        # Clean up downloaded file
        if os.path.exists(zip_path):
            os.remove(zip_path)
        raise Exception()

    # Flatten directory structure (move all subdirs/files up one level)
    # Find the first subdirectory in jniLibs
    subdirs = [d for d in os.listdir(app_jni_dir) if os.path.isdir(
        os.path.join(app_jni_dir, d))]
    if subdirs:
        parent_dir = os.path.join(app_jni_dir, subdirs[0])
        # Move all contents up one level
        for item in os.listdir(parent_dir):
            src = os.path.join(parent_dir, item)
            dst = os.path.join(app_jni_dir, item)
            if os.path.exists(dst):
                if os.path.isdir(dst):
                    shutil.rmtree(dst)
                else:
                    os.remove(dst)
            shutil.move(src, dst)
        # Remove now-empty parent directory
        shutil.rmtree(parent_dir)

    print("Android validation binaries setup completed.")


def download_gradle_wrapper():
    """Download gradle-wrapper.jar if it doesn't exist."""
    wrapper_jar_path = os.path.join(
        SCRIPTPATH, "gradle", "wrapper", "gradle-wrapper.jar")

    if os.path.exists(wrapper_jar_path):
        return  # Already exists

    print("Gradle wrapper JAR not found, downloading...")

    # Read gradle version from properties file
    properties_path = os.path.join(
        SCRIPTPATH, "gradle", "wrapper", "gradle-wrapper.properties")
    if not os.path.exists(properties_path):
        print(
            f"Error: gradle-wrapper.properties not found at {properties_path}")
        raise Exception()

    # Extract gradle version from properties
    gradle_version = None
    with open(properties_path, 'r') as f:
        for line in f:
            if line.startswith('distributionUrl='):
                # Extract version from URL like gradle-8.11.1-bin.zip
                import re
                match = re.search(r'gradle-([0-9.]+)-', line)
                if match:
                    gradle_version = match.group(1)
                    break

    if not gradle_version:
        print("Error: Could not determine Gradle version from gradle-wrapper.properties")
        raise Exception()

    # Download gradle-wrapper.jar
    wrapper_url = f"https://raw.githubusercontent.com/gradle/gradle/v{gradle_version}/gradle/wrapper/gradle-wrapper.jar"

    try:
        import urllib.request
        os.makedirs(os.path.dirname(wrapper_jar_path), exist_ok=True)
        print(f"Downloading gradle-wrapper.jar for Gradle {gradle_version}...")
        urllib.request.urlretrieve(wrapper_url, wrapper_jar_path)
        print("Gradle wrapper downloaded successfully.")
    except Exception as e:
        print(f"Failed to download gradle-wrapper.jar: {e}")
        print("Please download it manually or ensure internet connection.")
        raise Exception()


def prepare_environment(args=None):
    """Check if Android development environment is properly set up."""

    cmake_file = Path('CMakeLists.txt').resolve()
    print("Touching to force CMake re-run:", cmake_file)
    cmake_file.touch()

    # Check for JAVA_HOME (can be set in environment or use default Android Studio path)
    java_home = os.environ.get("JAVA_HOME")

    if not java_home:
        # Try default Android Studio paths
        if platform.system() == "Darwin":
            possible_paths = [
                "/Applications/Android Studio.app/Contents/jbr/Contents/Home",
                "/Applications/Android Studio.app/Contents/jre/Contents/Home",
            ]
            for path in possible_paths:
                if os.path.exists(path):
                    java_home = path
                    break
        elif platform.system() == "Windows":
            # Common Windows Android Studio paths
            possible_paths = [
                "C:/Program Files/Android/Android Studio/jbr",
                "C:/Program Files/Android/Android Studio/jre",
            ]
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
        raise Exception()

    # Ensure gradle-wrapper.jar exists
    download_gradle_wrapper()

    # Check for gradlew
    gradlew_path = os.path.join(
        SCRIPTPATH, "gradlew" if platform.system() != "Windows" else "gradlew.bat")
    if not os.path.exists(gradlew_path):
        print(f"Error: gradlew not found at {gradlew_path}")
        raise Exception()

    # Make sure gradlew is executable on Unix systems
    if platform.system() != "Windows":
        os.chmod(gradlew_path, 0o755)

    # Create output directory
    output_dir = os.path.join(SCRIPTPATH, "output")

    if args and args.get("clean", False):
        robust_rmtree(output_dir)

    os.makedirs(output_dir, exist_ok=True)

    # Change to Android project directory for Gradle sync
    os.chdir(SCRIPTPATH)

    return gradlew_path, output_dir


def build_apk(args):
    """Build Android APK using Gradle."""
    gradlew_path, output_dir = prepare_environment(args)

    # Prepare Gradle command
    config = args["config"]
    cmake_args = args["cmake_options"]

    if config == "Debug":
        gradle_task = "assembleDebug"
        apk_path = "./app/build/outputs/apk/debug/app-debug.apk"
    else:
        gradle_task = "assembleRelease"
        apk_path = "./app/build/outputs/apk/release/app-release.apk"

    # Build command
    build_cmd = [
        gradlew_path,
        gradle_task,
        f"-PcmakeArgs={' '.join(cmake_args)}",
        "--info"
    ]

    log_file = os.path.join(output_dir, "build.log")

    apk_path = os.path.join(SCRIPTPATH, apk_path)

    # Run Gradle build with logging
    try:
        run_command_with_logging(build_cmd, log_file, "Building Android APK")
        print(f"Build successful! APK created at: {apk_path}")
        return apk_path

    except Exception as e:
        print(f"Build failed with exception: {e}")
        raise Exception()


def install_and_run_apk(apk_path):
    """Install APK and run the application."""
    package_name = "io.tqjxlm.sparkle"
    activity_name = f"{package_name}/{package_name}.VulkanActivity"

    print("Installing APK...")
    install_cmd = ["adb", "install", apk_path]
    result = subprocess.run(install_cmd)
    if result.returncode != 0:
        print("APK installation failed!")
        return

    print("Starting application...")
    start_cmd = ["adb", "shell", "am", "start", "-n", activity_name]
    result = subprocess.run(start_cmd)
    if result.returncode != 0:
        print("Failed to start application!")
        return

    print("Monitoring application logs (Ctrl+C to stop)...")
    try:
        logcat_cmd = ["adb", "logcat", "-s", "sparkle"]
        subprocess.run(logcat_cmd)
    except KeyboardInterrupt:
        print("\nStopping log monitoring...")

    # Try to pull output log
    print("Pulling application output log...")
    pull_cmd = ["adb", "pull",
                f"/sdcard/Android/data/{package_name}/files/logs/output.log", "."]
    subprocess.run(pull_cmd)


def sync_only(args):
    """Trigger Gradle sync without building to generate CMake files."""
    gradlew_path, output_dir = prepare_environment(args)

    # Prepare Gradle sync command - use generateJsonModelDebug to trigger CMake configure
    config = args["config"]
    cmake_args = args["cmake_options"]

    if config == "Debug":
        sync_cmd = [
            gradlew_path,
            "generateJsonModelDebug",
            f"-PcmakeArgs={cmake_args}",
            "--info",
        ]
    else:
        sync_cmd = [
            gradlew_path,
            "generateJsonModelRelease",
            f"-PcmakeArgs={cmake_args}",
            "--info",
        ]

    log_file = os.path.join(output_dir, "sync.log")

    # Run Gradle sync with logging
    try:
        run_command_with_logging(sync_cmd, log_file, "Syncing Android project")
        print("Gradle sync successful! CMake files generated.")
        return True

    except Exception as e:
        print(f"Gradle sync failed with exception: {e}")
        return False


def build_and_run(args):
    """Build Android APK and optionally run it."""
    setup_android_validation(SCRIPTPATH)

    apk_path = build_apk(args)

    if args["run"]:
        install_and_run_apk(apk_path)

    return apk_path
