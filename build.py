import json
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
# framework's binary
COOK_FRAMEWORKS = ("glfw", "macos")

# device targets run from an installed package, so cooked content can only reach them
# through the package stage; desktop runs read the cook output from the build tree directly
DEVICE_FRAMEWORKS = ("android", "ios")

# every desktop host can cook: macos through its native framework, windows and linux
# through glfw
if platform.system() == "Darwin":
    HOST_COOK_FRAMEWORK = "macos"
else:
    HOST_COOK_FRAMEWORK = "glfw"

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


# must match CookTargetFamilies in AppFramework.cpp; glfw targets carry the host OS
COOK_TARGETS = ("android", "ios", "macos", "macos-glfw", "windows-glfw", "linux-glfw")


def cook_target(framework):
    """The cook-target identity of a framework built on this host (matches product_name)."""
    if framework == "glfw":
        return f"{host_system_name()}-glfw"
    return framework


def cooker_framework(framework):
    """The framework whose binary executes the cook: itself when host-runnable,
    otherwise the host framework (the artifact pool is shared across targets)."""
    return framework if framework in COOK_FRAMEWORKS else HOST_COOK_FRAMEWORK


def default_cooked_image_dir(framework):
    """Where the package stage finds the framework's cooked content image by default,
    anchored to the repo root because builders may chdir during build."""
    return os.path.join(SCRIPTPATH, COOKED_IMAGE_DIR[cooker_framework(framework)], cook_target(framework))


def resolve_image_member(image_root, relative, description):
    """Resolves a manifest path only when it is relative and contained in the image."""
    drive_qualified = (len(relative) >= 2 and relative[0].isalpha() and
                       relative[1] == ":")
    if (not relative or os.path.isabs(relative) or
            relative.startswith(("/", "\\")) or drive_qualified):
        raise RuntimeError(f"{description} escapes the content image: {relative}")

    resolved = os.path.realpath(os.path.join(image_root, relative))
    try:
        contained = os.path.commonpath([resolved, image_root]) == image_root
    except ValueError:
        contained = False
    if not contained:
        raise RuntimeError(f"{description} escapes the content image: {relative}")
    return resolved


def load_json(path, description):
    if not os.path.isfile(path):
        raise RuntimeError(f"{description} missing at {path}; run the cook stage first")
    with open(path, encoding="utf-8") as json_file:
        return json.load(json_file)


def project_artifacts(pool_dir, target_products, image_dir):
    """Copies the target plan's artifact payloads out of the pool and returns the
    projected manifest. Every artifact must verify: a cooked package never ships a
    plan entry it cannot back."""
    pool_manifest = load_json(os.path.join(pool_dir, "manifest.json"), "cook manifest")
    pool_root = os.path.realpath(os.path.dirname(pool_dir))

    image_manifest = {}
    for key in target_products["artifacts"]:
        entry = pool_manifest.get(key)
        artifact = entry.get("artifact", "") if isinstance(entry, dict) else ""
        if not artifact:
            raise RuntimeError(f"artifact {key} missing from the cook manifest; re-run the cook stage")
        payload = resolve_image_member(pool_root, artifact, f"artifact {key}")
        if not os.path.isfile(payload):
            raise RuntimeError(f"artifact payload missing for {key}: {artifact}")

        destination = os.path.join(image_dir, artifact)
        os.makedirs(os.path.dirname(destination), exist_ok=True)
        shutil.copy(payload, destination)
        image_manifest[key] = entry
    return image_manifest


