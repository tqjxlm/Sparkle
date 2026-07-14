"""Motion-phase fidelity: systematic error of the NRD output under motion, vs ground truth at the pose.

Methodology (see docs/RenderingValidation.md + the fidelity criterion): a single motion frame is
noise-confounded, so capture the SAME motion frame (last sweep capture, frame 15) across M seed-offset
realizations and average; the sweep then holds that exact pose, so a raw converged run provides ground
truth. Reported per region (spec cluster / floor / whole):

  * bias        : mean(mean_of_realizations - GT) — signed; the "darkening" if negative.
  * systematic  : RMS of max(|mean - GT| - 2*stderr, 0) — deviation that noise cannot explain.
  * sharpness   : laplacian RMS ratio (mean / GT) — <1 = frosted/over-blurred, ~1 = detail preserved.

Outputs tmp/nrd_diff/motion_fidelity.png (GT | mean | systematic x8) for the semantic gate.

Run:  python3 tests/nrd/nrd_motion_fidelity.py [--skip_build] [--realizations 4] [--headless]
"""

import argparse
import os
import shutil

from nrd_common import PROJECT_ROOT, load_image, lum, render_test_support, run_sweep

SEED_STRIDE = 7919
MOTION_FRAME = 15
REGIONS = {"spec_cluster": (slice(170, 400), slice(500, 1000)),
           "floor": (slice(560, 715), slice(100, 1180)),
           "whole": (slice(None), slice(None))}


def run(args, extra, skip_build):
    run_sweep(args.framework, ["--sweep_step_degrees", "2.0"] + extra,
              skip_build=skip_build, headless=args.headless)


def laplacian_rms(img):
    import numpy as np
    lums = lum(img)
    lap = -4 * lums
    lap += np.roll(lums, 1, 0) + np.roll(lums, -1, 0) + np.roll(lums, 1, 1) + np.roll(lums, -1, 1)
    return np.sqrt((lap[1:-1, 1:-1] ** 2).mean())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", default="macos",
                        choices=render_test_support.SUPPORTED_FRAMEWORKS)
    parser.add_argument("--realizations", type=int, default=8)
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--skip_build", action="store_true")
    args, passthrough = parser.parse_known_args()

    shot_dir = render_test_support.get_screenshot_dir(args.framework)
    work_dir = os.path.join(shot_dir, "motion_fidelity")
    os.makedirs(work_dir, exist_ok=True)

    run(args, ["--max_spp", "512", "--sweep_capture_converged", "true"] + list(passthrough), args.skip_build)
    shutil.copy(os.path.join(shot_dir, "denoiser_sweep_converged.png"), os.path.join(work_dir, "gt.png"))
    shutil.copy(os.path.join(shot_dir, f"denoiser_sweep_{MOTION_FRAME}.png"), os.path.join(work_dir, "raw_motion.png"))

    # raw realizations calibrate the metric: raw is unbiased, so its "systematic" is the noise floor
    # that M realizations cannot subtract; the denoiser's real error is its EXCESS over that floor.
    for arm, extra in [("nrd", ["--nrd", "true"]), ("raw", [])]:
        for m in range(args.realizations):
            run(args, ["--max_spp", "1", "--random_seed_offset", str(m * SEED_STRIDE)] + extra
                + list(passthrough), True)
            shutil.copy(os.path.join(shot_dir, f"denoiser_sweep_{MOTION_FRAME}.png"),
                        os.path.join(work_dir, f"{arm}_motion_{m}.png"))

    import numpy as np
    from PIL import Image

    gt = load_image(os.path.join(work_dir, "gt.png"))

    def stats(arm):
        reals = np.stack([load_image(os.path.join(work_dir, f"{arm}_motion_{m}.png"))
                          for m in range(args.realizations)])
        mean = reals.mean(axis=0)
        stderr = reals.std(axis=0) / np.sqrt(args.realizations)
        return mean, np.maximum(np.abs(mean - gt) - 2.0 * stderr, 0.0)

    mean, systematic = stats("nrd")
    raw_mean, raw_systematic = stats("raw")

    print(f"\n==== NRD motion-phase fidelity (M={args.realizations}, pose = last sweep capture) ====")
    print(f"{'region':>14} | {'bias':>8} {'systematic':>10} {'noise_floor':>11} {'excess':>8} {'sharpness':>9}")
    for name, (ys, xs) in REGIONS.items():
        bias = float((mean - gt)[ys, xs].mean())
        syst = float(np.sqrt((systematic[ys, xs] ** 2).mean()))
        floor = float(np.sqrt((raw_systematic[ys, xs] ** 2).mean()))
        excess = max(syst - floor, 0.0)
        sharp = laplacian_rms(mean[ys, xs]) / max(laplacian_rms(gt[ys, xs]), 1e-6)
        print(f"{name:>14} | {bias:+8.4f} {syst:10.4f} {floor:11.4f} {excess:8.4f} {sharp:9.3f}")

    tiles = [gt, mean, np.clip(systematic * 8.0, 0, 1)]
    montage = Image.new("RGB", (gt.shape[1] * 3 + 8, gt.shape[0]), (24, 24, 24))
    for i, tile in enumerate(tiles):
        montage.paste(Image.fromarray((np.clip(tile, 0, 1) * 255).astype("uint8")), (i * (gt.shape[1] + 4), 0))
    out = os.path.join(PROJECT_ROOT, "tmp", "nrd_diff", "motion_fidelity.png")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    montage.save(out)
    print(f"\nmontage written: {out} (GT | mean of {args.realizations} | systematic x8)")


if __name__ == "__main__":
    main()
