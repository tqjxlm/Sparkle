"""NRD gate suite: the one command CI (and pre-push discipline) runs to protect the denoiser.

Runs, in order (single build, then --skip_build throughout):
  1. converged flicker gate : nrd_static_stability_test --settle 2000 (asserts nrd == raw baseline;
     guards the convergence handoff + output stabilization).
  2. firefly gate           : nrd_static_stability_test --settle 150 captures, then
     nrd_preconv_quality --assert_fireflies 100 (healthy = ~3 spiking pixels, broken = ~750;
     guards the zero-firefly stack against knob/protocol regressions).
  3. motion noise gate      : nrd_motion_test (asserts denoised motion noise < 0.02 vs raw 0.18;
     guards reprojection, the hitT protocol, and seed independence).
  4. pitch motion gate      : nrd_motion_test --axis pitch (vertical motion exercises NRD's
     matrix-derived reprojection, which yaw cannot regress).

--allow_unsupported: if the GPU lacks hardware ray tracing (virtualized CI runners), exit 0 with
a SKIPPED marker instead of failing — the gpu pipeline cannot run there at all, so the gates are
meaningless rather than broken. On unsupported GPUs the app silently falls back to forward
rendering and still exits 0, so exit codes alone cannot tell "gates ran against NRD" from "gates
ran against a forward render that trivially passes". The probe therefore requires the app's
"effective pipeline: Gpu" log line (RenderConfig::Validate) as positive proof; if the marker is
missing for any other reason than the fallback warning, the suite fails loudly.

Run:  python3 tests/nrd/run_nrd_gates.py [--skip_build] [--allow_unsupported]
"""

import argparse
import os
import shutil
import subprocess
import sys

from nrd_common import PROJECT_ROOT, static_render_test


def run(cmd, capture=False):
    print("\n>>>", " ".join(cmd), flush=True)
    if capture:
        proc = subprocess.run(cmd, cwd=PROJECT_ROOT, capture_output=True, text=True)
        sys.stdout.write(proc.stdout[-4000:])
        sys.stderr.write(proc.stderr[-2000:])
        return proc.returncode, proc.stdout + proc.stderr
    return subprocess.run(cmd, cwd=PROJECT_ROOT).returncode, ""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", default="macos")
    parser.add_argument("--skip_build", action="store_true")
    parser.add_argument("--allow_unsupported", action="store_true")
    args = parser.parse_args()

    py = sys.executable
    build = [py, os.path.join(PROJECT_ROOT, "build.py"), "--framework", args.framework]

    # capability probe (also produces the converged reference for the firefly gate)
    probe = build + (["--skip_build"] if args.skip_build else []) + [
        "--run", "--test_case", "screenshot", "--pipeline", "gpu", "--headless", "true", "--max_spp", "512"]
    code, output = run(probe, capture=True)
    if "effective pipeline: Gpu" not in output:
        if args.allow_unsupported and "hardware ray tracing not supported" in output:
            print("SKIPPED: this GPU has no hardware ray tracing; the gpu pipeline (and NRD) cannot run here.")
            sys.exit(0)
        print("probe did not run the gpu pipeline; NRD gates would be vacuous")
        sys.exit(1)
    if code != 0:
        print("probe run failed")
        sys.exit(1)

    stability_dir = os.path.join(
        static_render_test.get_screenshot_dir(args.framework), "static_stability")
    os.makedirs(stability_dir, exist_ok=True)
    shutil.copy(static_render_test.find_screenshot(args.framework),
                os.path.join(stability_dir, "gt.png"))

    gates = [
        [py, "tests/nrd/nrd_static_stability_test.py", "--framework", args.framework, "--headless",
         "--skip_build", "--settle", "2000"],
        [py, "tests/nrd/nrd_static_stability_test.py", "--framework", args.framework, "--headless",
         "--skip_build", "--settle", "150"],
        [py, "tests/nrd/nrd_preconv_quality.py", "--framework", args.framework, "--assert_fireflies", "100"],
        [py, "tests/nrd/nrd_motion_test.py", "--framework", args.framework, "--headless", "--skip_build"],
        [py, "tests/nrd/nrd_motion_test.py", "--framework", args.framework, "--headless", "--skip_build",
         "--axis", "pitch"],
    ]
    for gate in gates:
        code, _ = run(gate)
        if code != 0:
            print(f"\nNRD GATES: FAIL ({' '.join(gate)})")
            sys.exit(1)

    print("\nNRD GATES: ALL PASS")


if __name__ == "__main__":
    main()