def assemble_cooked_image(cooker, target):
    """The cook stage's product for one target: a content image projected from the
    artifact pool by the target's plan (cook_products.json). Every packed asset ships
    as itself unless the plan replaces it with verified artifacts; a source whose
    artifacts fail verification ships as the runtime fallback."""
    pool_dir = os.path.join(SCRIPTPATH, COOKED_OUTPUT_DIR[cooker])
    products = load_json(os.path.join(pool_dir, "cook_products.json"), "cook products manifest")
    if target not in products:
        raise RuntimeError(
            f"cook output has no target '{target}' (has: {', '.join(sorted(products))});"
            f" re-run the cook stage with --cook_targets {target}")

    image_dir = os.path.join(SCRIPTPATH, COOKED_IMAGE_DIR[cooker], target)
    shutil.rmtree(image_dir, ignore_errors=True)

    image_manifest = project_artifacts(pool_dir, products[target], image_dir)

    consumed_sources = products[target]["consumed_sources"]
    packed_dir = os.path.join(SCRIPTPATH, "resources", "packed")
    if not os.path.isdir(packed_dir):
        raise RuntimeError(f"packed resources missing at {packed_dir}; run resources setup first")
    replaced = 0
    for root, _, names in os.walk(packed_dir):
        for name in names:
            source = os.path.join(root, name)
            relative = os.path.relpath(source, packed_dir).replace(os.sep, "/")
            replacing_keys = consumed_sources.get(relative)
            if replacing_keys and all(key in image_manifest for key in replacing_keys):
                replaced += 1
                continue
            if replacing_keys:
                print(f"keeping consumed source {relative}: its artifacts failed verification")
            destination = os.path.join(image_dir, relative)
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            shutil.copy(source, destination)

    # written last so an interrupted assembly never passes downstream manifest checks
    cooked_dir = os.path.join(image_dir, "cooked")
    os.makedirs(cooked_dir, exist_ok=True)
    with open(os.path.join(cooked_dir, "content_target.json"), "w", encoding="utf-8") as target_file:
        json.dump({"target": target}, target_file, indent=2)
    with open(os.path.join(cooked_dir, "manifest.json"), "w", encoding="utf-8") as manifest_file:
        json.dump(image_manifest, manifest_file, indent=2)

    print(f"Assembled cooked content image for {target} at {image_dir}:"
          f" {len(image_manifest)} artifacts, {replaced} sources replaced")


def resolve_stages(stage_args, framework):
    """Stage selection: explicit --stage wins; the default is the full pipeline.
    Explicit selections (including 'all') never degrade: a missing input fails
    fast in validate_stages."""
    if stage_args:
        selected = set()
        for stage in stage_args:
            selected |= set(STAGES) if stage == "all" else {stage}
    else:
        selected = set(STAGES)
    return [stage for stage in STAGES if stage in selected]


