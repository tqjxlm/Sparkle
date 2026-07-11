"""Check or fix source formatting for the whole repository.

Runs the same formatters CI uses, over all git-tracked files (thirdparty excluded):

* c++/objc/slang: clang-format (major version pinned below, using .clang-format)
* python: autopep8 (configured in pyproject.toml)
* markdown: markdownlint via npx (configured in .markdownlint.json)

Usage:
    python3 dev/check_format.py          # report violations, exit 1 if any
    python3 dev/check_format.py --fix    # rewrite files in place
"""

import argparse
import re
import shutil
import subprocess
import sys

# majors verified to produce identical results on this codebase
CLANG_FORMAT_SUPPORTED_MAJORS = range(18, 23)
CLANG_FORMAT_PIP_SPEC = "clang-format==22.1.5"
MARKDOWNLINT_NPX_SPEC = "markdownlint-cli@0.45.0"

# keep command lines short enough for Windows
CHUNK_SIZE = 50


def git_files(*patterns):
    output = subprocess.check_output(
        ["git", "ls-files", "--", *patterns], text=True)
    return [f for f in output.splitlines() if not f.startswith("thirdparty/")]


def run_chunked(base_cmd, files):
    """Run base_cmd over files in chunks. Returns True if all runs succeed."""
    ok = True
    for i in range(0, len(files), CHUNK_SIZE):
        result = subprocess.run(base_cmd + files[i:i + CHUNK_SIZE])
        ok = ok and result.returncode == 0
    return ok


def check_clang_format(fix):
    files = git_files("*.cpp", "*.h", "*.mm", "*.m", "*.slang", "*.slangh")

    if not shutil.which("clang-format"):
        print(f"ERROR: clang-format not found. Install with: pip install {CLANG_FORMAT_PIP_SPEC}")
        return False

    version_output = subprocess.check_output(["clang-format", "--version"], text=True)
    match = re.search(r"version (\d+)", version_output)
    if not match or int(match.group(1)) not in CLANG_FORMAT_SUPPORTED_MAJORS:
        print(f"ERROR: clang-format major version must be in "
              f"[{CLANG_FORMAT_SUPPORTED_MAJORS.start}, {CLANG_FORMAT_SUPPORTED_MAJORS.stop - 1}], "
              f"got: {version_output.strip()}")
        print(f"Install with: pip install {CLANG_FORMAT_PIP_SPEC}")
        return False

    mode = ["-i"] if fix else ["--dry-run", "--Werror"]
    return run_chunked(["clang-format"] + mode, files)


def check_autopep8(fix):
    files = git_files("*.py")

    if not shutil.which("autopep8"):
        print("ERROR: autopep8 not found. Install with: pip install autopep8")
        return False

    mode = ["--in-place"] if fix else ["--diff", "--exit-code"]
    return run_chunked(["autopep8"] + mode, files)


def check_markdownlint(fix):
    files = git_files("*.md")

    npx = shutil.which("npx")
    if not npx:
        print("ERROR: npx not found. Install Node.js to run markdownlint.")
        return False

    mode = ["--fix"] if fix else []
    return run_chunked([npx, "--yes", MARKDOWNLINT_NPX_SPEC] + mode, files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fix", action="store_true",
                        help="rewrite files in place instead of just checking")
    args = parser.parse_args()

    results = {
        "clang-format": check_clang_format(args.fix),
        "autopep8": check_autopep8(args.fix),
        "markdownlint": check_markdownlint(args.fix),
    }

    failed = [name for name, ok in results.items() if not ok]
    if failed:
        print(f"\nFormat check FAILED: {', '.join(failed)}")
        if not args.fix:
            print("Run 'python3 dev/check_format.py --fix' to fix automatically.")
        sys.exit(1)

    print("\nFormat check passed.")


if __name__ == "__main__":
    main()
