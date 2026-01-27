import os
import subprocess

from build_system.utils import compress_zip, run_command_with_logging, robust_rmtree
from build_system.builder_interface import FrameworkBuilder

SCRIPT = os.path.abspath(__file__)
SCRIPTPATH = os.path.dirname(SCRIPT)


def get_project_dir():
    """Directory for CMake/Xcode project files."""
    return os.path.join(SCRIPTPATH, "project")


def get_output_dir():
    """Directory for build output (set by CMake PRODUCT_OUTPUT_DIRECTORY)."""
    return os.path.join(SCRIPTPATH, "output", "build")


def get_app_path():
    output_dir = get_output_dir()
    app_name = "sparkle.app"
    # CMake outputs to the same directory regardless of config (RUNTIME_OUTPUT_DIRECTORY)
    return os.path.join(output_dir, app_name)


def try_archiving(app_path, archive_path):
    # your apple id. e.g. abc@gmail.com
    username = os.environ.get("APPLE_NOTARIZATION_USERNAME")
    # your notarization password. see https://support.apple.com/en-us/102654
    password = os.environ.get("APPLE_NOTARIZATION_PASSWORD")
    # your developer team id. see https://developer.apple.com/help/account/manage-your-team/locate-your-team-id/
    team_id = os.environ.get("APPLE_DEVELOPER_TEAM_ID")
    # your signing identity. Go to https://developer.apple.com/account/resources/certificates/ and add one for "Developer ID Application"
    signing_identity = os.environ.get("APPLE_SIGNING_IDENTITY")

    if not username or not password or not team_id or not signing_identity:
        print("Archiving is not fully configured. discard.")
        return False

    # Sign the app bundle first
    sign_cmd = ["codesign", "--force", "--options",
                "runtime", "--sign", signing_identity, app_path]
    completion = subprocess.run(sign_cmd)

    if completion.returncode != 0:
        print("Failed to sign the app bundle. discard.")
        return False

    # Create ZIP for submission
    compress_cmd = ["ditto", "-c", "-k",
                    "--keepParent", app_path, archive_path]
    completion = subprocess.run(compress_cmd)

    if completion.returncode != 0:
        print("Failed to create archive for notarization. discard.")
        return False

    # Submit to Apple for a notarization ticket (which will be saved on apple server)
    notarize_cmd = [
        "xcrun", "notarytool", "submit", archive_path,
        "--apple-id", username,
        "--password", password,
        "--team-id", team_id,
        "--wait"
    ]
    completion = subprocess.run(notarize_cmd)

    if completion.returncode != 0:
        print("Failed to notarize with notarytool. discard.")
        return False

    # Staple ticket to the original .app (request ticket from the apple server)
    staple_cmd = ["xcrun", "stapler", "staple", app_path]
    completion = subprocess.run(staple_cmd)

    if completion.returncode != 0:
        print("Failed to staple. discard.")
        return False

    # Compress again to create the final distribution ZIP
    completion = subprocess.run(compress_cmd)

    if completion.returncode != 0:
        print("Failed to create final archive after notarization. discard.")
        return False

    return True


class MacosBuilder(FrameworkBuilder):
    """macOS framework builder implementation."""

    def __init__(self):
        super().__init__("macos")

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
        ] + generator_args + compiler_args + args["cmake_options"]

        print("Configuring CMake for clangd with command:", " ".join(cmake_cmd))
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

        generator_args = ["-G Xcode"]

        cmake_cmd = [
            args["cmake_executable"],
            "../../..",
        ] + generator_args + args["cmake_options"]

        print("Generating Xcode project with command:", " ".join(cmake_cmd))
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

        build_cmd = [args["cmake_executable"], "--build", ".", "--config",
                     args["config"], "--target", "sparkle"]

        output_dir = get_output_dir()
        log_file = os.path.join(output_dir, "build.log")

        run_command_with_logging(build_cmd, log_file, "Building macOS project")

    def archive(self, args):
        """Archive the built project."""

        output_dir = get_output_dir()
        app_path = get_app_path()
        archive_path = os.path.join(output_dir, "product.zip")

        # try archiving with apple toolchain
        archived = try_archiving(app_path, archive_path)

        # not able to archive, leave it as is (may be processed by external workflows, e.g. github actions)
        if not archived:
            print("Failed to archive the app, just create a plain zip archive")
            compress_zip(app_path, archive_path)

        return archive_path

    def run(self, args):
        """Run the built project."""
        app_path = get_app_path()

        if os.path.exists(app_path):
            executable_path = os.path.join(
                app_path, "Contents", "MacOS", "sparkle")
            if os.path.exists(executable_path):
                print(f"Running application: {executable_path}")
                run_cmd = [executable_path] + args["unknown_args"]
                subprocess.run(run_cmd, env=os.environ.copy())
            else:
                print(f"Error: Executable not found at {executable_path}")
                raise Exception()
        else:
            print(f"Error: Application bundle not found at {app_path}")
            raise Exception()
