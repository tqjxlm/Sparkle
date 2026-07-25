import os
import subprocess

from build_system.utils import compress_zip
from build_system.xcode_builder import XcodeBuilder, output_dir

SCRIPT = os.path.abspath(__file__)
SCRIPTPATH = os.path.dirname(SCRIPT)


def get_output_dir():
    return output_dir(SCRIPTPATH)


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

    sign_cmd = ["codesign", "--force", "--options",
                "runtime", "--sign", signing_identity, app_path]
    compress_cmd = ["ditto", "-c", "-k",
                    "--keepParent", app_path, archive_path]
    notarize_cmd = [
        "xcrun", "notarytool", "submit", archive_path,
        "--apple-id", username,
        "--password", password,
        "--team-id", team_id,
        "--wait"
    ]
    staple_cmd = ["xcrun", "stapler", "staple", app_path]

    # notarization submits a signed zip, then staples the returned ticket to the .app;
    # the final zip is the one that carries the ticket
    steps = [
        (sign_cmd, "Failed to sign the app bundle."),
        (compress_cmd, "Failed to create archive for notarization."),
        (notarize_cmd, "Failed to notarize with notarytool."),
        (staple_cmd, "Failed to staple."),
        (compress_cmd, "Failed to create final archive after notarization."),
    ]

    for cmd, failure_message in steps:
        if subprocess.run(cmd).returncode != 0:
            print(f"{failure_message} discard.")
            return False

    return True


class MacosBuilder(XcodeBuilder):
    """macOS framework builder implementation."""

    def __init__(self):
        super().__init__("macos", SCRIPTPATH)

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
                result = subprocess.run(run_cmd, env=os.environ.copy())
                return result.returncode
            else:
                raise RuntimeError(f"Executable not found at {executable_path}")
        else:
            raise RuntimeError(f"Application bundle not found at {app_path}")