def validate_stages(stages, args):
    """Stages never run an upstream stage implicitly: missing inputs fail fast."""
    framework = args["framework"]
    if "cook" in stages:
        cooker = cooker_framework(framework)
        binary_exists = os.path.exists(os.path.join(SCRIPTPATH, BINARY_PATH[cooker]))
        # building a cross-compiled framework never produces the cooker binary
        if not binary_exists and (cooker != framework or "build" not in stages):
            raise RuntimeError(
                f"cooking for '{framework}' runs the {cooker} binary, which is missing at"
                f" {BINARY_PATH[cooker]}; build it first: python3 build.py --framework {cooker}")

    # uncooked packages are disallowed by design even though the runtime could cook
    if "package" in stages and "cook" not in stages:
        image_dir = args["cooked"] or default_cooked_image_dir(framework)
        if not os.path.isfile(os.path.join(image_dir, "cooked", "manifest.json")):
            raise RuntimeError(
                f"the package stage needs a cooked content image; none at {image_dir}."
                f" run: python3 build.py --framework {framework} --stage cook")


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
                        " repeat to select multiple. Default: every stage the host supports."
                        " Cross-compiled frameworks cook through the host framework's binary")
    parser.add_argument("--cooked",
                        help="Cooked content image directory for the package stage"
                        " (default: the built framework's own image from the host cook)")
    parser.add_argument("--cook_targets",
                        help="'+'-separated cook targets the cook stage produces content"
                        f" images for (default: the built framework's own target);"
                        f" known targets: {', '.join(COOK_TARGETS)}")
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
    parser.add_argument("--strip_test", action="store_true",
                        help="Disable test case support (enabled by default)")
    parser.add_argument("--clean", action="store_true",
                        help="Clean output directory before configure")
    parser.add_argument("--apple_auto_sign", action="store_true",
                        help="Enable automatic code signing for Apple platforms. Requires APPLE_DEVELOPER_TEAM_ID to be set."
                        "See https://developer.apple.com/help/account/manage-your-team/locate-your-team-id/")
    parser.add_argument("--cmake-args",
                        help="Additional CMake arguments (e.g., --cmake-args='-DCMAKE_EXE_LINKER_FLAGS=\"-lc++abi\"')")
    parser.add_argument("--android_abi", choices=["arm64-v8a", "x86_64"],
                        help="Android target ABI (default arm64-v8a, the shipping ABI);"
                        " x86_64 targets emulators on x86 hosts")
    parser.add_argument("--ios_platform", choices=["device", "simulator"],
                        help="iOS target platform (default device, the shipping target);"
                        " simulator targets the host's iOS Simulator and builds unsigned")

    # Unknown args pass through to the app (cook stage runs and run.py launches)
    parsed_args, unknown_args = parser.parse_known_args(args)

    cook_targets = None
    if parsed_args.cook_targets:
        cook_targets = [target for target in parsed_args.cook_targets.split("+") if target]
        unknown_targets = sorted(set(cook_targets) - set(COOK_TARGETS))
        if unknown_targets:
            parser.error(f"unknown cook target(s) {', '.join(unknown_targets)};"
                         f" known targets: {', '.join(COOK_TARGETS)}")
        if not cook_targets:
            parser.error("no cook target requested")

    return {
        "framework": parsed_args.framework,
        "config": parsed_args.config,
        "stages": resolve_stages(parsed_args.stage, parsed_args.framework),
        "cooked": parsed_args.cooked,
        "cook_targets": cook_targets,
        "cmake_options": construct_additional_cmake_options(parsed_args, parsed_args.cmake_args),
        "unknown_args": unknown_args,
        "android_abi": parsed_args.android_abi,
        "ios_platform": parsed_args.ios_platform,
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


def configure_git_hooks():
    """The committed hooks guard generated files (e.g. ci.yml) at commit time."""
    if os.path.exists(os.path.join(SCRIPTPATH, ".git")):
        subprocess.run(["git", "config", "core.hooksPath", ".githooks"],
                       cwd=SCRIPTPATH, check=False)


def setup(args):
    """Stage-scoped setup: each step exists to feed a specific stage, so invocations
    that skip the stage skip the step (e.g. CI cook and test nodes, which never
    compile, skip the recursive submodule clone)."""
    configure_git_hooks()

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


def host_system_name():
    system_name = {"Windows": "windows", "Linux": "linux", "Darwin": "macos"}.get(platform.system())
    if system_name is None:
        raise RuntimeError(f"Unsupported platform: {platform.system()}")
    return system_name


def product_name(framework, config, extension):
    """Products are named by target. glfw is the one framework whose target is the host's
    own desktop OS, so only there the host system is part of the target identity."""
    if framework == "glfw":
        return f"{host_system_name()}-glfw-{config}{extension}"
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


def archive_product(builder, args):
    """The raw build product: the built app archived and named, with no cooked
    content. CI build nodes upload it through dev/archive_product.py; the package
    stage turns it into a shippable package."""
    print("Archiving...")
    archive_path = builder.archive(args)
    return copy_build_products(archive_path, args)


def package_project(builder, args):
    sys.path.insert(0, os.path.join(SCRIPTPATH, "dev"))
    from package_cooked import PackagingError, package_cooked

    product_path = archive_product(builder, args)

    image_dir = args["cooked"] or default_cooked_image_dir(args["framework"])
    try:
        package_cooked(product_path, args["framework"], image_dir,
                       target=cook_target(args["framework"]))
    except PackagingError as error:
        raise RuntimeError(f"package stage failed: {error}") from error

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
    validate_stages(stages, args)

    if "build" in stages:
        print("Building...")
        builder.build(args)
    else:
        print("Skipping build.")

    if "cook" in stages:
        cooker = cooker_framework(args["framework"])
        targets = args["cook_targets"] or [cook_target(args["framework"])]
        print(f"Cooking with the {cooker} binary for: {'+'.join(targets)}")
        cook_args = dict(args)
        cook_args["framework"] = cooker
        cook_args["unknown_args"] = ["--cook", "true", "--cook_targets", "+".join(targets),
                                     "--log_path", "logs/cook.log"] + args["unknown_args"]
        cook_builder = builder if cooker == args["framework"] else create_builder(cooker)
        exit_code = cook_builder.run(cook_args)
        if exit_code is not None and exit_code != 0:
            sys.exit(exit_code)
        for target in targets:
            assemble_cooked_image(cooker, target)

    if "package" in stages:
        package_project(builder, args)


def main():
    os.chdir(SCRIPTPATH)

    args = parse_args()

    setup(args)

    check_environment(args)

    build_project(args)


if __name__ == "__main__":
    main()
