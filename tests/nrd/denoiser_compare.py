"""Side-by-side denoiser backend comparison: quality (static + motion) and per-frame cost.

Rows: raw (no denoiser) plus each backend from --denoisers. Columns:

  * static noise/flicker : 3x3 local-luminance std and consecutive-frame delta over 16 static frames
    captured 16 accumulated samples in (early transient, where a denoiser matters most).
  * static FLIP          : frame 8 vs a converged reference rendered locally with the denoiser off.
  * yaw/pitch noise      : the motion-gate statistic at max_spp 1 (continuous 2 deg/frame rotation).
  * frame cost           : wall-clock delta vs the raw arm over --perf_frames identical static frames.

Run:  python3 tests/nrd/denoiser_compare.py [--denoisers nrd,metalfx] [--skip_build] [--headless]
      [--perf_frames 2000] [--render_scale 1.0]
"""

import argparse
import os
import shutil
import time

from nrd_common import (
    NUM_FRAMES,
    load_image,
    load_sweep_frames,
    lum,
    render_test_support,
    run_sweep,
    run_test_case,
)


def spatial_noise(frames):
    import numpy as np
    from numpy.lib.stride_tricks import sliding_window_view

    lums = lum(frames)
    return float(np.mean([sliding_window_view(np.pad(f, 1, mode="edge"), (3, 3)).std(axis=(2, 3)).mean()
                          for f in lums]))


def flicker(frames):
    import numpy as np

    return float(np.abs(frames[1:] - frames[:-1]).mean())


def mean_flip(reference, image):
    from flip_evaluator import nbflip

    _, value, _ = nbflip.evaluate(reference, image, False, True, False, True, {})
    return float(value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", default="macos",
                        choices=render_test_support.SUPPORTED_FRAMEWORKS)
    parser.add_argument("--denoisers", default="nrd,metalfx", help="comma-separated backends to compare")
    parser.add_argument("--render_scale", default="1.0")
    parser.add_argument("--perf_frames", type=int, default=2000)
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--skip_build", action="store_true")
    args, passthrough = parser.parse_known_args()

    render_test_support.install_dependencies()
    import numpy as np

    shot_dir = render_test_support.get_screenshot_dir(args.framework)
    work_dir = os.path.join(shot_dir, "denoiser_compare")
    os.makedirs(work_dir, exist_ok=True)
    arms = ["raw"] + [d.strip() for d in args.denoisers.split(",") if d.strip()]
    scale = ["--render_scale", args.render_scale]

    run_test_case(args.framework, "screenshot",
                  ["--clear_screenshots", "true", "--pipeline", "gpu"] + scale + list(passthrough),
                  skip_build=args.skip_build, headless=args.headless)
    reference_path = os.path.join(work_dir, "reference.png")
    shutil.copy(render_test_support.find_screenshot(args.framework), reference_path)
    reference = load_image(reference_path).astype(np.float32)

    results = {}
    for arm in arms:
        denoiser = [] if arm == "raw" else ["--denoiser", arm]
        row = {}

        run_sweep(args.framework,
                  ["--max_spp", "100000", "--sweep_step_degrees", "0", "--sweep_settle_frames", "16"]
                  + scale + denoiser + list(passthrough), skip_build=True, headless=args.headless)
        frames = load_sweep_frames(shot_dir)
        row["static_noise"] = spatial_noise(frames)
        row["static_flicker"] = flicker(frames)
        row["static_flip"] = mean_flip(reference, frames[8].astype(np.float32))
        shutil.copy(os.path.join(shot_dir, "denoiser_sweep_8.png"),
                    os.path.join(work_dir, f"static_{arm}.png"))

        for axis, motion in [("yaw", ["--sweep_step_degrees", "2.0"]),
                             ("pitch", ["--sweep_step_degrees", "0.0", "--sweep_pitch_step_degrees", "2.0"])]:
            run_sweep(args.framework, ["--max_spp", "1"] + motion + scale + denoiser + list(passthrough),
                      skip_build=True, headless=args.headless)
            frames = load_sweep_frames(shot_dir)
            row[f"{axis}_noise"] = spatial_noise(frames)
            shutil.copy(os.path.join(shot_dir, "denoiser_sweep_8.png"),
                        os.path.join(work_dir, f"{axis}_{arm}.png"))

        start = time.monotonic()
        run_sweep(args.framework,
                  ["--max_spp", "100000", "--sweep_step_degrees", "0",
                   "--sweep_settle_frames", str(args.perf_frames)] + scale + denoiser + list(passthrough),
                  skip_build=True, headless=args.headless)
        row["perf_wall"] = time.monotonic() - start

        results[arm] = row

    raw_wall = results["raw"]["perf_wall"]
    print(f"\n==== denoiser comparison (render_scale {args.render_scale}, "
          f"perf over {args.perf_frames} static frames) ====")
    print(f"{'arm':>8} {'st_noise':>9} {'st_flicker':>10} {'st_flip':>8} {'yaw_noise':>9} {'pitch_noise':>11} "
          f"{'cost_ms/frame':>13}")
    for arm, row in results.items():
        cost = (row["perf_wall"] - raw_wall) / args.perf_frames * 1000.0
        cost_text = f"{cost:13.2f}" if arm != "raw" else f"{'0 (base)':>13}"
        print(f"{arm:>8} {row['static_noise']:9.4f} {row['static_flicker']:10.4f} {row['static_flip']:8.4f} "
              f"{row['yaw_noise']:9.4f} {row['pitch_noise']:11.4f} {cost_text}")
    print(f"captures in {work_dir}")


if __name__ == "__main__":
    main()
