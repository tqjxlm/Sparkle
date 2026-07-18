import json
import os
import platform
import shutil
import subprocess
import time
import zipfile

from build_system.utils import run_command_with_logging, robust_rmtree
from build_system.builder_interface import FrameworkBuilder

SCRIPT = os.path.abspath(__file__)
SCRIPTPATH = os.path.dirname(SCRIPT)

toolchain_args = [
    f"-DCMAKE_TOOLCHAIN_FILE={os.path.join(SCRIPTPATH, 'ios.toolchain.cmake')}"]


def is_simulator(args):
    return args.get("ios_platform") == "simulator"


def platform_args(args):
    """The ios-cmake toolchain platform: the device target by default, or the
    host-arch simulator for --ios_platform simulator."""
    if is_simulator(args):
        arm64_host = platform.machine().lower() in ("arm64", "aarch64")
        target = "SIMULATORARM64" if arm64_host else "SIMULATOR64"
    else:
        target = "OS64"
    return [f"-DPLATFORM={target}", "-DCMAKE_SYSTEM_NAME=iOS",
            "-DDEPLOYMENT_TARGET=18.0", "-DENABLE_BITCODE=FALSE"]


def get_project_dir():
    """Directory for CMake/Xcode project files."""
    return os.path.join(SCRIPTPATH, "project")


def reset_on_platform_switch(args):
    """Device and simulator builds cannot share CMake caches or build products;
    a platform switch resets both trees."""
    stamp_path = os.path.join(get_project_dir(), ".ios_platform")
    current = "simulator" if is_simulator(args) else "device"
    if os.path.isfile(stamp_path):
        with open(stamp_path) as stamp_file:
            previous = stamp_file.read().strip()
        if previous != current:
            print(f"iOS target platform changed ({previous} -> {current}), cleaning outputs")
            robust_rmtree(get_project_dir())
            robust_rmtree(get_output_dir())
    os.makedirs(get_project_dir(), exist_ok=True)
    with open(stamp_path, "w") as stamp_file:
        stamp_file.write(current)


def get_output_dir():
    """Directory for build output (set by CMake PRODUCT_OUTPUT_DIRECTORY)."""
    return os.path.join(SCRIPTPATH, "output", "build")


def get_app_path():
    return os.path.join(get_output_dir(), "sparkle.app")


def signing_identity(app_path):
    """The identity that signed the bundle, or None for unsigned/ad-hoc bundles."""
    result = subprocess.run(["codesign", "-dvv", app_path],
                            capture_output=True, text=True)
    if result.returncode != 0:
        return None
    for line in result.stderr.splitlines():
        if line.startswith("Authority="):
            return line.split("=", 1)[1]
    return None


def extract_ipa(ipa_path, destination):
    """ditto, not python zipfile: extraction must preserve the executable bit."""
    robust_rmtree(destination)
    os.makedirs(destination)
    subprocess.run(["ditto", "-x", "-k", ipa_path, destination], check=True)
    return os.path.join(destination, "Payload", "sparkle.app")


def resign_ipa(ipa_path):
    """Injection invalidates the app bundle's code seal; re-sign with the
    identity that signed the build. An unsigned bundle stays unsigned - it was
    never device-installable in the first place."""
    work_dir = os.path.join(SCRIPTPATH, "output", "resign")
    app_path = extract_ipa(ipa_path, work_dir)

    identity = signing_identity(app_path)
    if identity is None:
        print(f"WARNING: {ipa_path} is not signed with an identity; skipping re-sign")
        return

    subprocess.run(["codesign", "--force", "--sign", identity,
                    "--preserve-metadata=entitlements", app_path], check=True)
    os.remove(ipa_path)
    subprocess.run(["ditto", "-c", "-k", work_dir, ipa_path], check=True)
    print(f"re-signed {ipa_path} with identity: {identity}")


