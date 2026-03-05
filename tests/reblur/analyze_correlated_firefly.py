#!/usr/bin/env python3
"""
Deep analysis of correlated firefly clusters in ReBLUR.

Investigates whether the remaining instability is caused by specular caustic
patches that oscillate coherently, bypassing spatial-neighborhood-based
suppression in both anti-firefly (HistoryFix) and temporal stabilization.

Hypothesis: When a specular caustic illuminates a patch of pixels, all pixels
in the 5x5 stabilization neighborhood shift together. The stabilization's
color box follows the neighborhood mean, and the clamped history jumps to
the new value. This defeats the temporal averaging that stabilization provides.
"""

import sys
from pathlib import Path

try:
    from PIL import Image
    import numpy as np
except ImportError:
    print("ERROR: pip install pillow numpy")
    sys.exit(1)


SCREENSHOTS_DIR = (
    Path(__file__).parent.parent.parent
    / "build_system" / "glfw" / "output" / "build" / "generated" / "screenshots"
)


def load_frames(subdir):
    d = SCREENSHOTS_DIR / subdir
    frames = []
    for i in range(5):
        p = d / f"multi_frame_{i}.png"
        if not p.exists():
            return None
        frames.append(np.array(Image.open(p), dtype=np.float32) / 255.0)
    return frames


def get_luma(frame):
    return np.dot(frame[:, :, :3], [0.2126, 0.7152, 0.0722])


def analyze_cluster(frames, cx, cy, label, radius=15):
    """Analyze a spatial cluster of pixels around (cx, cy)."""
    H, W = frames[0].shape[:2]
    y0 = max(0, cy - radius)
    y1 = min(H, cy + radius + 1)
    x0 = max(0, cx - radius)
    x1 = min(W, cx + radius + 1)

    print(f"\n{'='*80}")
    print(f"  Cluster analysis: center=({cx},{cy}), window=[{x0}:{x1}, {y0}:{y1}]")
    print(f"  Source: {label}")
    print(f"{'='*80}")

    # Compute per-pixel luma across frames for this region
    lumas = []
    for f in frames:
        lumas.append(get_luma(f[y0:y1, x0:x1]))
    lumas = np.stack(lumas)  # (5, H_region, W_region)

    # Frame-to-frame changes
    for i in range(4):
        diff = np.abs(lumas[i+1] - lumas[i])
        pct_changed = np.mean(diff > 1.0/255) * 100
        mean_change = np.mean(diff)
        max_change = np.max(diff)
        # Spatial correlation: what fraction of the region changes in same direction?
        direction = np.sign(lumas[i+1] - lumas[i])
        pos_frac = np.mean(direction > 0) * 100
        neg_frac = np.mean(direction < 0) * 100
        zero_frac = np.mean(direction == 0) * 100
        print(f"  Frame {i}->{i+1}: {pct_changed:.1f}% changed, mean={mean_change:.4f}, max={max_change:.4f}")
        print(f"           Direction: {pos_frac:.0f}% up, {neg_frac:.0f}% down, {zero_frac:.0f}% same")

    # Analyze correlation between pixel pairs
    flat_lumas = lumas.reshape(5, -1)  # (5, N)
    per_pixel_std = np.std(flat_lumas, axis=0)

    # Find the most unstable pixels in this region
    unstable_mask = per_pixel_std > 0.01
    n_unstable = np.sum(unstable_mask)
    total = len(per_pixel_std)

    print(f"\n  Region stats: {total} pixels, {n_unstable} unstable (std>0.01) = {n_unstable/total*100:.1f}%")

    if n_unstable >= 2:
        # Compute correlation matrix of unstable pixel luminances
        unstable_lumas = flat_lumas[:, unstable_mask]  # (5, N_unstable)
        # Pairwise correlation
        if unstable_lumas.shape[1] > 1:
            corr_matrix = np.corrcoef(unstable_lumas.T)
            mean_corr = np.mean(corr_matrix[np.triu_indices_from(corr_matrix, k=1)])
            print(f"  Mean pairwise correlation of unstable pixels: {mean_corr:.3f}")
            if mean_corr > 0.5:
                print(f"  ** HIGH CORRELATION -> Correlated caustic/firefly patch confirmed **")
            elif mean_corr > 0.2:
                print(f"  ** MODERATE CORRELATION -> Partially correlated instability **")
            else:
                print(f"  ** LOW CORRELATION -> Independent pixel instability **")

    # Check 5x5 neighborhood coherence for the worst pixel
    region_stds = np.std(lumas, axis=0)
    worst_local_yx = np.unravel_index(np.argmax(region_stds), region_stds.shape)
    worst_y, worst_x = worst_local_yx
    print(f"\n  Worst pixel in region: local=({worst_x},{worst_y}), "
          f"global=({x0+worst_x},{y0+worst_y}), std={region_stds[worst_y, worst_x]:.5f}")

    # 5x5 neighborhood around worst pixel
    ny0 = max(0, worst_y - 2)
    ny1 = min(lumas.shape[1], worst_y + 3)
    nx0 = max(0, worst_x - 2)
    nx1 = min(lumas.shape[2], worst_x + 3)

    print(f"  5x5 neighborhood of worst pixel (frame luminances):")
    for f_idx in range(5):
        patch = lumas[f_idx, ny0:ny1, nx0:nx1]
        mean_v = np.mean(patch)
        std_v = np.std(patch)
        min_v = np.min(patch)
        max_v = np.max(patch)
        print(f"    Frame {f_idx}: mean={mean_v:.4f}, std={std_v:.4f}, "
              f"range=[{min_v:.4f}, {max_v:.4f}]")

    # Key diagnostic: does the neighborhood mean oscillate?
    neigh_means = [float(np.mean(lumas[f_idx, ny0:ny1, nx0:nx1])) for f_idx in range(5)]
    neigh_mean_std = np.std(neigh_means)
    print(f"  Neighborhood mean across frames: {' -> '.join(f'{m:.4f}' for m in neigh_means)}")
    print(f"  Neighborhood mean std: {neigh_mean_std:.5f}")
    if neigh_mean_std > 0.01:
        print(f"  ** NEIGHBORHOOD MEAN IS UNSTABLE -> Stabilization clamping box oscillates **")


