#!/usr/bin/env python3
"""
Deep analysis of ReBLUR per-stage instability.

For each stage and for the vanilla reference:
1. Measure frame-to-frame instability (% pixels changing by >1/255, >5/255)
2. Identify the worst unstable pixels and characterize their behavior
3. Compute spatial distribution of instability (by region)
4. Compare stages to isolate where instability originates vs amplifies
"""

import sys
from pathlib import Path

try:
    from PIL import Image
    import numpy as np
except ImportError:
    print("ERROR: pip install pillow numpy")
    sys.exit(1)


SCREENSHOTS_DIR = Path(__file__).parent.parent.parent / "build_system" / "glfw" / "output" / "build" / "generated" / "screenshots"


def load_frames(subdir):
    """Load 5 multi_frame screenshots as float32 arrays."""
    d = SCREENSHOTS_DIR / subdir
    frames = []
    for i in range(5):
        p = d / f"multi_frame_{i}.png"
        if not p.exists():
            print(f"  Missing: {p}")
            return None
        frames.append(np.array(Image.open(p), dtype=np.float32) / 255.0)
    return frames


def measure_instability(frames):
    """Compute per-pixel instability stats across consecutive frame pairs."""
    H, W = frames[0].shape[:2]

    # Compute per-pixel standard deviation across 5 frames (per channel, take max)
    stack = np.stack(frames, axis=0)  # (5, H, W, C)
    rgb = stack[:, :, :, :3]
    per_pixel_std = np.std(rgb, axis=0)  # (H, W, 3)
    per_pixel_max_std = np.max(per_pixel_std, axis=2)  # (H, W)

    # Frame-to-frame diffs
    diffs = []
    for i in range(4):
        diff = np.abs(frames[i + 1][:, :, :3] - frames[i][:, :, :3])
        diffs.append(diff)

    # Average frame-to-frame metrics
    pct_1_list = []
    pct_5_list = []
    mean_diff_list = []
    max_diff_list = []
    for diff in diffs:
        pct_1_list.append(float(np.mean(np.any(diff > 1.0/255, axis=2)) * 100))
        pct_5_list.append(float(np.mean(np.any(diff > 5.0/255, axis=2)) * 100))
        mean_diff_list.append(float(np.mean(diff)))
        max_diff_list.append(float(np.max(diff)))

    return {
        "pct_1": np.mean(pct_1_list),
        "pct_5": np.mean(pct_5_list),
        "mean_diff": np.mean(mean_diff_list),
        "max_diff": np.mean(max_diff_list),
        "luma": float(np.mean(frames[-1][:, :, :3])),
        "per_pixel_max_std": per_pixel_max_std,
        "per_pixel_std": per_pixel_std,
    }


