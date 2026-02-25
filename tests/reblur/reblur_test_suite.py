"""REBLUR test suite: runs all available REBLUR tests and reports results.

Tests included:
  1. Smoke test — app launches without crash
  2. Vanilla functional test — GPU pipeline without REBLUR matches ground truth (FLIP <= 0.1)
  3. Split-merge equivalence — split shader with debug_pass 255 matches ground truth (FLIP <= 0.1)
  4. REBLUR screenshot — split path + full denoiser pipeline runs without crash
  5. REBLUR per-pass validation — spatial passes produce valid output with decreasing variance
  6. REBLUR pass validation (C++) — native test case exercises full spatial pipeline
  7. REBLUR temporal validation — TemporalAccum/HistoryFix produce valid output, convergence
  8. REBLUR temporal convergence (C++) — 30+ frames temporal pipeline without crash

Usage:
  python tests/reblur/reblur_test_suite.py --framework glfw [--skip_build]
"""

import argparse
import os
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))


def parse_args():
    parser = argparse.ArgumentParser(description="REBLUR test suite")
    parser.add_argument("--framework", default="glfw", choices=("glfw", "macos"))
    parser.add_argument("--skip_build", action="store_true",
                        help="Skip the initial build (assumes already built)")
    return parser.parse_args()


def run_command(cmd, label, cwd=PROJECT_ROOT):
    """Run a command and return (success: bool, duration_seconds: float, output: str)."""
    print(f"\n{'='*70}", flush=True)
    print(f"  {label}", flush=True)
    print(f"{'='*70}", flush=True)
    print(f"  cmd: {' '.join(cmd)}", flush=True)
    print(flush=True)

    start = time.time()
    result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    duration = time.time() - start

    # Show output (truncated for very long output)
    combined = result.stdout + result.stderr
    if len(combined) > 4000:
        # Show last 4000 chars for long output
        print(f"  ... (truncated, showing last 4000 chars) ...", flush=True)
        print(combined[-4000:], flush=True)
    else:
        print(combined, flush=True)

    success = result.returncode == 0
    status = "PASS" if success else "FAIL"
    print(f"  [{status}] {label} ({duration:.1f}s)", flush=True)
    return success, duration, combined


def main():
    args = parse_args()
    fw = args.framework
    py = sys.executable
    build_py = os.path.join(PROJECT_ROOT, "build.py")
    functional_test_py = os.path.join(PROJECT_ROOT, "dev", "functional_test.py")
    pass_validation_py = os.path.join(SCRIPT_DIR, "reblur_pass_validation.py")
    temporal_validation_py = os.path.join(SCRIPT_DIR, "reblur_temporal_validation.py")

    results = []

    # --- Build ---
    if not args.skip_build:
        ok, dur, _ = run_command(
            [py, build_py, "--framework", fw],
            "Build")
        if not ok:
            print("\nBuild failed — aborting test suite.", flush=True)
            return 1
        results.append(("Build", ok, dur))
    else:
        print("\nSkipping build (--skip_build)", flush=True)

    # All subsequent commands use --skip_build since we built once above.

    # --- Test 1: Smoke test ---
    ok, dur, _ = run_command(
        [py, build_py, "--framework", fw, "--skip_build",
         "--run", "--test_case", "smoke", "--headless", "true"],
        "1. Smoke test")
    results.append(("Smoke test", ok, dur))

    # --- Test 2: Vanilla functional test (no REBLUR) ---
    ok, dur, _ = run_command(
        [py, functional_test_py,
         "--framework", fw, "--pipeline", "gpu", "--headless", "--skip_build",
         "--", "--use_reblur", "false", "--spp", "1", "--max_spp", "2048"],
        "2. Vanilla functional test (FLIP vs ground truth)")
    results.append(("Vanilla functional test", ok, dur))

    # --- Test 3: Split-merge equivalence (debug_pass 255) ---
    ok, dur, _ = run_command(
        [py, functional_test_py,
         "--framework", fw, "--pipeline", "gpu", "--headless", "--skip_build",
         "--", "--use_reblur", "true", "--reblur_debug_pass", "255",
         "--spp", "1", "--max_spp", "2048"],
        "3. Split-merge equivalence (debug_pass 255, FLIP vs ground truth)")
    results.append(("Split-merge equivalence", ok, dur))

    # --- Test 4: REBLUR screenshot (full pipeline, no crash) ---
    ok, dur, _ = run_command(
        [py, build_py, "--framework", fw, "--skip_build",
         "--run", "--test_case", "screenshot", "--headless", "true",
         "--pipeline", "gpu", "--use_reblur", "true",
         "--spp", "1", "--max_spp", "64"],
        "4. REBLUR screenshot (full denoiser pipeline, crash test)")
    results.append(("REBLUR screenshot", ok, dur))

    # --- Test 5: Per-pass validation (Python) ---
    ok, dur, _ = run_command(
        [py, pass_validation_py, "--framework", fw, "--skip_build"],
        "5. REBLUR per-pass validation (PrePass/Blur/PostBlur)")
    results.append(("Per-pass validation", ok, dur))

    # --- Test 6: REBLUR pass validation (C++ test case) ---
    ok, dur, _ = run_command(
        [py, build_py, "--framework", fw, "--skip_build",
         "--run", "--test_case", "reblur_pass_validation", "--headless", "true",
         "--pipeline", "gpu", "--use_reblur", "true",
         "--spp", "1", "--max_spp", "4", "--test_timeout", "60"],
        "6. REBLUR pass validation (C++ test case)")
    results.append(("C++ pass validation", ok, dur))

    # --- Test 7: Temporal validation (Python) ---
    ok, dur, _ = run_command(
        [py, temporal_validation_py, "--framework", fw, "--skip_build"],
        "7. REBLUR temporal validation (TemporalAccum/HistoryFix/convergence)")
    results.append(("Temporal validation", ok, dur))

    # --- Test 8: Temporal convergence (C++ test case) ---
    ok, dur, _ = run_command(
        [py, build_py, "--framework", fw, "--skip_build",
         "--run", "--test_case", "reblur_temporal_convergence", "--headless", "true",
         "--pipeline", "gpu", "--use_reblur", "true",
         "--spp", "1", "--max_spp", "64", "--test_timeout", "120"],
        "8. REBLUR temporal convergence (C++ test, 30+ frames)")
    results.append(("C++ temporal convergence", ok, dur))

    # --- Summary ---
    total_duration = sum(dur for _, _, dur in results)
    passed = sum(1 for _, ok, _ in results if ok)
    failed = sum(1 for _, ok, _ in results if not ok)

    print(f"\n{'='*70}", flush=True)
    print(f"  REBLUR TEST SUITE RESULTS", flush=True)
    print(f"{'='*70}", flush=True)
    for name, ok, dur in results:
        status = "PASS" if ok else "FAIL"
        print(f"  [{status}]  {name:<40s}  ({dur:.1f}s)", flush=True)
    print(f"{'='*70}", flush=True)
    print(f"  {passed} passed, {failed} failed  ({total_duration:.1f}s total)", flush=True)
    print(f"{'='*70}", flush=True)

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