def find_firefly_clusters(frames, threshold_std=0.1, min_cluster_size=3):
    """Find spatially connected clusters of unstable (firefly) pixels."""
    stack = np.stack([get_luma(f) for f in frames])
    per_pixel_std = np.std(stack, axis=0)

    # Binary mask of firefly-like pixels
    mask = per_pixel_std > threshold_std

    # Connected component labeling (simple flood fill)
    H, W = mask.shape
    visited = np.zeros((H, W), dtype=bool)
    clusters = []

    for y in range(H):
        for x in range(W):
            if mask[y, x] and not visited[y, x]:
                # BFS flood fill
                queue = [(y, x)]
                visited[y, x] = True
                component = []
                while queue:
                    cy, cx = queue.pop(0)
                    component.append((cy, cx))
                    for dy, dx in [(-1,0),(1,0),(0,-1),(0,1)]:
                        ny, nx = cy+dy, cx+dx
                        if 0 <= ny < H and 0 <= nx < W and mask[ny, nx] and not visited[ny, nx]:
                            visited[ny, nx] = True
                            queue.append((ny, nx))
                if len(component) >= min_cluster_size:
                    ys = [c[0] for c in component]
                    xs = [c[1] for c in component]
                    clusters.append({
                        "size": len(component),
                        "center": (int(np.mean(xs)), int(np.mean(ys))),
                        "bbox": (min(xs), min(ys), max(xs), max(ys)),
                        "max_std": max(per_pixel_std[c[0], c[1]] for c in component),
                    })

    clusters.sort(key=lambda c: c["size"], reverse=True)
    return clusters


def analyze_stab_clamping_failure(postblur_frames, full_frames):
    """Compare PostBlur to Full pipeline to identify where stabilization fails."""
    print(f"\n{'='*80}")
    print(f"  Stabilization Failure Analysis")
    print(f"{'='*80}")

    pb_lumas = np.stack([get_luma(f) for f in postblur_frames])
    full_lumas = np.stack([get_luma(f) for f in full_frames])

    pb_std = np.std(pb_lumas, axis=0)
    full_std = np.std(full_lumas, axis=0)

    # Pixels where stabilization DIDN'T help (full_std close to pb_std)
    pb_unstable = pb_std > 0.05
    full_unstable = full_std > 0.05

    stab_failed = pb_unstable & full_unstable

    if np.sum(stab_failed) > 0:
        # Amplification ratio for failed pixels
        ratios = full_std[stab_failed] / np.maximum(pb_std[stab_failed], 1e-8)
        print(f"  Pixels where PostBlur std>0.05 AND full std>0.05: {np.sum(stab_failed)}")
        print(f"  Stabilization retention ratio: mean={np.mean(ratios):.3f}, "
              f"median={np.median(ratios):.3f}")
        print(f"  (ratio near 1.0 = stabilization not helping at all)")

        # For these pixels, check neighborhood correlation
        ys, xs = np.where(stab_failed)
        n_sample = min(50, len(ys))
        high_corr_count = 0
        low_corr_count = 0

        for idx in range(n_sample):
            y, x = ys[idx], xs[idx]
            H, W = pb_std.shape
            ny0, ny1 = max(0, y-2), min(H, y+3)
            nx0, nx1 = max(0, x-2), min(W, x+3)

            # Compute correlation between this pixel and its 5x5 neighbors
            center_series = pb_lumas[:, y, x]  # (5,)
            neighbors_series = pb_lumas[:, ny0:ny1, nx0:nx1].reshape(5, -1)  # (5, N_neigh)
            correlations = []
            for j in range(neighbors_series.shape[1]):
                if np.std(neighbors_series[:, j]) > 0.001:
                    corr = np.corrcoef(center_series, neighbors_series[:, j])[0, 1]
                    if not np.isnan(corr):
                        correlations.append(corr)
            if correlations:
                mean_corr = np.mean(correlations)
                if mean_corr > 0.5:
                    high_corr_count += 1
                else:
                    low_corr_count += 1

        print(f"\n  Of {n_sample} sampled failed-stabilization pixels:")
        print(f"    High neighborhood correlation (>0.5): {high_corr_count} ({high_corr_count/n_sample*100:.0f}%)")
        print(f"    Low neighborhood correlation:         {low_corr_count} ({low_corr_count/n_sample*100:.0f}%)")
        if high_corr_count > low_corr_count:
            print(f"  ** CONFIRMED: Correlated patches defeat neighborhood-based stabilization **")
    else:
        print(f"  No pixels with both PostBlur std>0.05 and full std>0.05")