def find_ios_device():
    """Identifier and name of a paired, reachable iOS device, or None. devicectl
    opens the tunnel on demand at install time, so a paired device that is not
    actively tethered is still a valid target; only an 'unavailable' tunnel is
    out of reach."""
    result = subprocess.run(
        ["xcrun", "devicectl", "list", "devices", "--json-output", "-"],
        capture_output=True, text=True, env=os.environ.copy())
    if result.returncode != 0:
        raise RuntimeError("Failed to list devices via devicectl.")

    for device in json.loads(result.stdout)["result"]["devices"]:
        connection = device.get("connectionProperties", {})
        if device.get("hardwareProperties", {}).get("platform") != "iOS":
            continue
        if connection.get("pairingState") != "paired":
            continue
        if connection.get("tunnelState") == "unavailable":
            continue
        return device["identifier"], device.get("deviceProperties", {}).get("name", "")
    return None


def run_on_device(app_path):
    # Install and run on device using xcrun devicectl
    try:
        device = find_ios_device()
        if device is None:
            raise RuntimeError("No reachable iOS device found. Connect or pair an iOS device and trust this Mac.")

        device_id, device_name = device
        print(f"\nInstalling app on device: {device_name} ({device_id})")

        install_cmd = ["xcrun", "devicectl", "device", "install", "app",
                       "--device", device_id, app_path]

        print(f"Installing app with command: {' '.join(install_cmd)}")
        install_result = subprocess.run(
            install_cmd, capture_output=True, text=True, env=os.environ.copy())
        if install_result.returncode != 0:
            print(f"Error installing app: {install_result.stderr}")
            print("The app may already be installed or there may be signing issues.")
            print("Try manually installing through Xcode.")
            raise RuntimeError("Failed to install app")

        print("App installed successfully!")

        bundle_id = "io.tqjxlm.sparkle"
        launch_cmd = ["xcrun", "devicectl", "device", "process", "launch",
                      "--device", device_id, bundle_id]

        print(f"Launching app with command: {' '.join(launch_cmd)}")
        launch_result = subprocess.run(
            launch_cmd, capture_output=True, text=True, env=os.environ.copy())

        if launch_result.returncode != 0:
            print(f"Error launching app: {launch_result.stderr}")
            print("App was installed but failed to launch automatically.")
            print("You can manually launch it from the device.")
        else:
            print("App launched successfully on device!")
            print("For now, there's no built-in way to print logs to console. If you want to see app log, please open xcode project and run manually.")
        return
    except FileNotFoundError:
        print("\nError: xcrun not found. Make sure Xcode command line tools are installed.")
        print("Run: xcode-select --install")
    except Exception as e:
        print(f"\nError during device deployment: {e}")

    # fallback to manual deployment
    print("Please follow manual deployment instructions:")
    print("1. Open the generated Xcode project")
    print("2. Select your target device")
    print("3. Build and run from Xcode")


SIMULATOR_DEVICE_NAME = "sparkle_test"
DEVICE_OUTPUT_DIR = os.path.join(SCRIPTPATH, "output", "device")
TEST_TIMEOUT_SECONDS = 900


def simctl(*args, check=True, capture=False, timeout=None):
    return subprocess.run(["xcrun", "simctl"] + list(args), check=check, text=True,
                          capture_output=capture, timeout=timeout, env=os.environ.copy())


def newest_iphone_device_type():
    """simctl lists device types oldest first; the last iPhone is the newest model."""
    result = simctl("list", "devicetypes", "--json", capture=True)
    iphones = [device_type["identifier"]
               for device_type in json.loads(result.stdout)["devicetypes"]
               if device_type.get("productFamily") == "iPhone"]
    if not iphones:
        raise RuntimeError("no iPhone simulator device types available; install an iOS"
                           " runtime through Xcode")
    return iphones[-1]


def find_simulator():
    result = simctl("list", "devices", "--json", capture=True)
    for runtime, devices in json.loads(result.stdout)["devices"].items():
        if "SimRuntime.iOS" not in runtime:
            continue
        for device in devices:
            if device.get("name") == SIMULATOR_DEVICE_NAME and device.get("isAvailable"):
                return device["udid"], device.get("state")
    return None, None


