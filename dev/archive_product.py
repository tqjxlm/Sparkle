"""Archive the built app as a raw build product.

The raw product carries the build's own embedded assets and no cooked content;
it is the artifact CI build nodes upload for the cook and release stages to
consume. It is not a shippable package — those only come from the package
stage of build.py or a CI release node, which swap in a cook target's content
image (see dev/package_cooked.py).
"""
import argparse
import os
import sys

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, PROJECT_ROOT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", required=True,
                        choices=["glfw", "macos", "ios", "android"])
    parser.add_argument("--config", default="Release", choices=["Release", "Debug"])
    parser.add_argument("--android_abi", choices=["arm64-v8a", "x86_64"])
    parser.add_argument("--ios_platform", choices=["device", "simulator"])
    parsed = parser.parse_args()

    argv = ["--framework", parsed.framework, "--config", parsed.config]
    if parsed.android_abi:
        argv += ["--android_abi", parsed.android_abi]
    if parsed.ios_platform:
        argv += ["--ios_platform", parsed.ios_platform]

    import build
    from build_system.builder_factory import create_builder

    os.chdir(build.SCRIPTPATH)
    args = build.parse_args(argv)
    build.archive_product(create_builder(args["framework"]), args)


if __name__ == "__main__":
    main()
