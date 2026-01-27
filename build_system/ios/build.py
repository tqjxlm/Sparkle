import os
import subprocess

from build_system.utils import compress_zip, run_command_with_logging, robust_rmtree
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


def run_on_device(app_path):
    # Install and run on device using xcrun devicectl
    try:
        # List connected devices
        list_devices_cmd = ["xcrun", "devicectl", "list", "devices"]
        result = subprocess.run(
            list_devices_cmd, capture_output=True, text=True, env=os.environ.copy())

        if result.returncode != 0:
            print(
                "Error: Failed to list devices. Make sure a device is connected and trusted.")
            raise Exception("Failed to list devices")

        # Parse device output to find connected devices
        devices = []
        for line in result.stdout.split('\n'):
            if ('iPhone' in line or 'iPad' in line) and ('connected' in line):
                # Extract device identifier
                parts = line.split()
                for part in parts:
                    if len(part) == 36 and '-' in part:  # Device UDID format
                        devices.append(part)
                        break

        if not devices:
            print(
                "No iOS devices found. Please connect and trust an iOS device.")
            raise Exception("Failed to find devices")

        device_id = devices[0]  # Use first available device
        print(f"\nInstalling app on device: {device_id}")

        # Install the app
        install_cmd = ["xcrun", "devicectl", "device", "install", "app",
                       "--device", device_id, app_path]

        print(f"Installing app with command: {' '.join(install_cmd)}")
        install_result = subprocess.run(
            install_cmd, capture_output=True, text=True, env=os.environ.copy())
        if install_result.returncode != 0:
            print(f"Error installing app: {install_result.stderr}")
            print(
                "The app may already be installed or there may be signing issues.")
            print("Try manually installing through Xcode.")
            raise Exception("Failed to install app")

        print("App installed successfully!")

        # Launch the app
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


def get_app_path():
    output_dir = get_output_dir()
    app_name = "sparkle.app"
    # CMake outputs to the same directory regardless of config (RUNTIME_OUTPUT_DIRECTORY)
    return os.path.join(output_dir, app_name)


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

        print("Configuring CMake for clangd (iOS) with command:",
              " ".join(cmake_cmd))
        result = subprocess.run(cmake_cmd, env=os.environ.copy())
        if result.returncode != 0:
            print("CMake configure failed.")
            raise Exception()

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
            # Require a team id to sign automatically. It will be used by cmake later.
            team_id = os.environ.get("APPLE_DEVELOPER_TEAM_ID")
            if team_id:
                sign_args = ["-DENABLE_APPLE_AUTO_SIGN=ON"]
            else:
                print(
                    "Error: APPLE_DEVELOPER_TEAM_ID environment variable is not set."
                    "Please set APPLE_DEVELOPER_TEAM_ID. https://developer.apple.com/help/account/manage-your-team/locate-your-team-id/")
                raise Exception()

        generator_args = ["-G Xcode"]

        cmake_cmd = [
            args["cmake_executable"],
            "../../..",
        ] + generator_args + toolchain_args + args["cmake_options"] + sign_args + platform_args

        print("Generating iOS Xcode project with command:", " ".join(cmake_cmd))
        result = subprocess.run(cmake_cmd, env=os.environ.copy())
        if result.returncode != 0:
            print("CMake project generation failed.")
            raise Exception()

        print(
            f"Xcode project is generated at {output_dir}. Open with command:")
        print(f"open {output_dir}/sparkle.xcodeproj")

    def build(self, args):
        """Build the project."""
        self.generate_project(args)

        # Build
        build_cmd = [args["cmake_executable"], "--build", ".", "--config",
                     args["config"], "--target", "sparkle"]

        output_dir = get_output_dir()
        log_file = os.path.join(output_dir, "build.log")

        run_command_with_logging(build_cmd, log_file, "Building iOS project")

    def archive(self, args):
        """Archive the built project."""
        output_dir = get_output_dir()
        app_path = get_app_path()
        archive_path = os.path.join(output_dir, "sparkle.ipa")

        compress_zip(app_path, archive_path)

        return archive_path

    def run(self, args):
        """Run the built project."""
        app_path = get_app_path()

        if os.path.exists(app_path):
            print(f"\nBuilt app bundle is available at: {app_path}")
            run_on_device(app_path)
        else:
            print(
                f"\nWarning: App bundle not found at expected location: {app_path}")
            print("Check the build output directory for the actual location.")
            raise Exception("App bundle not found")
