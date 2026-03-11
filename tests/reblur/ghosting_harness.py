"""Shared harness helpers for the deterministic reblur_ghosting test case."""

import subprocess

# reblur_ghosting should exit explicitly once all captures are ready. Running
# for minutes without returning means the app or harness is stuck.
GHOSTING_WALLCLOCK_TIMEOUT_SECONDS = 180


def run_ghosting_app(py, build_py, project_root, framework, extra_args, label,
                     clear_screenshots=False):
    cmd = [py, build_py, "--framework", framework, "--skip_build",
           "--run", "--test_case", "reblur_ghosting", "--headless", "true"]
    if clear_screenshots:
        cmd += ["--clear_screenshots", "true"]
    cmd += extra_args
    print(f"  cmd: {' '.join(cmd)}")

    try:
        result = subprocess.run(
            cmd,
            cwd=project_root,
            capture_output=True,
            text=True,
            timeout=GHOSTING_WALLCLOCK_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired:
        print(
            f"  FAIL: {label} exceeded the {GHOSTING_WALLCLOCK_TIMEOUT_SECONDS}s wall-clock guard")
        print("    reblur_ghosting should exit explicitly once all captures are ready")
        return False

    combined_output = "\n".join(
        chunk for chunk in (result.stdout, result.stderr) if chunk)
    hit_frame_guard = "timed out after" in combined_output

    if result.returncode != 0:
        if hit_frame_guard:
            print(f"  FAIL: {label} hit the inferred frame-timeout guard")
            print("    reblur_ghosting should have passed explicitly before the guard")
        else:
            print(f"  FAIL: {label} exited with code {result.returncode}")

        for line in combined_output.strip().splitlines()[-8:]:
            print(f"    {line}")
        return False

    if hit_frame_guard:
        print(
            f"  FAIL: {label} reported a frame-timeout despite a zero exit code")
        return False

    return True
