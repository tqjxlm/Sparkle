import os
import subprocess

from build_system.utils import compress_zip, run_command_with_logging, robust_rmtree
from build_system.builder_interface import FrameworkBuilder

SCRIPT = os.path.abspath(__file__)
SCRIPTPATH = os.path.dirname(SCRIPT)


def get_output_dir():
    return os.path.join(SCRIPTPATH, "project")


def get_app_path(args):
    output_dir = get_output_dir()
    app_name = "sparkle.app"
    if args["config"] == "Debug":
        app_path = os.path.join(output_dir, "Debug", app_name)
    else:
        app_path = os.path.join(output_dir, "Release", app_name)
    return app_path


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
        output_dir = get_output_dir()

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

        # Build specific target instead of ALL_BUILD
        build_cmd = [args["cmake_executable"], "--build", ".", "--config",
                     args["config"], "--target", "sparkle"]

        output_dir = get_output_dir()
        log_file = os.path.join(output_dir, "build.log")

        run_command_with_logging(build_cmd, log_file, "Building macOS project")

    def archive(self, args):
        """Archive the built project."""
        output_dir = get_output_dir()
        app_path = get_app_path(args)
        archive_path = os.path.join(output_dir, "product.zip")

        # TODO: archive and sign
        compress_zip(app_path, archive_path)

        return archive_path

    def run(self, args):
        """Run the built project."""
        app_path = get_app_path(args)

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
