"""
Shared CMake-through-Xcode driver for the Apple frameworks.
"""

import os

from build_system.builder_interface import FrameworkBuilder
from build_system.utils import run_checked, run_command_with_logging, robust_rmtree

# the Apple frameworks build with the toolchain clang rather than a discovered LLVM
CLANG_ARGS = ["-DCMAKE_C_COMPILER=/usr/bin/clang",
              "-DCMAKE_CXX_COMPILER=/usr/bin/clang++"]


def project_dir(script_path):
    """Directory for CMake/Xcode project files."""
    return os.path.join(script_path, "project")


def output_dir(script_path):
    """Directory for build output (set by CMake PRODUCT_OUTPUT_DIRECTORY)."""
    return os.path.join(script_path, "output", "build")


class XcodeBuilder(FrameworkBuilder):
    """Configure, generate and build an Xcode project. Subclasses name the framework
    and contribute the cmake and build arguments that distinguish their platform."""

    def __init__(self, framework_name, script_path):
        super().__init__(framework_name)
        self.script_path = script_path

    def get_project_dir(self):
        return project_dir(self.script_path)

    def get_output_dir(self):
        return output_dir(self.script_path)

    def platform_cmake_args(self, args):
        """Arguments every cmake invocation for this platform needs."""
        return []

    def project_cmake_args(self, args):
        """Arguments only the Xcode project generation needs, e.g. signing."""
        return []

    def extra_build_args(self, args):
        """Arguments appended after the cmake --build command line."""
        return []

    def prepare_project_dir(self, args):
        """Bring the project directory into the state the generation expects."""
        if args.get("clean", False):
            robust_rmtree(self.get_project_dir())
        os.makedirs(self.get_project_dir(), exist_ok=True)

    def configure_for_clangd(self, args):
        """Configure CMake for clangd support."""
        clangd_dir = os.path.join(self.script_path, "clangd")

        if args.get("clean", False):
            robust_rmtree(clangd_dir)

        os.makedirs(clangd_dir, exist_ok=True)
        os.chdir(clangd_dir)

        cmake_cmd = [
            args["cmake_executable"],
            "../../..",
            f"-DCMAKE_BUILD_TYPE={args['config']}",
        ] + CLANG_ARGS + args["cmake_options"] + self.platform_cmake_args(args)

        run_checked(cmake_cmd, "CMake configure failed.",
                    f"Configuring CMake for clangd ({self.framework_name})")

        print(f"Configuration complete in {clangd_dir}")

    def generate_project(self, args):
        """Generate Xcode project files."""
        self.prepare_project_dir(args)
        os.chdir(self.get_project_dir())

        cmake_cmd = [
            args["cmake_executable"],
            "../../..",
            "-G Xcode",
        ] + args["cmake_options"] + self.platform_cmake_args(args) + self.project_cmake_args(args)

        run_checked(cmake_cmd, "CMake project generation failed.",
                    f"Generating {self.framework_name} Xcode project")

        print(f"Xcode project is generated at {self.get_project_dir()}. Open with command:")
        print(f"open {self.get_project_dir()}/sparkle.xcodeproj")

    def configure_only(self, args):
        """The Xcode build configures through project generation."""
        self.generate_project(args)

    def build(self, args):
        """Build the project."""
        self.generate_project(args)

        build_cmd = [args["cmake_executable"], "--build", ".", "--config",
                     args["config"], "--target", "sparkle"] + self.extra_build_args(args)

        log_file = os.path.join(self.get_output_dir(), "build.log")

        run_command_with_logging(build_cmd, log_file, f"Building {self.framework_name} project")
