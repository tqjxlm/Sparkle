"""Compute the CI matrices from one product table.

The pipeline (.github/workflows/build.yml) runs three matrix jobs over the same
product set: build, release and test. GitHub cannot share a matrix definition
between jobs, so this script is the single source of truth: the plan job runs it
once and downstream jobs consume the JSON through fromJSON().

Prints one GITHUB_OUTPUT line per matrix: build=..., release=..., test=...
"""

import argparse
import json

# every product CI ships: the target framework and the runner that builds it
PRODUCTS = (
    {"os": "macos-latest", "framework": "macos"},
    {"os": "macos-latest", "framework": "ios"},
    {"os": "macos-latest", "framework": "glfw"},
    {"os": "windows-latest", "framework": "glfw"},
    {"os": "ubuntu-latest", "framework": "android"},
)

# built by the standalone job the cook stage's needs edge targets, never by the matrix
STANDALONE_BUILDS = (
    {"os": "macos-latest", "framework": "macos", "build_type": "Release"},
)

# the test table: which released products run the aggregate suite, and with what
# coverage. a product absent from the table ships untested (no runner can drive it yet)
TESTS = (
    # no GPU on hosted windows runners: software vulkan via lavapipe, which can
    # take the suite close to an hour (see TODO.md), hence the generous timeout
    {
        "os": "windows-latest",
        "framework": "glfw",
        "build_type": "Release",
        "suite_args": "--software --headless --require_cooked",
        "suite_timeout": 120,
        "screenshots": "build_system/glfw/output/build/generated/screenshots/",
    },
    # physical Metal GPU (the runner class the cook stage relies on). its
    # virtualized device exposes no ray tracing, so the gpu path-tracing pipeline
    # (and NRD) silently falls back to forward and cannot be tested here
    {
        "os": "macos-latest",
        "framework": "macos",
        "build_type": "Release",
        "suite_args": "--headless --require_cooked",
        "suite_timeout": 60,
        "screenshots": "~/Documents/sparkle/screenshots/",
    },
)


def matrices(build_types):
    """The three include lists. Cell keys stay ordered os, framework, build_type:
    GitHub renders job display names from them, and wait-for-job awaits cells by
    those names."""
    release = [dict(product, build_type=build_type)
               for product in PRODUCTS for build_type in build_types]
    build = [cell for cell in release if cell not in STANDALONE_BUILDS]
    test = [dict(row) for row in TESTS if row["build_type"] in build_types]
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