def analyze_worst_pixels(frames, stats, label, top_n=20):
    """Identify and characterize the most unstable pixels."""
    max_std = stats["per_pixel_max_std"]
    H, W = max_std.shape

    # Get top-N most unstable pixels
    flat_indices = np.argsort(max_std.ravel())[::-1][:top_n]
    top_pixels = [(idx // W, idx % W) for idx in flat_indices]

    print(f"\n  Top {top_n} most unstable pixels ({label}):")
    print(f"  {'Pixel':>12} {'Std':>8} {'Frame values (luma)':>60}")

    for y, x in top_pixels:
        std_val = max_std[y, x]
        # Get luminance at this pixel across all 5 frames
        lumas = [float(np.dot(frames[f][y, x, :3], [0.2126, 0.7152, 0.0722])) for f in range(5)]
        luma_str = " -> ".join(f"{l:.3f}" for l in lumas)
        print(f"  ({x:4d},{y:4d})  {std_val:.5f}  {luma_str}")


def analyze_spatial_distribution(stats, label):
    """Break down instability by spatial region."""
    max_std = stats["per_pixel_max_std"]
    H, W = max_std.shape

    # Split into regions: top/bottom halves, center/edges
    regions = {
        "Top half": max_std[:H//2, :],
        "Bottom half": max_std[H//2:, :],
        "Center": max_std[H//4:3*H//4, W//4:3*W//4],
        "Edges": np.concatenate([
            max_std[:H//4, :].ravel(),
            max_std[3*H//4:, :].ravel(),
            max_std[H//4:3*H//4, :W//4].ravel(),
            max_std[H//4:3*H//4, 3*W//4:].ravel(),
        ]),
    }

    # Also analyze by luminance band of the final frame
    # (to see if instability correlates with brightness)

    print(f"\n  Spatial distribution ({label}):")
    print(f"  {'Region':>15} {'Mean Std':>10} {'Max Std':>10} {'% > 0.005':>10} {'% > 0.05':>10}")
    for name, region in regions.items():
        r = region.ravel()
        print(f"  {name:>15} {np.mean(r):.6f}   {np.max(r):.5f}   "
              f"{np.mean(r > 0.005)*100:>8.2f}%  {np.mean(r > 0.05)*100:>8.2f}%")


def analyze_instability_vs_luminance(frames, stats, label):
    """Correlate instability with pixel luminance."""
    max_std = stats["per_pixel_max_std"]
    final_frame = frames[-1][:, :, :3]
    luma = np.dot(final_frame, [0.2126, 0.7152, 0.0722])

    bands = [
        ("Very dark (0-0.05)", 0, 0.05),
        ("Dark (0.05-0.2)", 0.05, 0.2),
        ("Mid (0.2-0.5)", 0.2, 0.5),
        ("Bright (0.5-0.8)", 0.5, 0.8),
        ("Very bright (0.8+)", 0.8, 1.1),
    ]

    print(f"\n  Instability vs luminance ({label}):")
    print(f"  {'Band':>25} {'Pixels':>8} {'Mean Std':>10} {'% > 0.005':>10} {'% > 1/255':>10}")
    for name, lo, hi in bands:
        mask = (luma >= lo) & (luma < hi)
        n = np.sum(mask)
        if n == 0:
            continue
        region = max_std[mask]
        # Also compute frame-to-frame change rate for these pixels
        diff = np.abs(frames[-1][:, :, :3] - frames[-2][:, :, :3])
        pixel_changed = np.any(diff > 1.0/255, axis=2)
        pct_changed = np.mean(pixel_changed[mask]) * 100
        print(f"  {name:>25} {n:>8} {np.mean(region):.6f}   {np.mean(region > 0.005)*100:>8.2f}%  {pct_changed:>8.2f}%")


def compare_stages_pixel_by_pixel(all_stats, all_frames):
    """Compare instability patterns between stages to identify where it originates."""
    if "reblur_temporal_accum" not in all_stats or "reblur_full_current" not in all_stats:
        return

    ta = all_stats["reblur_temporal_accum"]["per_pixel_max_std"]
    full = all_stats["reblur_full_current"]["per_pixel_max_std"]

    # Pixels unstable in temporal_accum but stable in full pipeline
    # → stabilization is successfully suppressing these
    ta_unstable = ta > 0.005
    full_unstable = full > 0.005

    fixed = ta_unstable & ~full_unstable
    surviving = ta_unstable & full_unstable
    new = ~ta_unstable & full_unstable

    print(f"\n=== Stage-to-Stage Instability Flow ===")
    print(f"  Unstable in temporal_accum: {np.mean(ta_unstable)*100:.2f}%")
    print(f"  Unstable in full pipeline:  {np.mean(full_unstable)*100:.2f}%")
    print(f"  Fixed by later stages:      {np.mean(fixed)*100:.2f}%")
    print(f"  Surviving through pipeline:  {np.mean(surviving)*100:.2f}%")
    print(f"  NEW instability in later stages: {np.mean(new)*100:.2f}%")

    # For surviving pixels, what's the amplification factor?
    if np.any(surviving):
        amp = full[surviving] / np.maximum(ta[surviving], 1e-8)
        print(f"  Surviving pixels amplification: mean={np.mean(amp):.2f}x, median={np.median(amp):.2f}x")

    # If we have postblur data, check that too
    if "reblur_postblur" in all_stats:
        pb = all_stats["reblur_postblur"]["per_pixel_max_std"]
        pb_unstable = pb > 0.005
        stab_fixed = pb_unstable & ~full_unstable
        stab_surviving = pb_unstable & full_unstable
        stab_new = ~pb_unstable & full_unstable
        print(f"\n  PostBlur → Full Pipeline:")
        print(f"    Unstable in postblur:       {np.mean(pb_unstable)*100:.2f}%")
        print(f"    Fixed by stabilization:     {np.mean(stab_fixed)*100:.2f}%")
        print(f"    Surviving through stab:     {np.mean(stab_surviving)*100:.2f}%")
        print(f"    NEW instability from stab:  {np.mean(stab_new)*100:.2f}%")


def detect_firefly_pattern(frames, stats, label):
    """Detect pixels that show firefly-like behavior: sudden spikes then decay."""
    max_std = stats["per_pixel_max_std"]
    H, W = max_std.shape

    # Get luminance per pixel per frame
    lumas = np.stack([np.dot(f[:, :, :3], [0.2126, 0.7152, 0.0722]) for f in frames])  # (5, H, W)

    # Detect firefly: a pixel where one frame is >3x brighter than the median of others
    median_luma = np.median(lumas, axis=0)  # (H, W)
    max_luma = np.max(lumas, axis=0)

    firefly_mask = (max_luma > 3 * np.maximum(median_luma, 0.01)) & (max_std > 0.01)
    n_firefly = np.sum(firefly_mask)

    # Detect oscillation: pixels that go up then down then up
    oscillation_count = np.zeros((H, W), dtype=int)
    for i in range(1, 4):
        sign_change = (lumas[i+1] - lumas[i]) * (lumas[i] - lumas[i-1]) < 0
        oscillation_count += sign_change.astype(int)

    oscillating_mask = (oscillation_count >= 2) & (max_std > 0.005)
    n_oscillating = np.sum(oscillating_mask)

    print(f"\n  Pattern detection ({label}):")
    print(f"    Firefly pixels (spike >3x median): {n_firefly} ({n_firefly/(H*W)*100:.3f}%)")
    print(f"    Oscillating pixels (2+ reversals, std>0.005): {n_oscillating} ({n_oscillating/(H*W)*100:.3f}%)")

    # Steady drift: pixels that monotonically increase or decrease
    monotonic_inc = np.all(np.diff(lumas, axis=0) >= -0.5/255, axis=0)
    monotonic_dec = np.all(np.diff(lumas, axis=0) <= 0.5/255, axis=0)
    monotonic_mask = (monotonic_inc | monotonic_dec) & (max_std > 0.005)
    n_monotonic = np.sum(monotonic_mask)
    print(f"    Steady drift (monotonic, std>0.005): {n_monotonic} ({n_monotonic/(H*W)*100:.3f}%)")


def main():
    stages = [
        "vanilla_current",
        "reblur_temporal_accum",
        "reblur_historyfix",
        "reblur_postblur",
        "reblur_full_current",
    ]

    all_frames = {}
    all_stats = {}

    print("=" * 100)
    print("ReBLUR Per-Stage Instability Analysis")
    print("=" * 100)

    # Load and measure all stages
    for stage in stages:
        frames = load_frames(stage)
        if frames is None:
            print(f"  Skipping {stage} (no screenshots)")
            continue
        all_frames[stage] = frames
        stats = measure_instability(frames)
        all_stats[stage] = stats

    # Summary table
    print(f"\n{'Stage':<30} {'Mean Diff':>10} {'Max Diff':>10} {'>1/255':>10} {'>5/255':>10} {'Luma':>8}")
    print("-" * 78)
    for stage in stages:
        if stage not in all_stats:
            continue
        s = all_stats[stage]
        print(f"{stage:<30} {s['mean_diff']:>10.6f} {s['max_diff']:>10.4f} "
              f"{s['pct_1']:>8.2f}%  {s['pct_5']:>8.2f}%  {s['luma']:>.4f}")

    # Detailed analysis for each stage
    for stage in stages:
        if stage not in all_stats:
            continue
        print(f"\n{'='*80}")
        print(f"  Stage: {stage}")
        print(f"{'='*80}")

        analyze_worst_pixels(all_frames[stage], all_stats[stage], stage)
        analyze_spatial_distribution(all_stats[stage], stage)
        analyze_instability_vs_luminance(all_frames[stage], all_stats[stage], stage)
        detect_firefly_pattern(all_frames[stage], all_stats[stage], stage)

    # Cross-stage comparison
    compare_stages_pixel_by_pixel(all_stats, all_frames)

    print("\n" + "=" * 100)
    print("Analysis complete.")


if __name__ == "__main__":
    main()
