import hashlib
import json
import os
import re
import select
import shutil
import subprocess
import sys
import time
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


PACKAGE_NAME = "io.tqjxlm.sparkle"
ACTIVITY_NAME = f"{PACKAGE_NAME}/{PACKAGE_NAME}.VulkanActivity"
DEVICE_FILES_DIR = f"/sdcard/Android/data/{PACKAGE_NAME}/files"
DEVICE_OUTPUT_DIR = os.path.join(SCRIPTPATH, "output", "device")

EMULATOR_AVD_NAME = "sparkle_test"
EMULATOR_API_LEVEL = 35

# runner-side commands a TestCase may request through a "[TestCaseAction] <name>" log line.
# cycle_window goes through the home screen rather than the power key: emulators count as
# permanently charging, and stay-on defaults vary per image, so a sleep keyevent may leave
# the surface untouched
TEST_CASE_ACTIONS = {
    "cycle_window": (
        ["shell", "am", "start", "-a", "android.intent.action.MAIN",
         "-c", "android.intent.category.HOME"],
        ["shell", "am", "start", "-n",
         "io.tqjxlm.sparkle/io.tqjxlm.sparkle.VulkanActivity"],
    ),
}

TEST_PASS_PATTERN = re.compile(r"Test case '.+' passed")
TEST_FAIL_PATTERNS = (
    re.compile(r"Test case '.+' failed"),
    re.compile(r"TestCase '.+' is not registered"),
    re.compile(r"Failed to init"),
    re.compile(r"Fatal signal|FATAL EXCEPTION"),
)
TEST_ACTION_PATTERN = re.compile(r"\[TestCaseAction\] (\w+)")


def sdk_tool(*relative):
    path = os.path.join(find_android_sdk(), *relative)
    if platform.system() == "Windows" and not os.path.exists(path):
        path += ".exe" if not path.endswith(".bat") else ""
    return path


def adb(*args, check=True, capture=False, timeout=None):
    adb_binary = sdk_tool("platform-tools", "adb")
    if not os.path.exists(adb_binary):
        adb_binary = "adb"
    return subprocess.run([adb_binary] + list(args), check=check, text=True,
                          capture_output=capture, timeout=timeout)


def online_devices():
    result = adb("devices", capture=True)
    return [line.split()[0] for line in result.stdout.splitlines()[1:]
            if line.strip().endswith("device")]


def run_sdk_install(sdkmanager, package):
    """sdkmanager floods a progress bar and prompts for licenses; keep the log
    clean and answer yes, surfacing the output only on failure."""
    result = subprocess.run([sdkmanager, "--install", package],
                            input="y\n" * 8, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"sdkmanager --install {package} failed:\n"
                           f"{result.stdout[-2000:]}\n{result.stderr[-2000:]}")


def ensure_emulator():
    """Make sure a device is online, booting the project emulator if none is.

    Installs the system image and creates the AVD on first use. The image ABI
    follows the host (the emulator only accelerates same-arch guests), so the
    installed apk must match; google_apis images allow adb root, which the
    config push relies on.
    """
    if online_devices():
        return

    host_abi = "arm64-v8a" if platform.machine().lower() in ("arm64", "aarch64") else "x86_64"
    image = f"system-images;android-{EMULATOR_API_LEVEL};google_apis;{host_abi}"

    sdkmanager = sdk_tool("cmdline-tools", "latest", "bin", "sdkmanager")
    avdmanager = sdk_tool("cmdline-tools", "latest", "bin", "avdmanager")
    for tool in (sdkmanager, avdmanager):
        if not os.path.exists(tool):
            raise RuntimeError(f"{tool} not found; install the Android SDK"
                               " cmdline-tools package")

    # pin the AVD home so avdmanager and the emulator cannot disagree on it
    avd_home = os.environ.setdefault("ANDROID_AVD_HOME",
                                     os.path.expanduser("~/.android/avd"))
    os.makedirs(avd_home, exist_ok=True)

    emulator = sdk_tool("emulator", "emulator")
    if not os.path.exists(emulator):
        print("Installing emulator...", flush=True)
        run_sdk_install(sdkmanager, "emulator")

    if not os.path.isdir(os.path.join(find_android_sdk(), *image.split(";"))):
        print(f"Installing {image}...", flush=True)
        run_sdk_install(sdkmanager, image)

    if not os.path.isdir(os.path.join(avd_home, f"{EMULATOR_AVD_NAME}.avd")):
        print(f"Creating AVD {EMULATOR_AVD_NAME}...", flush=True)
        subprocess.run([avdmanager, "create", "avd", "-n", EMULATOR_AVD_NAME,
                        "-k", image, "--force"], check=True, input="no\n", text=True)

    for probe in (["-list-avds"], ["-accel-check"]):
        result = subprocess.run([emulator] + probe, capture_output=True, text=True)
        print(f"emulator {probe[0]}: {(result.stdout + result.stderr).strip()}", flush=True)

    log_path = os.path.join(SCRIPTPATH, "output", "emulator.log")
    os.makedirs(os.path.dirname(log_path), exist_ok=True)

    snapshot = os.path.join(avd_home, f"{EMULATOR_AVD_NAME}.avd",
                            "snapshots", "default_boot", "snapshot.pb")
    if not os.path.isfile(snapshot):
        # a clean shutdown is what writes the quickboot snapshot, taken only after
        # the boot-broadcast storm (which kills activities started into it) has
        # settled; later boots — and CI runs restoring the cached AVD — resume from
        # it in seconds
        print("Seeding quickboot snapshot...", flush=True)
        process = boot_and_wait(emulator, log_path)
        adb("shell", "locksettings", "set-disabled", "true", check=False)
        time.sleep(15)
        adb("emu", "kill", check=False)
        process.wait(timeout=120)

    boot_and_wait(emulator, log_path)
    print("Emulator booted.", flush=True)


