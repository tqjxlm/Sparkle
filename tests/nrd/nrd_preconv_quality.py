"""Pre-convergence quality metrics: edge fireflies + low-frequency patches.

Consumes the frame sequences captured by nrd_static_stability_test.py (static camera, early window,
--settle 16) plus a converged raw reference, and reports per config:

  * firefly_count / firefly_energy : per-pixel temporal spike = max - median luminance over the sequence;
    a firefly is a spike > threshold. Count/energy on purpose (a mean would average edge sparkles away).
  * firefly_edge_frac : fraction of firefly pixels lying on image edges (gradient mask of the reference)
    — confirms the "on edges" localization.
  * patch_rms : RMS of the 16x16 box-filtered signed deviation (temporal mean - reference). Low-pass
    isolates the low-frequency shadow patches from pixel noise.
  * patch_flicker_rms : RMS of the 16x16 box-filtered temporal std — the patches' breathing.

Run:  python3 tests/nrd/nrd_preconv_quality.py [--gt <converged raw png>]
      (expects <screenshots>/static_stability/{raw,nrd}_{0..15}.png from a --settle 16 run)
"""

import argparse
import os
import sys

from nrd_common import NUM_FRAMES, load_image, lum, render_test_support

FIREFLY_SPIKE = 0.10


def box(img, k):
    import numpy as np
    csum = np.cumsum(np.cumsum(np.pad(img, ((1, 0), (1, 0))), axis=0), axis=1)
    h, w = img.shape
    ys = np.clip(np.arange(h) - k, 0, h)
    ye = np.clip(np.arange(h) + k + 1, 0, h)
    xs = np.clip(np.arange(w) - k, 0, w)
    xe = np.clip(np.arange(w) + k + 1, 0, w)
    area = (ye - ys)[:, None] * (xe - xs)[None, :]
    return (csum[ye][:, xe] - csum[ye][:, xs] - csum[ys][:, xe] + csum[ys][:, xs]) / area


def analyze(name, frames, gt, out_dir):  # returns firefly count for gating
    import numpy as np
    from PIL import Image

    lums = np.stack([lum(f) for f in frames])
    spike = lums.max(axis=0) - np.median(lums, axis=0)
    fireflies = spike > FIREFLY_SPIKE

    gy, gx = np.gradient(lum(gt))
    edges = np.hypot(gx, gy) > 0.03

    count = int(fireflies.sum())
    energy = float(spike[fireflies].sum()) if count else 0.0
    edge_frac = float((fireflies & edges).sum() / max(count, 1))

    mean_img = frames.mean(axis=0)
    patch = box(lum(mean_img) - lum(gt), 16)
    patch_rms = float(np.sqrt((patch ** 2).mean()))
    patch_flicker = box(lums.std(axis=0), 16)
    patch_flicker_rms = float(np.sqrt((patch_flicker ** 2).mean()))

    print(f"{name:>5}: fireflies count={count:6d} energy={energy:8.1f} edge_frac={edge_frac:.2f} | "
          f"patch_rms={patch_rms:.5f} patch_flicker_rms={patch_flicker_rms:.5f}")

    vis = np.clip(np.stack([spike * 4, np.abs(patch) * 8, np.zeros_like(spike)], axis=-1), 0, 1)
    Image.fromarray((vis * 255).astype("uint8")).save(os.path.join(out_dir, f"preconv_{name}.png"))
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", default="macos",
                        choices=render_test_support.SUPPORTED_FRAMEWORKS)
    parser.add_argument("--gt", default=None, help="converged raw reference png")
    parser.add_argument("--denoiser", default="nrd", help="denoised arm to analyze (capture prefix)")
    parser.add_argument("--assert_fireflies", type=int, default=0,
                        help="fail if the denoised arm has more temporally spiking pixels than this (0 = report only)")
    args = parser.parse_args()

    import numpy as np

    work_dir = os.path.join(
        render_test_support.get_screenshot_dir(args.framework), "static_stability")
    gt_path = args.gt or os.path.join(work_dir, "gt.png")
    gt = load_image(gt_path)

    print(f"(firefly = temporal max-median spike > {FIREFLY_SPIKE}; red channel of preconv_<cfg>.png = "
          f"spikes x4, green = low-freq patches x8)")
    counts = {}
    for name in ["raw", args.denoiser]:
        frames = np.stack([load_image(os.path.join(work_dir, f"{name}_{k}.png")) for k in range(NUM_FRAMES)])
        counts[name] = analyze(name, frames, gt, work_dir)

    if args.assert_fireflies > 0:
        ok = counts[args.denoiser] <= args.assert_fireflies
        print(f"firefly gate: {args.denoiser}={counts[args.denoiser]} limit={args.assert_fireflies} "
              f"-> {'PASS' if ok else 'FAIL'}")
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