def analyze_temporal_accum_firefly_input(ta_frames, full_frames):
    """Check if the temporal accumulation input (1-spp) causes frame jumps."""
    print(f"\n{'='*80}")
    print(f"  Temporal Accumulation Input Analysis")
    print(f"{'='*80}")

    ta_lumas = np.stack([get_luma(f) for f in ta_frames])

    # For the worst unstable pixels in the full pipeline, check the temporal accum output
    full_stack = np.stack([get_luma(f) for f in full_frames])
    full_std = np.std(full_stack, axis=0)

    # Top 50 worst pixels
    flat_indices = np.argsort(full_std.ravel())[::-1][:50]
    H, W = full_std.shape

    # At accumSpeed=63, the frame-to-frame change in temporal accum output reflects
    # the 1-spp sample. If the temporal output jumps by X between frames:
    #   X = 1/64 * (new_1spp - history)
    #   new_1spp = history + 64 * X
    print(f"\n  Inferred 1-spp sample extremity for worst full-pipeline pixels:")
    print(f"  {'Pixel':>12} {'TA Jump':>10} {'Inferred 1spp':>15} {'Full Jump':>12} {'TA Std':>10}")
    for idx in flat_indices[:20]:
        y, x = idx // W, idx % W
        ta_series = ta_lumas[:, y, x]
        full_series = full_stack[:, y, x]

        # Max consecutive jump in temporal accum
        ta_diffs = np.diff(ta_series)
        max_ta_jump = np.max(np.abs(ta_diffs))
        # Inferred 1-spp value: assuming accumSpeed=63
        max_jump_idx = np.argmax(np.abs(ta_diffs))
        history_val = ta_series[max_jump_idx]
        new_val = ta_series[max_jump_idx + 1]
        inferred_1spp = history_val + 64 * (new_val - history_val)

        max_full_jump = np.max(np.abs(np.diff(full_series)))
        ta_std = np.std(ta_series)

        print(f"  ({x:4d},{y:4d})  {max_ta_jump:>10.4f} {inferred_1spp:>15.2f} "
              f"{max_full_jump:>12.4f} {ta_std:>10.4f}")


def main():
    print("=" * 80)
    print("  Correlated Firefly Cluster Analysis")
    print("=" * 80)

    postblur_frames = load_frames("reblur_postblur")
    full_frames = load_frames("reblur_full_current")
    ta_frames = load_frames("reblur_temporal_accum")
    vanilla_frames = load_frames("vanilla_current")

    if not all([postblur_frames, full_frames, ta_frames]):
        print("ERROR: Missing screenshot data. Run diagnostic captures first.")
        sys.exit(1)

    # 1. Find firefly clusters
    print("\n--- Firefly Clusters (full pipeline, std > 0.1) ---")
    clusters = find_firefly_clusters(full_frames, threshold_std=0.1, min_cluster_size=3)
    print(f"  Found {len(clusters)} clusters")
    for i, c in enumerate(clusters[:10]):
        print(f"  #{i+1}: {c['size']} pixels, center={c['center']}, "
              f"bbox={c['bbox']}, max_std={c['max_std']:.4f}")

    # 2. Analyze the top clusters across all stages
    for i, c in enumerate(clusters[:3]):
        cx, cy = c["center"]
        analyze_cluster(full_frames, cx, cy, "full pipeline")
        analyze_cluster(postblur_frames, cx, cy, "postblur (pre-stabilization)")
        analyze_cluster(ta_frames, cx, cy, "temporal accumulation")

    # 3. Stabilization failure analysis
    analyze_stab_clamping_failure(postblur_frames, full_frames)

    # 4. Temporal accumulation input analysis
    analyze_temporal_accum_firefly_input(ta_frames, full_frames)

    print(f"\n{'='*80}")
    print("  Analysis complete.")


if __name__ == "__main__":
    main()