def boot_and_wait(emulator, log_path):
    # no -gpu override: headless auto mode selects swangle for GL and lavapipe for
    # Vulkan. forcing swiftshader_indirect would force the SwiftShader Vulkan ICD
    # with it, which crashes the emulator on slang-generated SPIR-V (source
    # language 11)
    boot_command = [emulator, "-avd", EMULATOR_AVD_NAME, "-no-window", "-no-audio",
                    "-no-boot-anim", "-memory", "4096", "-cores", "4"]
    with open(log_path, "w") as log_file:
        emulator_process = subprocess.Popen(boot_command, stdout=log_file,
                                            stderr=subprocess.STDOUT)

    deadline = time.time() + 300
    while time.time() < deadline:
        if emulator_process.poll() is not None:
            with open(log_path, errors="replace") as log_file:
                tail = "".join(log_file.readlines()[-30:])
            raise RuntimeError(f"emulator exited with code"
                               f" {emulator_process.returncode}:\n{tail}")
        result = adb("shell", "getprop", "sys.boot_completed",
                     check=False, capture=True)
        if result.stdout.strip() == "1":
            return emulator_process
        time.sleep(2)

    emulator_process.kill()
    raise RuntimeError(f"emulator did not boot in time; see {log_path}")


def adb_restart_as_root():
    result = adb("root", check=False, capture=True)
    adb("wait-for-device", timeout=60)
    return "cannot run as root" not in (result.stdout + result.stderr)


def install_apk_once(apk_path):
    """Skip the ~250 MB install when the connected device already has this apk."""
    with open(apk_path, "rb") as apk_file:
        sha1 = hashlib.sha1(apk_file.read()).hexdigest()

    marker = "/data/local/tmp/sparkle_apk.sha1"
    installed = adb("shell", "cat", marker, check=False, capture=True).stdout.strip()
    if installed == sha1:
        print("APK unchanged on device, skipping install")
        return

    print(f"Installing {apk_path}...")
    adb("install", "-r", apk_path)
    adb("shell", f"echo {sha1} > {marker}")


def ensure_app_data_dirs():
    """The app's external files tree exists only after a first launch, so a fresh
    install on a pristine device needs one warm-up start."""
    if adb("shell", "test", "-d", DEVICE_FILES_DIR, check=False).returncode == 0:
        return

    print("First launch to create app storage...", flush=True)
    adb("shell", "am", "start", "-n", ACTIVITY_NAME)
    deadline = time.time() + 60
    while time.time() < deadline:
        if adb("shell", "test", "-d", DEVICE_FILES_DIR, check=False).returncode == 0:
            break
        time.sleep(2)
    else:
        raise RuntimeError(f"{DEVICE_FILES_DIR} did not appear after first launch")
    adb("shell", "am", "force-stop", PACKAGE_NAME)


def parse_cvar_args(unknown_args):
    """Interpret pass-through app arguments (--name value pairs) as config values.

    Android has no argv channel; the values ship through the runtime config file."""
    if len(unknown_args) % 2 != 0:
        raise RuntimeError(f"expected --name value pairs, got {unknown_args}")

    cvars = {}
    for name, value in zip(unknown_args[::2], unknown_args[1::2]):
        if not name.startswith("--"):
            raise RuntimeError(f"expected an option name, got {name}")
        try:
            cvars[name[2:]] = json.loads(value)
        except json.JSONDecodeError:
            cvars[name[2:]] = value
    return cvars


def push_test_config(cvars, rooted):
    """Deliver the runtime config. Shell-owned files under Android/data are invisible
    to the app through FUSE, so with root the file is staged and chown'd to the app."""
    local_path = os.path.join(DEVICE_OUTPUT_DIR, "config.json")
    os.makedirs(DEVICE_OUTPUT_DIR, exist_ok=True)
    with open(local_path, "w") as config_file:
        json.dump(cvars, config_file, indent=2)

    config_dir = f"{DEVICE_FILES_DIR}/config"
    if rooted:
        staged = "/data/local/tmp/sparkle_config.json"
        adb("push", local_path, staged)
        owner = adb("shell", "stat", "-c", "%U", DEVICE_FILES_DIR,
                    capture=True).stdout.strip()
        adb("shell", f"mkdir -p {config_dir}"
            f" && cp {staged} {config_dir}/config.json"
            f" && chown -R {owner}:ext_data_rw {config_dir}")
    else:
        adb("shell", "mkdir", "-p", config_dir)
        adb("push", local_path, f"{config_dir}/config.json")


