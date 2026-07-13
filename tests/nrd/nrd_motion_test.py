"""NRD motion gate: continuous-rotation noise must stay near the denoised floor.

Drives the frame-precise denoiser_sweep capture (2 deg/frame) with NRD on and asserts the
mean 3x3 local-luminance std (spatial noise) stays below the gate. Reference points: healthy NRD
~0.011, raw 1-spp input ~0.18 — the gate (0.02) catches any regression that lets input noise
through (broken reprojection, dead history, seed correlation) with ~2x headroom over healthy.

--axis pitch runs the vertical arm: it exercises NRD's matrix-derived reprojection (disocclusion
plane tests, specular virtual motion), which yaw leaves untouched — a mirrored projection matrix
passes the yaw arm while failing pitch. The pitch gate is tighter because that failure mode is
subtler: calibration 2026-07-07, healthy 0.0086 vs mirrored-matrix 0.0140 (specular ghosting +
raw-noise breakthrough), deterministic seeds make the measurement reproducible.

Run:  python3 tests/nrd/nrd_motion_test.py [--framework macos] [--skip_build] [--headless] [--axis yaw|pitch]
"""

import argparse
import sys

from nrd_common import NUM_FRAMES, load_sweep_frames, lum, run_sweep, static_render_test

NOISE_GATES = {"yaw": 0.02, "pitch": 0.012}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", default="macos",
                        choices=static_render_test.SUPPORTED_FRAMEWORKS)
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--skip_build", action="store_true")
    parser.add_argument("--axis", default="yaw", choices=["yaw", "pitch"])
    args, passthrough = parser.parse_known_args()

    if args.axis == "yaw":
        motion_flags = ["--sweep_step_degrees", "2.0"]
    else:
        motion_flags = ["--sweep_step_degrees", "0.0", "--sweep_pitch_step_degrees", "2.0"]

    run_sweep(args.framework, ["--max_spp", "1", "--nrd", "true"] + motion_flags + list(passthrough),
              skip_build=args.skip_build, headless=args.headless)

    import numpy as np
    from numpy.lib.stride_tricks import sliding_window_view

    frames = load_sweep_frames(
        static_render_test.get_screenshot_dir(args.framework))
    lums = lum(frames)
    noise = float(np.mean([sliding_window_view(np.pad(f, 1, mode="edge"), (3, 3)).std(axis=(2, 3)).mean()
                           for f in lums]))
    flicker = float(np.abs(frames[1:] - frames[:-1]).mean())

    # under continuous motion every capture must differ; identical frames mean the harness filmed a
    # frozen/stale image and every statistic below is vacuous
    stuck = [k for k in range(1, NUM_FRAMES) if np.array_equal(frames[k], frames[k - 1])]
    if stuck:
        print(f"motion capture broken: frames identical to their predecessor at {stuck} -> FAIL")
        sys.exit(1)

    gate = NOISE_GATES[args.axis]
    ok = noise < gate
    print(f"nrd motion ({args.axis}): noise={noise:.4f} flicker={flicker:.4f}")
    print(f"motion noise gate ({args.axis}): {noise:.4f} < {gate} -> {'PASS' if ok else 'FAIL'}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