def ensure_simulator():
    """The udid of a booted project simulator, creating and booting it on first use.
    The simulated screen is irrelevant: test runs render headless at the configured
    resolution, so any iPhone model works."""
    udid, state = find_simulator()
    if udid is None:
        device_type = newest_iphone_device_type()
        print(f"Creating simulator {SIMULATOR_DEVICE_NAME} ({device_type})...", flush=True)
        udid = simctl("create", SIMULATOR_DEVICE_NAME, device_type,
                      capture=True).stdout.strip()
        state = "Shutdown"
    if state != "Booted":
        print(f"Booting simulator {udid}...", flush=True)
        simctl("boot", udid)
        simctl("bootstatus", udid, "-b", timeout=300)
    return udid


def simulator_home(udid):
    """The spawned process's HOME: external storage (Documents) lives under it."""
    result = simctl("getenv", udid, "HOME", capture=True, check=False)
    home = result.stdout.strip()
    if result.returncode == 0 and home:
        return home
    return os.path.expanduser(
        os.path.join("~", "Library", "Developer", "CoreSimulator", "Devices", udid, "data"))


def collect_test_artifacts(sim_home):
    """Copy the run's log and screenshots where the suite and evaluators expect them."""
    logs_dir = os.path.join(DEVICE_OUTPUT_DIR, "logs")
    os.makedirs(logs_dir, exist_ok=True)
    app_log = os.path.join(sim_home, "Documents", "logs", "output.log")
    if os.path.isfile(app_log):
        shutil.copy(app_log, os.path.join(logs_dir, f"output_{int(time.time() * 1000)}.log"))

    screenshots_dir = os.path.join(DEVICE_OUTPUT_DIR, "screenshots")
    shutil.rmtree(screenshots_dir, ignore_errors=True)
    os.makedirs(screenshots_dir)
    app_screenshots = os.path.join(sim_home, "Documents", "screenshots")
    if os.path.isdir(app_screenshots):
        for name in os.listdir(app_screenshots):
            shutil.copy(os.path.join(app_screenshots, name), screenshots_dir)


def packaged_ipa_path(args):
    product = os.path.join(SCRIPTPATH, "product", f"ios-{args['config']}.ipa")
    return product if os.path.isfile(product) else None


def run_test_case(args):
    """Automated single-TestCase run inside the simulator; returns the exit code.

    The app binary must come from a --ios_platform simulator package. simctl spawn
    runs it as a plain headless process: argv passes straight through and the process
    exit code is the TestCase verdict, so no install or log scraping is needed."""
    ipa_path = args.get("product_path") or packaged_ipa_path(args)
    if not ipa_path:
        raise RuntimeError("no packaged ipa to test; run the package stage first"
                           f" (expected {os.path.join(SCRIPTPATH, 'product')})")

    app_path = extract_ipa(ipa_path, os.path.join(SCRIPTPATH, "output", "deploy"))
    binary_path = os.path.join(app_path, "sparkle")

    udid = ensure_simulator()
    sim_home = simulator_home(udid)

    command = ["xcrun", "simctl", "spawn", udid, binary_path] + list(args["unknown_args"])
    if "--headless" not in args["unknown_args"]:
        command += ["--headless", "true"]
    print(f"Running: {' '.join(command)}", flush=True)
    try:
        code = subprocess.run(command, env=os.environ.copy(),
                              timeout=TEST_TIMEOUT_SECONDS).returncode
        print(f"=== test run: exit {code}", flush=True)
    except subprocess.TimeoutExpired:
        subprocess.run(["pkill", "-f", "sparkle.app/sparkle"], check=False)
        print(f"=== test run: timeout after {TEST_TIMEOUT_SECONDS}s", flush=True)
        code = 1

    collect_test_artifacts(sim_home)
    return 0 if code == 0 else 1


