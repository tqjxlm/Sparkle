"""Re-cook the NRD ReBLUR shaders after an NRD submodule update.

Builds the macOS app and runs --test_case nrd_cook, which cross-compiles every ReBLUR pipeline
SPIR-V -> MSL (macOS + iOS variants) and rewrites shaders/nrd/cooked/ (committed). NrdDenoiser
refuses to run with a stale cook, so commit the regenerated files together with the NRD bump.

Run:  python3 dev/cook_nrd_shaders.py [--skip_build]
"""

import argparse
import os
import subprocess
import sys

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip_build", action="store_true")
    args = parser.parse_args()

    cmd = [sys.executable, os.path.join(PROJECT_ROOT, "build.py"), "--framework", "macos"]
    if args.skip_build:
        cmd.append("--skip_build")
    cmd += ["--run", "--test_case", "nrd_cook", "--headless", "true",
            "--nrd_cook_output", os.path.join(PROJECT_ROOT, "shaders", "nrd", "cooked")]
    print("Running:", " ".join(cmd), flush=True)
    sys.exit(subprocess.run(cmd, cwd=PROJECT_ROOT).returncode)


if __name__ == "__main__":
    main()
