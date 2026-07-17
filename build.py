import os
import platform
import shlex
import subprocess
import sys
import argparse
import shutil
from pathlib import Path

from build_system.prerequisites import find_cmake, find_and_set_vulkan_sdk, find_slangc

SCRIPT = os.path.abspath(__file__)
SCRIPTPATH = os.path.dirname(SCRIPT)

STAGES = ("build", "cook", "package")

# the cooker lives in the sparkle binary; cross-compiled frameworks cook through the host
# framework's binary, and linux has no supported framework that could provide one
COOK_FRAMEWORKS = ("glfw", "macos")

# device targets run from an installed package, so cooked content can only reach them
# through the package stage; desktop runs read the cook output from the build tree directly
DEVICE_FRAMEWORKS = ("android", "ios")

if platform.system() == "Darwin":
    HOST_COOK_FRAMEWORK = "macos"
elif platform.system() == "Windows":
    HOST_COOK_FRAMEWORK = "glfw"
else:
    HOST_COOK_FRAMEWORK = None

BINARY_PATH = {
    "glfw": os.path.join("build_system", "glfw", "output", "build",
                         "sparkle.exe" if platform.system() == "Windows" else "sparkle"),
    "macos": os.path.join("build_system", "macos", "output", "build",
                          "sparkle.app", "Contents", "MacOS", "sparkle"),
}

# where a host cook run leaves its artifacts (the app's internal storage)
COOKED_OUTPUT_DIR = {
    "glfw": os.path.join("build_system", "glfw", "output", "build", "generated", "cooked"),
    "macos": os.path.join("build_system", "macos", "output", "build",
                          "sparkle.app", "Contents", "SharedSupport", "cooked"),
}

COOKED_IMAGE_DIR = {
    "glfw": os.path.join("build_system", "glfw", "output", "cooked_image"),
    "macos": os.path.join("build_system", "macos", "output", "cooked_image"),
}


def cooker_framework(framework):
    """The framework whose binary executes the cook: itself when host-runnable,
    otherwise the host framework (artifacts are shared across targets). None when
    this host cannot produce a cooker binary."""
    return framework if framework in COOK_FRAMEWORKS else HOST_COOK_FRAMEWORK


def default_cooked_image_dir(framework):
    """Where the package stage finds the cooked content image by default: the host
    cooker's assembled image. None when this host cannot cook, anchored to the repo
    root otherwise because builders may chdir during build."""
    cooker = cooker_framework(framework)
    return os.path.join(SCRIPTPATH, COOKED_IMAGE_DIR[cooker]) if cooker else None


def assemble_cooked_image(cooker):
    """The cook stage's product: a self-contained content image holding every raw
    asset passed through plus the cooked artifacts. The package stage swaps a
    product's packed content for this directory."""
    image_dir = os.path.join(SCRIPTPATH, COOKED_IMAGE_DIR[cooker])
    shutil.rmtree(image_dir, ignore_errors=True)
    shutil.copytree(os.path.join(SCRIPTPATH, "resources", "packed"), image_dir)
    shutil.copytree(os.path.join(SCRIPTPATH, COOKED_OUTPUT_DIR[cooker]),
                    os.path.join(image_dir, "cooked"))
    print(f"Assembled cooked content image at {image_dir}")


def resolve_stages(stage_args, skip_build, cook, archive, framework):
    """Stage selection: explicit --stage wins, then legacy flags; the default is
    build + cook (cooked content is always wanted; hosts that cannot cook for the
    target build without it), plus package for device frameworks (a device only
    receives cooked content through its installed package)."""
    if stage_args:
        selected = set()
        for stage in stage_args:
            selected |= set(STAGES) if stage == "all" else {stage}
    elif skip_build or cook or archive:
        selected = set()
        if not skip_build:
            selected.add("build")
        if cook:
            selected.add("cook")
        if archive:
            selected.add("package")
    else:
        selected = {"build"}
        if cooker_framework(framework) is not None:
            selected.add("cook")
        if framework in DEVICE_FRAMEWORKS:
            selected.add("package")
    return [stage for stage in STAGES if stage in selected]


def validate_stages(stages, framework):
    """Stages never run an upstream stage implicitly: missing inputs fail fast."""
    if "cook" in stages:
        cooker = cooker_framework(framework)
        if cooker is None:
            raise RuntimeError(
                f"this host cannot cook for '{framework}': no supported framework produces a"
                " cooker binary on it; cook on macOS or Windows and pass --cooked when packaging")
        binary_exists = os.path.exists(os.path.join(SCRIPTPATH, BINARY_PATH[cooker]))
        # building a cross-compiled framework never produces the cooker binary
        if not binary_exists and (cooker != framework or "build" not in stages):
            raise RuntimeError(
                f"cooking for '{framework}' runs the {cooker} binary, which is missing at"
                f" {BINARY_PATH[cooker]}; build it first: python3 build.py --framework {cooker}")