class IosBuilder(FrameworkBuilder):
    """iOS framework builder implementation."""

    def __init__(self):
        super().__init__("ios")

    def configure_for_clangd(self, args):
        """Configure CMake for clangd support."""
        output_dir = os.path.join(SCRIPTPATH, "clangd")

        if args.get("clean", False):
            robust_rmtree(output_dir)

        os.makedirs(output_dir, exist_ok=True)
        os.chdir(output_dir)

        compiler_args = ["-DCMAKE_C_COMPILER=/usr/bin/clang",
                         "-DCMAKE_CXX_COMPILER=/usr/bin/clang++"]
        generator_args = [f"-DCMAKE_BUILD_TYPE={args['config']}"]

        cmake_cmd = [
            args["cmake_executable"],
            "../../..",
        ] + generator_args + toolchain_args + args["cmake_options"] + compiler_args + platform_args(args)

        print("Configuring CMake for clangd (iOS) with command:", " ".join(cmake_cmd))
        result = subprocess.run(cmake_cmd, env=os.environ.copy())
        if result.returncode != 0:
            raise RuntimeError("CMake configure failed.")

        print(f"Configuration complete in {output_dir}")

    def generate_project(self, args):
        """Generate Xcode project files."""
        output_dir = get_project_dir()

        if args.get("clean", False):
            robust_rmtree(output_dir)

        reset_on_platform_switch(args)
        os.chdir(output_dir)

        sign_args = []
        if args.get("apple_auto_sign", False):
            team_id = os.environ.get("APPLE_DEVELOPER_TEAM_ID")
            if team_id:
                sign_args = ["-DENABLE_APPLE_AUTO_SIGN=ON"]
            else:
                raise RuntimeError(
                    "APPLE_DEVELOPER_TEAM_ID environment variable is not set. "
                    "Please set APPLE_DEVELOPER_TEAM_ID. "
                    "https://developer.apple.com/help/account/manage-your-team/locate-your-team-id/")

        generator_args = ["-G Xcode"]

        cmake_cmd = [
            args["cmake_executable"],
            "../../..",
        ] + generator_args + toolchain_args + args["cmake_options"] + sign_args + platform_args(args)

        print("Generating iOS Xcode project with command:", " ".join(cmake_cmd))
        result = subprocess.run(cmake_cmd, env=os.environ.copy())
        if result.returncode != 0:
            raise RuntimeError("CMake project generation failed.")

        print(f"Xcode project is generated at {output_dir}. Open with command:")
        print(f"open {output_dir}/sparkle.xcodeproj")

    def configure_only(self, args):
        """The Xcode build configures through project generation."""
        self.generate_project(args)

    def build(self, args):
        """Build the project."""
        self.generate_project(args)

        build_cmd = [args["cmake_executable"], "--build", ".", "--config",
                     args["config"], "--target", "sparkle"]

        if args.get("apple_auto_sign", False):
            build_cmd += ["--", "-allowProvisioningUpdates"]

        output_dir = get_output_dir()
        log_file = os.path.join(output_dir, "build.log")

        run_command_with_logging(build_cmd, log_file, "Building iOS project")

    def archive(self, args):
        """Archive the built project as an IPA (requires Payload/ directory structure)."""
        output_dir = get_output_dir()
        app_path = get_app_path()
        archive_path = os.path.join(output_dir, "sparkle.ipa")

        if not os.path.isdir(app_path):
            raise FileNotFoundError(f"App bundle not found: {app_path}")

        app_name = os.path.basename(app_path)
        with zipfile.ZipFile(archive_path, 'w', zipfile.ZIP_DEFLATED) as zf:
            for root, dirs, files in os.walk(app_path):
                for file in files:
                    file_path = os.path.join(root, file)
                    arcname = os.path.join(
                        "Payload", app_name,
                        os.path.relpath(file_path, app_path))
                    zf.write(file_path, arcname)

        return archive_path

    def resign_package(self, package_path):
        resign_ipa(package_path)

    def run(self, args):
        """Run the built project."""
        if "--test_case" in args["unknown_args"]:
            return run_test_case(args)

        product_path = args.get("product_path")
        if product_path:
            app_path = extract_ipa(product_path, os.path.join(SCRIPTPATH, "output", "deploy"))
        else:
            app_path = get_app_path()
            print("Installing the raw build output; without the package stage"
                  " it carries no cooked content and the device cooks on the fly.")

        if not os.path.exists(app_path):
            raise RuntimeError(f"App bundle not found at expected location: {app_path}")

        print(f"\nApp bundle to deploy: {app_path}")
        run_on_device(app_path)
