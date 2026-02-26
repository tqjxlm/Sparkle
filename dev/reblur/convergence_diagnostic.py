#!/usr/bin/env python3
"""
ReBLUR Convergence Diagnostic Script

Captures denoiser output at various stages and max_spp values to isolate
convergence bottlenecks. Compares:
1. Temporal-only output at max_spp=64 vs max_spp=2048 (tests accumSpeed cap)
2. Per-stage instability (which pass introduces most frame-to-frame variation)
3. Vanilla reference for ground truth comparison

Evidence gathered:
- If temporal-only output is similar at 64 and 2048 spp → accumSpeed cap is bottleneck
- Per-stage instability numbers → identifies which pass is the weakest link
"""

import argparse
import subprocess
import sys
import os
import shutil
import tempfile
from pathlib import Path

try:
    from PIL import Image
    import numpy as np
except ImportError:
    print("ERROR: pip install pillow numpy")
    sys.exit(1)

SCRIPT_DIR = Path(__file__).parent.parent.parent
BUILD_PY = SCRIPT_DIR / "build.py"
SUPPORTED_FRAMEWORKS = ("glfw", "macos")
RESULTS_DIR = Path(tempfile.gettempdir()) / "sparkle_convergence_diagnostic"

# Set by main() after parsing args
_framework = None
_screenshot_dir = None


def get_screenshot_dir(framework):
    if framework == "glfw":
        return SCRIPT_DIR / "build_system" / "glfw" / "output" / "build" / "generated" / "screenshots"
    if framework == "macos":
        return Path.home() / "Documents" / "sparkle" / "screenshots"
    raise ValueError(f"Unsupported framework: {framework}")


def run_app(max_spp, use_reblur, debug_pass=99, test_case="multi_frame_screenshot"):
    cmd = [
        sys.executable, str(BUILD_PY),
        "--framework", _framework,
        "--pipeline", "gpu",
        "--use_reblur", "true" if use_reblur else "false",
        "--max_spp", str(max_spp),
        "--run", "--headless", "true",
        "--test_case", test_case,
        "--clear_screenshots", "true",
        "--skip_build",
    ]
    if use_reblur and debug_pass != 99:
        cmd += ["--reblur_debug_pass", str(debug_pass)]

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if result.returncode != 0:
        print(f"  FAILED: {result.stderr[-500:]}")
        return False
    return True


def collect_screenshots(label):
    """Copy screenshots from app output to results dir."""
    dest = RESULTS_DIR / label
    dest.mkdir(parents=True, exist_ok=True)
    files = sorted(_screenshot_dir.glob("multi_frame_*.png"))
    for f in files:
        shutil.copy2(f, dest / f.name)
    return len(files)


def analyze_stability(dirname, label):
    """Analyze frame-to-frame stability of consecutive screenshots."""
    frames = []
    for i in range(5):
        path = dirname / f"multi_frame_{i}.png"
        if not path.exists():
            return None
        frames.append(np.array(Image.open(path), dtype=np.float32) / 255.0)

    diffs = []
    for i in range(4):
        diff = np.abs(frames[i + 1] - frames[i])
        mean_diff = np.mean(diff)
        max_diff = np.max(diff)
        pct_1 = np.mean(np.any(diff > 1.0 / 255, axis=2)) * 100
        pct_5 = np.mean(np.any(diff > 5.0 / 255, axis=2)) * 100
        diffs.append((mean_diff, max_diff, pct_1, pct_5))

    avg_mean = np.mean([d[0] for d in diffs])
    avg_max = np.mean([d[1] for d in diffs])
    avg_pct1 = np.mean([d[2] for d in diffs])
    avg_pct5 = np.mean([d[3] for d in diffs])

    return {
        "label": label,
        "mean_diff": avg_mean,
        "max_diff": avg_max,
        "pct_changed_1": avg_pct1,
        "pct_changed_5": avg_pct5,
        "mean_luma": np.mean(frames[-1][:, :, :3]),
    }


