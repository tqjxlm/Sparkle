"""Run clang-tidy (using .clang-tidy) over all first-party sources in a compile database.

The compile database comes from a CMake configure, e.g.:

    python3 build.py --framework glfw --clangd

Usage:
    python3 dev/check_tidy.py                 # report violations, exit 1 if any
    python3 dev/check_tidy.py --build-dir DIR # use another compile database
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

# unlike clang-format, majors are not interchangeable: each adds/changes checks
CLANG_TIDY_MAJOR = 22
CLANG_TIDY_PIP_SPEC = "clang-tidy==22.1.7"

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD_DIR = os.path.join(REPO_ROOT, "build_system", "glfw", "output")

FIRST_PARTY_DIRS = ("libraries", "frameworks", "tests")


def find_clang_tidy():
    # prefer the pip package over PATH, which may hold another LLVM's clang-tidy
    try:
        import clang_tidy
        data_bin = os.path.join(os.path.dirname(clang_tidy.__file__), "data", "bin")
        exe = shutil.which("clang-tidy", path=data_bin)
    except ImportError:
        exe = None

    if not exe:
        exe = shutil.which("clang-tidy")
    if not exe:
        print(f"ERROR: clang-tidy not found. Install with: pip install {CLANG_TIDY_PIP_SPEC}")
        return None

    version_output = subprocess.check_output([exe, "--version"], text=True)
    match = re.search(r"version (\d+)", version_output)
    if not match or int(match.group(1)) != CLANG_TIDY_MAJOR:
        print(f"ERROR: clang-tidy major version must be {CLANG_TIDY_MAJOR}, "
              f"got: {version_output.strip()}")
        print(f"Install with: pip install {CLANG_TIDY_PIP_SPEC}")
        return None

    return exe


def first_party_files(build_dir):
    db_path = os.path.join(build_dir, "compile_commands.json")
    if not os.path.exists(db_path):
        print(f"ERROR: {db_path} not found. "
              "Generate it with: python3 build.py --framework glfw --clangd")
        return None

    with open(db_path) as db_file:
        entries = json.load(db_file)

    files = set()
    for entry in entries:
        rel = os.path.relpath(entry["file"], REPO_ROOT)
        if rel.startswith(FIRST_PARTY_DIRS):
            files.add(entry["file"])
    return sorted(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default=DEFAULT_BUILD_DIR,
                        help="build directory containing compile_commands.json")
    args = parser.parse_args()

    clang_tidy = find_clang_tidy()
    if not clang_tidy:
        sys.exit(1)

    files = first_party_files(args.build_dir)
    if files is None:
        sys.exit(1)

    def check(file):
        result = subprocess.run([clang_tidy, "-p", args.build_dir, "--quiet", file],
                                capture_output=True, text=True)
        return file, result.returncode, result.stdout

    failures = 0
    with ThreadPoolExecutor(max_workers=os.cpu_count()) as executor:
        for file, returncode, output in executor.map(check, files):
            rel = os.path.relpath(file, REPO_ROOT)
            if returncode == 0:
                print(f"ok {rel}")
            else:
                failures += 1
                print(f"FAILED {rel}")
                print(output)

    if failures:
        print(f"\nclang-tidy check FAILED: {failures}/{len(files)} files")
        sys.exit(1)

    print(f"\nclang-tidy check passed ({len(files)} files).")


if __name__ == "__main__":
    main()
