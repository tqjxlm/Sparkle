import json
import os
import subprocess
import zipfile

from build_system.utils import run_command_with_logging, robust_rmtree
from build_system.builder_interface import FrameworkBuilder

SCRIPT = os.path.abspath(__file__)
SCRIPTPATH = os.path.dirname(SCRIPT)

platform_args = ["-DPLATFORM=OS64", "-DCMAKE_SYSTEM_NAME=iOS",
                 "-DDEPLOYMENT_TARGET=18.0", "-DENABLE_BITCODE=FALSE"]
toolchain_args = [
    f"-DCMAKE_TOOLCHAIN_FILE={os.path.join(SCRIPTPATH, 'ios.toolchain.cmake')}"]


def get_project_dir():
    """Directory for CMake/Xcode project files."""
    return os.path.join(SCRIPTPATH, "project")


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
        ] + generator_args + toolchain_args + args["cmake_options"] + compiler_args + platform_args

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

        os.makedirs(output_dir, exist_ok=True)
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
        ] + generator_args + toolchain_args + args["cmake_options"] + sign_args + platform_args

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
