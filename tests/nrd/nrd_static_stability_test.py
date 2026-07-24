"""Static-camera temporal stability gate for NRD.

Reuses the frame-precise denoiser_sweep capture with --sweep_step_degrees 0 (static camera, consecutive
frames, no convergence hold) for nrd / raw, then reports and visualizes:

  * flicker series : mean |frame[k+1] - frame[k]| per consecutive pair — distinguishes convergence
    (decaying series) from steady-state churn (flat series = the instability the user sees).
  * temporal std heatmap : per-pixel std over the 16 frames (amplified) — WHERE the instability lives
    (specular objects? floor? everywhere? block patterns?).
  * consecutive diff tiles : amplified |f9 - f8| — WHAT a single frame-to-frame change looks like.

Montage -> tmp/nrd_diff/static_stability_montage.png (rows: raw / nrd).

Run: python3 tests/nrd/nrd_static_stability_test.py --framework macos --headless [--skip_build]
"""

import argparse
import os
import sys

import nrd_common
from nrd_common import NUM_FRAMES, PROJECT_ROOT, render_test_support, run_sweep

SETTLE = 16


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--framework", default="macos",
                   choices=render_test_support.SUPPORTED_FRAMEWORKS)
    p.add_argument("--settle", type=int, default=16,
                   help="frames to accumulate before capturing (16 = early transient; 2000 = converged)")
    p.add_argument("--denoiser", default="nrd", help="denoiser backend for the denoised arm")
    p.add_argument("--headless", action="store_true")
    p.add_argument("--skip_build", action="store_true")
    return p.parse_known_args()


def run_static(args, extra, skip_build, passthrough):
    run_sweep(args.framework,
              ["--max_spp", "100000", "--sweep_step_degrees", "0", "--sweep_settle_frames", str(args.settle)] +
              extra + passthrough,
              skip_build=skip_build, headless=args.headless)


def collect(framework, prefix, work_dir):
    import shutil
    shot_dir = render_test_support.get_screenshot_dir(framework)
    paths = []
    for k in range(NUM_FRAMES):
        src = os.path.join(shot_dir, f"denoiser_sweep_{k}.png")
        if not os.path.exists(src):
            print(f"missing capture: {src}")
            sys.exit(1)
        dst = os.path.join(work_dir, f"{prefix}_{k}.png")
        shutil.copy(src, dst)
        paths.append(dst)
    return paths


def load(paths):
    import numpy as np
    return np.stack([nrd_common.load_image(p) for p in paths])


def analyze(name, frames):
    import numpy as np
    diffs = np.abs(frames[1:] - frames[:-1]).mean(axis=(1, 2, 3))
    series = " ".join(f"{d:.4f}" for d in diffs)
    print(f"{name:>5} flicker per pair: {series}")
    print(f"{name:>5} flicker mean={diffs.mean():.4f} first-half={diffs[:7].mean():.4f} "
          f"second-half={diffs[7:].mean():.4f} (flat halves => steady-state churn, not convergence)")
    return diffs


def build_montage(rows, work_dir):
    import numpy as np
    from PIL import Image, ImageDraw

    tiles = []
    for name, frames in rows:
        f8 = frames[8]
        pair = np.clip(np.abs(frames[9] - frames[8]) * 8.0, 0, 1)
        tstd = np.clip(frames.std(axis=0) * 8.0, 0, 1)
        tiles.append((name, [("frame 8", f8), ("|f9-f8| x8", pair), ("temporal std x8", tstd)]))

    tw, th = tiles[0][1][0][1].shape[1], tiles[0][1][0][1].shape[0]
    pad, label_h = 4, 18
    montage = Image.new("RGB", (3 * (tw + pad) + pad, len(tiles) * (th + pad + label_h) + pad), (24, 24, 24))
    draw = ImageDraw.Draw(montage)
    for r, (name, cols) in enumerate(tiles):
        y = pad + r * (th + pad + label_h)
        for c, (label, img) in enumerate(cols):
            x = pad + c * (tw + pad)
            draw.text((x, y + 2), f"{name} | {label}", fill=(255, 255, 255))
            montage.paste(Image.fromarray((img * 255).astype("uint8")), (x, y + label_h))
    out = os.path.join(PROJECT_ROOT, "tmp", "nrd_diff", f"static_stability_montage_settle{SETTLE}.png")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    montage.save(out)
    return out


def main():
    global SETTLE
    render_test_support.install_dependencies()
    args, passthrough = parse_args()
    SETTLE = args.settle

    work_dir = os.path.join(
        render_test_support.get_screenshot_dir(args.framework), "static_stability")
    os.makedirs(work_dir, exist_ok=True)

    configs = [("raw", []), (args.denoiser, ["--denoiser", args.denoiser])]
    rows = []
    for i, (name, extra) in enumerate(configs):
        run_static(args, extra, skip_build=(args.skip_build or i > 0), passthrough=list(passthrough))
        rows.append((name, load(collect(args.framework, name, work_dir))))

    print("\n==== STATIC camera, consecutive frames (flicker should DECAY to ~0 for a converging denoiser) ====")
    results = {name: analyze(name, frames) for name, frames in rows}

    print(f"\nmontage written: {build_montage(rows, work_dir)}", flush=True)

    # converged-window gate: the denoiser must reach the vanilla renderer's zero-flicker baseline
    if args.settle >= 1000:
        limit = max(0.0006, 2.0 * float(results["raw"].mean()))
        denoised_mean = float(results[args.denoiser].mean())
        ok = denoised_mean <= limit
        print(f"converged flicker gate: {args.denoiser}={denoised_mean:.4f} limit={limit:.4f} "
              f"-> {'PASS' if ok else 'FAIL'}")
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