def construct_additional_cmake_options(parsed_args, cmake_args=None):
    profile_settings = "-DENABLE_PROFILER=ON" if parsed_args.profile else "-DENABLE_PROFILER=OFF"
    shader_debug_settings = "-DSHADER_DEBUG=ON" if parsed_args.shader_debug else "-DSHADER_DEBUG=OFF"
    asan_settings = "-DENABLE_ASAN=ON" if parsed_args.asan else "-DENABLE_ASAN=OFF"
    test_settings = "-DENABLE_TEST_CASES=OFF" if parsed_args.strip_test else "-DENABLE_TEST_CASES=ON"
    cmake_options = [profile_settings, shader_debug_settings, asan_settings, test_settings]

    # Add additional CMake arguments if provided
    if cmake_args:
        additional_args = shlex.split(cmake_args)
        cmake_options.extend(additional_args)

    return cmake_options


def parse_args(args=None):
    parser = argparse.ArgumentParser(
        description="Parse build arguments for sparkle.")

    parser.add_argument("--framework", default="glfw",
                        choices=["glfw", "macos", "ios", "android"], help="Build framework")
    parser.add_argument("--config", default="Debug",
                        choices=["Release", "Debug"], help="Build configuration")
    parser.add_argument("--stage", action="append",
                        choices=["build", "cook", "package", "all"],
                        help="Pipeline stage(s) to run in canonical order build -> cook -> package;"
                        " repeat to select multiple. Default: build + cook. Cross-compiled"
                        " frameworks cook through the host framework's binary")
    parser.add_argument("--archive", action="store_true",
                        help="Alias for the package stage (build + package)")
    parser.add_argument("--cooked",
                        help="Cooked content image directory for the package stage"
                        " (default: the host cooker's assembled image)")
    parser.add_argument("--asan", action="store_true",
                        help="Enable AddressSanitizer")
    parser.add_argument("--profile", action="store_true",
                        help="Enable profiler")
    parser.add_argument("--shader_debug", action="store_true",
                        help="Enable shader debug")
    parser.add_argument("--generate_only", action="store_true",
                        help="Generate native project (vs sln, xcode project, etc..) without building or running")
    parser.add_argument("--configure_only", action="store_true",
                        help="Run the build's configure step (fetches dependencies) without building")
    parser.add_argument("--clangd", action="store_true",
                        help="Generate compile_commands.json for clangd")
    parser.add_argument("--cook", action="store_true",
                        help="Alias for the cook stage (build + cook; with --skip_build: cook only)")
    parser.add_argument("--run", action="store_true",
                        help="Run the built executable after building")
    parser.add_argument("--skip_build", action="store_true",
                        help="Do every thing but skip building")
    parser.add_argument("--strip_test", action="store_true",
                        help="Disable test case support (enabled by default)")
    parser.add_argument("--clean", action="store_true",
                        help="Clean output directory before configure")
    parser.add_argument("--apple_auto_sign", action="store_true",
                        help="Enable automatic code signing for Apple platforms. Requires APPLE_DEVELOPER_TEAM_ID to be set."
                        "See https://developer.apple.com/help/account/manage-your-team/locate-your-team-id/")
    parser.add_argument("--cmake-args",
                        help="Additional CMake arguments (e.g., --cmake-args='-DCMAKE_EXE_LINKER_FLAGS=\"-lc++abi\"')")

    # Unknown args will be used for --run the built executable
    parsed_args, unknown_args = parser.parse_known_args(args)

    return {
        "framework": parsed_args.framework,
        "config": parsed_args.config,
        "stages": resolve_stages(parsed_args.stage, parsed_args.skip_build,
                                 parsed_args.cook, parsed_args.archive, parsed_args.framework),
        "run": parsed_args.run,
        "cooked": parsed_args.cooked,
        "cmake_options": construct_additional_cmake_options(parsed_args, parsed_args.cmake_args),
        "unknown_args": unknown_args,
        "generate_only": parsed_args.generate_only,
        "configure_only": parsed_args.configure_only,
        "clangd": parsed_args.clangd,
        "clean": parsed_args.clean,
        "apple_auto_sign": parsed_args.apple_auto_sign,
    }


