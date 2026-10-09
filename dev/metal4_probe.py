"""Build and run dev/metal4_probe.mm on the host Mac and inside the iOS Simulator.

Each run prints the Metal device, its GPU families and whether a Metal 4 workload
passes. Exits 1 if either run fails.

Usage:
    python3 dev/metal4_probe.py
"""

import os
import platform
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
SOURCE = os.path.join(SCRIPT_DIR, "metal4_probe.mm")
OUTPUT_DIR = os.path.join(PROJECT_ROOT, "build_cache", "metal4_probe")

sys.path.insert(0, PROJECT_ROOT)
from build_system.ios.build import ensure_simulator  # noqa: E402


def build(sdk, target):
    binary = os.path.join(OUTPUT_DIR, sdk)
    subprocess.run(["xcrun", "-sdk", sdk, "clang++", "-std=c++20", "-fobjc-arc", "-target", target,
                    "-framework", "Foundation", "-framework", "Metal", SOURCE, "-o", binary], check=True)
    return binary


def run(name, command):
    print(f"\n=== {name}", flush=True)
    return subprocess.run(command).returncode == 0


def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    subprocess.run(["xcodebuild", "-version"], check=True)

    arch = "arm64" if platform.machine().lower() in ("arm64", "aarch64") else "x86_64"
    macos = build("macosx", f"{arch}-apple-macos14.2")
    simulator = build("iphonesimulator", f"{arch}-apple-ios18.0-simulator")

    passed = run("macOS", [macos])
    passed &= run("iOS Simulator", ["xcrun", "simctl", "spawn", ensure_simulator(), simulator])
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