def main():
    global _framework, _screenshot_dir

    parser = argparse.ArgumentParser(description="ReBLUR convergence diagnostic")
    parser.add_argument("--framework", required=True, choices=SUPPORTED_FRAMEWORKS)
    args = parser.parse_args()

    _framework = args.framework
    _screenshot_dir = get_screenshot_dir(args.framework)

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)

    tests = [
        # Test 1: Does temporal quality improve after 64 frames? (accumSpeed cap test)
        ("reblur_temporal_64spp", 64, True, 3),
        ("reblur_temporal_2048spp", 2048, True, 3),
        # Test 2: Per-stage instability at 2048 spp
        ("reblur_prepass_2048spp", 2048, True, 0),
        ("reblur_histfix_2048spp", 2048, True, 4),
        ("reblur_postblur_2048spp", 2048, True, 2),
        ("reblur_full_2048spp", 2048, True, 99),
        # Test 3: Vanilla reference
        ("vanilla_2048spp", 2048, False, 99),
    ]

    results = []

    for label, max_spp, use_reblur, debug_pass in tests:
        print(f"\n--- Running: {label} (max_spp={max_spp}, reblur={use_reblur}, debug_pass={debug_pass}) ---")
        if run_app(max_spp, use_reblur, debug_pass):
            n = collect_screenshots(label)
            print(f"  Collected {n} screenshots")
            r = analyze_stability(RESULTS_DIR / label, label)
            if r:
                results.append(r)
                print(f"  mean_diff={r['mean_diff']:.6f}, max={r['max_diff']:.4f}, "
                      f"chg>1/255={r['pct_changed_1']:.2f}%, chg>5/255={r['pct_changed_5']:.2f}%, "
                      f"luma={r['mean_luma']:.4f}")

    # Summary
    print("\n" + "=" * 100)
    print(f"{'Label':<35} {'Mean Diff':>10} {'Max Diff':>10} {'Chg>1/255':>10} {'Chg>5/255':>10} {'Luminance':>10}")
    print("-" * 100)
    for r in results:
        print(f"{r['label']:<35} {r['mean_diff']:>10.6f} {r['max_diff']:>10.4f} "
              f"{r['pct_changed_1']:>9.2f}% {r['pct_changed_5']:>9.2f}% {r['mean_luma']:>10.4f}")
    print("=" * 100)

    # Key diagnostic tests
    print("\n=== DIAGNOSTIC CONCLUSIONS ===\n")

    # Test 1: AccumSpeed cap
    t64 = next((r for r in results if r["label"] == "reblur_temporal_64spp"), None)
    t2048 = next((r for r in results if r["label"] == "reblur_temporal_2048spp"), None)
    if t64 and t2048:
        ratio = t2048["pct_changed_1"] / max(t64["pct_changed_1"], 0.001)
        if ratio > 0.8:
            print(f"[EVIDENCE] AccumSpeed cap IS the bottleneck:")
            print(f"  Temporal instability at 64 spp: {t64['pct_changed_1']:.2f}%")
            print(f"  Temporal instability at 2048 spp: {t2048['pct_changed_1']:.2f}%")
            print(f"  Ratio: {ratio:.2f} (close to 1.0 = no improvement beyond 64 frames)")
        else:
            print(f"[INFO] AccumSpeed cap is NOT the only bottleneck (ratio={ratio:.2f})")

    # Test 2: Stage analysis
    postblur = next((r for r in results if r["label"] == "reblur_postblur_2048spp"), None)
    full = next((r for r in results if r["label"] == "reblur_full_2048spp"), None)
    vanilla = next((r for r in results if r["label"] == "vanilla_2048spp"), None)
    if postblur and full and vanilla:
        stab_reduction = (1 - full["pct_changed_1"] / max(postblur["pct_changed_1"], 0.001)) * 100
        print(f"\n[INFO] Stabilization reduces instability by {stab_reduction:.0f}%")
        print(f"  PostBlur: {postblur['pct_changed_1']:.2f}% → Full: {full['pct_changed_1']:.2f}%")
        gap = full["pct_changed_1"] / max(vanilla["pct_changed_1"], 0.001)
        print(f"  ReBLUR vs Vanilla gap: {gap:.1f}x")


if __name__ == "__main__":
    main()