def check_environment(args):
    # Check CMake availability (skip for Android as it uses NDK's built-in CMake)
    if args["framework"] != "android":
        cmake_executable = find_cmake()
        print(f"Using CMake: {cmake_executable}")
        args["cmake_executable"] = cmake_executable

    find_and_set_vulkan_sdk()

    slangc_path = find_slangc()
    os.environ["SLANGC"] = slangc_path
    print(f"Using slangc: {slangc_path}")

    # Exit if framework is macos or ios but not running on macOS
    if args["framework"] in ("macos", "ios") and sys.platform != "darwin":
        raise RuntimeError(
            f"Framework '{args['framework']}' requires macOS, but current system is not macOS.")


def run_git_submodule_update():
    print("Updating git submodules...")
    subprocess.run(
        ["git", "submodule", "update", "--init", "--recursive", "--jobs", "8"],
        check=True
    )


def setup(args):
    """Stage-scoped setup: each step exists to feed a specific stage, so invocations
    that skip the stage skip the step (e.g. CI cook and test nodes, which never
    compile, skip the recursive submodule clone)."""
    compiling = ("build" in args["stages"] or args["generate_only"]
                 or args["configure_only"] or args["clangd"])

    if compiling:
        run_git_submodule_update()

    # the build embeds resources/packed into the product and the cook stage images it
    if compiling or "cook" in args["stages"]:
        from resources.setup import setup_resources
        setup_resources()

    if compiling:
        from ide.setup import setup_ide
        setup_ide()


def product_name(framework, config, extension):
    """Products are named by target. glfw is the one framework whose target is the host's
    own desktop OS, so only there the host system is part of the target identity."""
    if framework == "glfw":
        if platform.system() == "Windows":
            system_name = "windows"
        elif platform.system() == "Linux":
            system_name = "linux"
        elif platform.system() == "Darwin":
            system_name = "macos"
        else:
            raise RuntimeError(f"Unsupported platform: {platform.system()}")
        return f"{system_name}-glfw-{config}{extension}"
    return f"{framework}-{config}{extension}"


def copy_build_products(product_archive_path, args):
    """Copy build products to the product directory for the specified framework."""
    product_dir = os.path.join(
        SCRIPTPATH, "build_system", args['framework'], "product")

    os.makedirs(product_dir, exist_ok=True)

    extension = ''.join(Path(product_archive_path).suffixes)
    product_final_name = product_name(args['framework'], args['config'], extension)

    product_final_path = os.path.join(product_dir, product_final_name)

    if os.path.exists(product_final_path):
        os.remove(product_final_path)

    shutil.copy(product_archive_path, product_final_path)
    print(f"Build products copied to {product_final_path}")
    return product_final_path


def package_project(builder, args):
    sys.path.insert(0, os.path.join(SCRIPTPATH, "dev"))
    from package_cooked import PackagingError, package_cooked

    print("Archiving...")
    archive_path = builder.archive(args)
    product_path = copy_build_products(archive_path, args)

    image_dir = args["cooked"] or default_cooked_image_dir(args["framework"])
    try:
        placed = package_cooked(product_path, args["framework"], image_dir)
    except PackagingError as error:
        raise RuntimeError(f"package stage failed: {error}") from error

    if placed:
        builder.resign_package(product_path)
    args["product_path"] = product_path


def build_project(args):
    """Build the project after setup is complete."""

    from build_system.builder_factory import create_builder

    builder = create_builder(args["framework"])

    if args["clangd"]:
        print("Configuring...")
        builder.configure_for_clangd(args)
        return
    if args["generate_only"]:
        print("Generating project...")
        builder.generate_project(args)
        return
    if args["configure_only"]:
        print("Configuring...")
        builder.configure_only(args)
        return

    stages = args["stages"]
    validate_stages(stages, args["framework"])

    if "build" in stages:
        print("Building...")
        builder.build(args)
    else:
        print("Skipping build.")

    if "cook" in stages:
        cooker = cooker_framework(args["framework"])
        print(f"Cooking with the {cooker} binary...")
        cook_args = dict(args)
        cook_args["framework"] = cooker
        cook_args["unknown_args"] = ["--cook", "true",
                                     "--log_path", "logs/cook.log"] + args["unknown_args"]
        cook_builder = builder if cooker == args["framework"] else create_builder(cooker)
        exit_code = cook_builder.run(cook_args)
        if exit_code is not None and exit_code != 0:
            sys.exit(exit_code)
        assemble_cooked_image(cooker)

    if "package" in stages:
        package_project(builder, args)

    if args["run"]:
        print("Running...")
        exit_code = builder.run(args)
        if exit_code is not None and exit_code != 0:
            sys.exit(exit_code)


def main():
    os.chdir(SCRIPTPATH)

    args = parse_args()

    setup(args)

    check_environment(args)

    build_project(args)


if __name__ == "__main__":
    main()