def watch_test_run(timeout_seconds):
    """Follow logcat until the TestCase verdict, serving TestCaseAction requests."""
    adb_binary = sdk_tool("platform-tools", "adb")
    # unbuffered: a quiet logcat must not block the deadline check, and select
    # only sees the fd, so no reader-side buffer may hold lines back
    logcat = subprocess.Popen(
        [adb_binary, "logcat", "-v", "brief", "sparkle:*", "DEBUG:I",
         "AndroidRuntime:E", "*:S"],
        stdout=subprocess.PIPE, bufsize=0)

    pending = b""
    quiet_polls = 0
    deadline = time.time() + timeout_seconds
    try:
        while time.time() < deadline:
            ready, _, _ = select.select([logcat.stdout], [], [],
                                        min(10, max(1, deadline - time.time())))
            if not ready:
                # a silent app that no longer runs will never produce a verdict;
                # two probes in a row absorb the startup race after am start
                pid = adb("shell", "pidof", PACKAGE_NAME,
                          check=False, capture=True).stdout.strip()
                quiet_polls = 0 if pid else quiet_polls + 1
                if quiet_polls >= 2:
                    return 2, "app process gone without a verdict"
                continue
            quiet_polls = 0
            chunk = os.read(logcat.stdout.fileno(), 65536)
            if not chunk:
                return 1, "logcat ended unexpectedly"
            *raw_lines, pending = (pending + chunk).split(b"\n")

            for raw_line in raw_lines:
                line = raw_line.decode(errors="replace")
                print(line, flush=True)

                action = TEST_ACTION_PATTERN.search(line)
                if action:
                    for command in TEST_CASE_ACTIONS[action.group(1)]:
                        time.sleep(2)
                        adb(*command, check=False)
                    continue
                if TEST_PASS_PATTERN.search(line):
                    return 0, "passed"
                if any(pattern.search(line) for pattern in TEST_FAIL_PATTERNS):
                    return 1, line.strip()
        return 1, f"timeout after {timeout_seconds}s"
    finally:
        logcat.kill()


def pull_test_artifacts():
    """Pull the run's log and screenshots where the suite and evaluators expect them."""
    logs_dir = os.path.join(DEVICE_OUTPUT_DIR, "logs")
    os.makedirs(logs_dir, exist_ok=True)
    local_log = os.path.join(logs_dir, f"output_{int(time.time() * 1000)}.log")
    adb("pull", f"{DEVICE_FILES_DIR}/logs/output.log", local_log, check=False)

    # a failed pull must not leave a stale screenshot; captures/ accumulates across cases
    screenshots_dir = os.path.join(DEVICE_OUTPUT_DIR, "screenshots")
    os.makedirs(screenshots_dir, exist_ok=True)
    for entry in os.listdir(screenshots_dir):
        if entry == "captures":
            continue
        path = os.path.join(screenshots_dir, entry)
        shutil.rmtree(path) if os.path.isdir(path) else os.remove(path)
    adb("pull", f"{DEVICE_FILES_DIR}/screenshots/.", screenshots_dir, check=False)


def packaged_apk_path(args):
    product = os.path.join(SCRIPTPATH, "product", f"android-{args['config']}.apk")
    return product if os.path.isfile(product) else None


def run_test_case(args):
    """Automated single-TestCase run on a device or emulator; returns the exit code."""
    ensure_emulator()
    rooted = adb_restart_as_root()
    if not rooted:
        print("WARNING: adb root unavailable; config push may be invisible to the app")

    apk_path = args.get("product_path") or packaged_apk_path(args)
    if not apk_path:
        raise RuntimeError("no packaged apk to test; run the package stage first"
                           f" (expected {os.path.join(SCRIPTPATH, 'product')})")
    install_apk_once(apk_path)
    ensure_app_data_dirs()

    adb("shell", "am", "force-stop", PACKAGE_NAME)
    push_test_config(parse_cvar_args(args["unknown_args"]), rooted)

    # one retry absorbs transient system kills (e.g. the app starting into a
    # just-booted, still-settling device)
    for attempt in (1, 2):
        adb("logcat", "-c", check=False)
        print(f"Starting {ACTIVITY_NAME}")
        adb("shell", "am", "start", "-n", ACTIVITY_NAME)
        code, verdict = watch_test_run(timeout_seconds=900)
        print(f"=== test run: {verdict}")
        if code != 2:
            break
        adb("shell", "am", "force-stop", PACKAGE_NAME)
    code = min(code, 1)

    adb("shell", "am", "force-stop", PACKAGE_NAME)
    pull_test_artifacts()
    adb("shell", "rm", "-f", f"{DEVICE_FILES_DIR}/config/config.json", check=False)
    return code


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


def gradle_abi_properties(args):
    abi = args.get("android_abi") if args else None
    return [f"-PtargetAbi={abi}"] if abi else []


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
    ] + gradle_abi_properties(args)

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
        ] + gradle_abi_properties(args)

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
        if "--test_case" in args["unknown_args"]:
            return run_test_case(args)
        return run_on_device(args)
