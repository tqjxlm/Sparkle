"""Compute the CI matrices from one product table.

The pipeline (.github/workflows/build.yml) runs three matrix jobs over the same
product set: build, release and test. GitHub cannot share a matrix definition
between jobs, so this script is the single source of truth: the plan job runs it
once and downstream jobs consume the JSON through fromJSON().

Prints one GITHUB_OUTPUT line per matrix: build=..., release=..., test=...
"""

import argparse
import csv
import json
import os

# every product CI ships: the target framework and the runner that builds it.
# the x86_64 android product exists only to feed the emulator test cell (hosted
# runners cannot virtualize arm64 guests), so it only builds the Release the
# cell tests; the shipping apk stays arm64. build_types stays out of the matrix
# cells: their key set renders the job display names the wait-for-job edges match
PRODUCTS = (
    {"os": "macos-latest", "framework": "macos"},
    {"os": "macos-latest", "framework": "ios"},
    {"os": "macos-latest", "framework": "glfw"},
    {"os": "windows-latest", "framework": "glfw"},
    {"os": "ubuntu-latest", "framework": "android"},
    {"os": "ubuntu-latest", "framework": "android", "abi": "x86_64",
     "build_types": ("Release",)},
)

# built by the standalone job the cook stage's needs edge targets, never by the matrix
STANDALONE_BUILDS = (
    {"os": "macos-latest", "framework": "macos", "build_type": "Release"},
)

# must hold every tests/coverage.csv triplet column: that table decides who runs the suite
TEST_RUNNERS = {
    # no GPU on hosted windows runners: software vulkan via lavapipe, which can
    # take the suite close to an hour (see TODO.md), hence the generous timeout
    "windows-glfw-release": {
        "suite_args": "--software",
        "suite_timeout": 120,
        "screenshots": "build_system/glfw/output/build/generated/screenshots/",
    },
    # physical Metal GPU (the runner class the cook stage relies on). its
    # virtualized device exposes no ray tracing, so the gpu path-tracing pipeline
    # (and NRD) silently falls back to forward and cannot be tested here
    "macos-macos-release": {
        "suite_args": "",
        "suite_timeout": 60,
        "screenshots": "~/Documents/sparkle/screenshots/",
    },
    # the Vulkan backend on a real GPU (via MoltenVK), which windows-glfw only
    # exercises through software rasterization
    "macos-glfw-release": {
        "suite_args": "",
        "suite_timeout": 60,
        "screenshots": "build_system/glfw/output/build/generated/screenshots/",
    },
    # the x86_64 android package on a KVM-accelerated headless emulator, whose
    # guest Vulkan device is llvmpipe (the windows cell's driver class): no ray
    # tracing, so the gpu pipeline stays local-only. abi keys the artifact and
    # job names apart from the shipping arm64 product, which no hosted runner
    # can emulate. 1560x720 matches the published android ground-truth captures
    "ubuntu-android-release": {
        "abi": "x86_64",
        "suite_args": "--width 1560 --height 720",
        "suite_timeout": 90,
        "screenshots": "build_system/android/output/device/screenshots/",
    },
}


def covered_triplets():
    """The triplet columns of the coverage table (its rows pick the cases)."""
    coverage_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                 "tests", "coverage.csv")
    with open(coverage_path, newline="") as coverage_file:
        header = next(csv.reader(coverage_file))
    return [column for column in header if column != "case"]


def test_cell(triplet):
    host, framework, config = triplet.split("-")
    return dict({"os": f"{host}-latest", "framework": framework,
                 "build_type": config.capitalize()}, **TEST_RUNNERS[triplet])


def product_cell(product, build_type):
    cell = {"os": product["os"], "framework": product["framework"],
            "build_type": build_type}
    cell.update({key: value for key, value in product.items()
                 if key not in cell and key != "build_types"})
    return cell


def matrices(build_types):
    """The three include lists. Cell keys stay ordered os, framework, build_type:
    GitHub renders job display names from them (extras such as abi trail), and
    wait-for-job awaits cells by those names."""
    release = [product_cell(product, build_type)
               for product in PRODUCTS for build_type in build_types
               if build_type in product.get("build_types", build_types)]
    build = [cell for cell in release if cell not in STANDALONE_BUILDS]
    test = [cell for cell in map(test_cell, covered_triplets())
            if cell["build_type"] in build_types]
    return {"build": build, "release": release, "test": test}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build_types", required=True,
                        help="the workflow's build_type input: a JSON list body"
                        " such as '\"Debug\",\"Release\"'")
    args = parser.parse_args()

    for name, matrix in matrices(json.loads(f"[{args.build_types}]")).items():
        print(f"{name}={json.dumps(matrix)}")


if __name__ == "__main__":
    main()
