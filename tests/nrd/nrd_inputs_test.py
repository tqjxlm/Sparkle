"""NRD channel gate: every ReBLUR input/output channel, checked semantically AND statistically.

For each --nrd_debug view this renders a screenshot (static camera, real TestScene) and asserts
distribution properties that distinguish a correct channel from a broken/mis-bound one. The saved views
in <screenshots>/nrd_inputs/ are the semantic half — eyeball them; the asserts are the repeatable half.

Note: views pass through tone mapping (monotonic, preserves r=g=b and zeros), so thresholds are
calibrated on post-tonemap values.

  ViewZ           grayscale; geometry gradient + sky saturated bright
  NormalRoughness colorful (normals) — must NOT be grayscale
  Roughness       grayscale with structure (different materials)
  MotionVector    static camera => all zeros (near-black)
  DiffRadiance    colorful demodulated diffuse with structure
  DiffHitDist     grayscale in [0,1] with structure
  SpecRadiance    specular radiance (sparse/dark is fine; must be finite)
  SpecHitDist     grayscale
  DenoisedDiff    ReBLUR output: smooth, colorful, structured (proves dispatches wrote real data)
  DenoisedSpec    ReBLUR output: finite, structured

Run:  python3 tests/nrd/nrd_inputs_test.py [--skip_build] [--spp 16] [--views ViewZ,MotionVector]
"""

import argparse
import os
import shutil
import sys

from nrd_common import ft, load_image, lum  # noqa: E402


def stats_of(path):
    import numpy as np

    rgb = load_image(path)
    lums = lum(rgb)
    return {
        "mean": float(lums.mean()),
        "std": float(lums.std()),
        "near_black": float((lums < 0.02).mean()),
        "bright": float((lums > 0.75).mean()),
        "chroma_dev": float(np.mean(np.abs(rgb[..., 0] - rgb[..., 1]) + np.abs(rgb[..., 1] - rgb[..., 2])) / 2.0),
    }


CHECKS = {
    "ViewZ": lambda s: [
        ("grayscale", s["chroma_dev"] < 0.01),
        ("structure", s["std"] > 0.05),
        ("sky bright", s["bright"] > 0.05),
    ],
    "NormalRoughness": lambda s: [
        ("colorful (normals)", s["chroma_dev"] > 0.03),
        ("structure", s["std"] > 0.03),
    ],
    "Roughness": lambda s: [
        ("grayscale", s["chroma_dev"] < 0.01),
        ("structure (materials differ)", s["std"] > 0.02),
    ],
    "MotionVector": lambda s: [
        ("static camera => zero motion", s["near_black"] > 0.98),
    ],
    "DiffRadiance": lambda s: [
        ("colorful", s["chroma_dev"] > 0.01),
        ("structure", s["std"] > 0.03),
        ("not black", s["mean"] > 0.05),
    ],
    "DiffHitDist": lambda s: [
        ("grayscale", s["chroma_dev"] < 0.01),
        ("structure", s["std"] > 0.03),
        ("not empty", s["mean"] > 0.02),
    ],
    "SpecRadiance": lambda s: [
        ("not fully black", s["mean"] > 0.005),
    ],
    "SpecHitDist": lambda s: [
        ("grayscale", s["chroma_dev"] < 0.01),
        ("not empty", s["mean"] > 0.005),
    ],
    "DenoisedDiff": lambda s: [
        ("colorful", s["chroma_dev"] > 0.01),
        ("structure", s["std"] > 0.03),
        ("not black (dispatches wrote data)", s["mean"] > 0.05),
    ],
    "DenoisedSpec": lambda s: [
        ("not fully black (dispatches wrote data)", s["mean"] > 0.005),
    ],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", default="macos", choices=ft.SUPPORTED_FRAMEWORKS)
    parser.add_argument("--spp", type=int, default=16)
    parser.add_argument("--views", default=",".join(CHECKS.keys()))
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--skip_build", action="store_true")
    args = parser.parse_args()

    out_dir = os.path.join(ft.get_screenshot_dir(args.framework), "nrd_inputs")
    os.makedirs(out_dir, exist_ok=True)

    skip_build = args.skip_build
    failures = []
    for view in args.views.split(","):
        ft.build_and_run(args.framework, "gpu", None,
                         ["--nrd", "true", "--nrd_debug", view, "--max_spp", str(args.spp)],
                         headless=args.headless, skip_build=skip_build)
        skip_build = True  # one build is enough for all views

        shot = ft.find_screenshot(args.framework)
        dst = os.path.join(out_dir, f"{view}.png")
        shutil.copy(shot, dst)

        s = stats_of(dst)
        print(f"{view}: " + " ".join(f"{k}={v:.4f}" for k, v in s.items()))
        for name, ok in CHECKS[view](s):
            print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
            if not ok:
                failures.append(f"{view}: {name}")

    print("nrd_inputs:", "PASS" if not failures else f"FAIL ({len(failures)}): {failures}")
    sys.exit(0 if not failures else 1)


if __name__ == "__main__":
    main()
