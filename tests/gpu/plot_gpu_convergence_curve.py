#!/usr/bin/env python3
"""Plot the GPU convergence curve from a gpu_convergence_curve log."""

from __future__ import annotations

import argparse
import csv
import math
import re
from pathlib import Path


LOG_PATTERN = re.compile(
    r"gpu_convergence_curve: sample_count=(?P<sample>\d+) "
    r"frame_count=(?P<frame>\d+) "
    r"valid=(?P<valid>\d+) "
    r"mean_luma_variance=(?P<mean>[^\s]+) "
    r"max_luma_variance=(?P<max>[^\s]+)"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Plot gpu_convergence_curve output.")
    parser.add_argument(
        "--log",
        default="build_system/glfw/output/build/generated/logs/output.log",
        help="Path to the Sparkle log file.",
    )
    parser.add_argument(
        "--out-dir",
        default="build_system/glfw/output/build/generated/diag",
        help="Directory for the generated csv/html files.",
    )
    parser.add_argument(
        "--name",
        default="gpu_convergence_curve_8192",
        help="Base name for output files.",
    )
    return parser.parse_args()


def parse_points(log_path: Path) -> list[dict[str, float]]:
    points: list[dict[str, float]] = []
    for line in log_path.read_text(encoding="utf-8").splitlines():
        match = LOG_PATTERN.search(line)
        if not match:
            continue
        points.append(
            {
                "sample_count": int(match.group("sample")),
                "frame_count": int(match.group("frame")),
                "valid": int(match.group("valid")),
                "mean_luma_variance": float(match.group("mean")),
                "max_luma_variance": float(match.group("max")),
            }
        )
    return points


def svg_polyline(
    points: list[dict[str, float]],
    key: str,
    chart_width: float,
    chart_height: float,
    x_min: float,
    x_max: float,
    y_min: float,
    y_max: float,
) -> str:
    coords: list[str] = []
    x_span = max(x_max - x_min, 1.0)
    y_span = max(y_max - y_min, 1e-6)
    for point in points:
        value = max(point[key], 1e-12)
        x = ((point["sample_count"] - x_min) / x_span) * chart_width
        y_value = math.log10(value)
        y = chart_height - ((y_value - y_min) / y_span) * chart_height
        coords.append(f"{x:.2f},{y:.2f}")
    return " ".join(coords)


def build_html(points: list[dict[str, float]], title: str) -> str:
    width = 1200
    height = 720
    margin_left = 90
    margin_right = 40
    margin_top = 50
    margin_bottom = 70
    chart_width = width - margin_left - margin_right
    chart_height = height - margin_top - margin_bottom

    x_min = points[0]["sample_count"]
    x_max = points[-1]["sample_count"]

    mean_min = min(max(point["mean_luma_variance"], 1e-12) for point in points)
    mean_max = max(point["mean_luma_variance"] for point in points)
    max_max = max(point["max_luma_variance"] for point in points)

    y_min = math.floor(math.log10(mean_min))
    y_max = math.ceil(math.log10(max_max))

    mean_polyline = svg_polyline(points, "mean_luma_variance", chart_width, chart_height, x_min, x_max, y_min, y_max)
    max_polyline = svg_polyline(points, "max_luma_variance", chart_width, chart_height, x_min, x_max, y_min, y_max)

    grid_lines: list[str] = []
    for exp in range(y_min, y_max + 1):
        y = margin_top + chart_height - ((exp - y_min) / max(y_max - y_min, 1)) * chart_height
        grid_lines.append(
            f"<line x1='{margin_left}' y1='{y:.2f}' x2='{width - margin_right}' y2='{y:.2f}' "
            "stroke='rgba(255,255,255,0.14)' stroke-width='1' />"
        )
        grid_lines.append(
            f"<text x='{margin_left - 12}' y='{y + 4:.2f}' text-anchor='end' fill='#9fb0bf' "
            f"font-size='12'>1e{exp}</text>"
        )

    for tick in range(0, 9):
        x_value = x_min + ((x_max - x_min) * tick / 8.0)
        x = margin_left + (chart_width * tick / 8.0)
        grid_lines.append(
            f"<line x1='{x:.2f}' y1='{margin_top}' x2='{x:.2f}' y2='{height - margin_bottom}' "
            "stroke='rgba(255,255,255,0.10)' stroke-width='1' />"
        )
        grid_lines.append(
            f"<text x='{x:.2f}' y='{height - margin_bottom + 24}' text-anchor='middle' fill='#9fb0bf' "
            f"font-size='12'>{int(round(x_value))}</text>"
        )

    mean_start = points[0]["mean_luma_variance"]
    mean_end = points[-1]["mean_luma_variance"]
    improvement = mean_start / mean_end if mean_end > 0.0 else 0.0

    return f"""<!doctype html>
<html lang='en'>
<head>
  <meta charset='utf-8'>
  <title>{title}</title>
  <style>
    :root {{
      color-scheme: dark;
      --bg: #0f1418;
      --panel: #172028;
      --text: #ebf2f7;
      --muted: #9fb0bf;
      --mean: #67d5b5;
      --max: #ffb84d;
    }}
    body {{
      margin: 0;
      background:
        radial-gradient(circle at top left, rgba(79,140,255,0.14), transparent 34%),
        linear-gradient(180deg, #10171d 0%, var(--bg) 100%);
      color: var(--text);
      font-family: 'Segoe UI', 'Helvetica Neue', sans-serif;
    }}
    main {{
      max-width: 1280px;
      margin: 0 auto;
      padding: 32px 24px 40px;
    }}
    h1 {{
      margin: 0 0 8px;
      font-size: 28px;
      letter-spacing: 0.02em;
    }}
    p {{
      margin: 0 0 20px;
      color: var(--muted);
    }}
    .stats {{
      display: grid;
      grid-template-columns: repeat(4, minmax(0, 1fr));
      gap: 14px;
      margin-bottom: 20px;
    }}
    .card {{
      background: rgba(23,32,40,0.86);
      border: 1px solid rgba(255,255,255,0.08);
      border-radius: 14px;
      padding: 14px 16px;
      box-shadow: 0 12px 30px rgba(0,0,0,0.18);
    }}
    .label {{
      color: var(--muted);
      font-size: 12px;
      text-transform: uppercase;
      letter-spacing: 0.08em;
    }}
    .value {{
      margin-top: 6px;
      font-size: 24px;
      font-weight: 700;
    }}
    .chart-wrap {{
      background: rgba(23,32,40,0.9);
      border: 1px solid rgba(255,255,255,0.08);
      border-radius: 18px;
      padding: 18px;
      box-shadow: 0 12px 30px rgba(0,0,0,0.18);
    }}
    .legend {{
      display: flex;
      gap: 18px;
      margin-bottom: 10px;
      color: var(--muted);
      font-size: 13px;
    }}
    .swatch {{
      display: inline-block;
      width: 12px;
      height: 12px;
      border-radius: 999px;
      margin-right: 8px;
      vertical-align: -1px;
    }}
    code {{
      color: #c5d5e4;
    }}
  </style>
</head>
<body>
  <main>
    <h1>{title}</h1>
    <p>Parsed from <code>gpu_convergence_curve</code> log output. Y axis is log10 variance.</p>
    <section class='stats'>
      <div class='card'><div class='label'>Samples</div><div class='value'>{len(points)}</div></div>
      <div class='card'><div class='label'>First Mean</div><div class='value'>{mean_start:.6g}</div></div>
      <div class='card'><div class='label'>Last Mean</div><div class='value'>{mean_end:.6g}</div></div>
      <div class='card'><div class='label'>Mean Improvement</div><div class='value'>{improvement:.2f}x</div></div>
    </section>
    <section class='chart-wrap'>
      <div class='legend'>
        <span><span class='swatch' style='background: var(--mean)'></span>Mean luma variance</span>
        <span><span class='swatch' style='background: var(--max)'></span>Max luma variance</span>
      </div>
      <svg viewBox='0 0 {width} {height}' width='100%' height='auto' role='img' aria-label='{title}'>
        <rect x='{margin_left}' y='{margin_top}' width='{chart_width}' height='{chart_height}'
              rx='14' fill='#11181f' stroke='rgba(255,255,255,0.08)' />
        {''.join(grid_lines)}
        <polyline fill='none' stroke='var(--max)' stroke-width='2.4' points='{max_polyline}'
                  transform='translate({margin_left}, {margin_top})' />
        <polyline fill='none' stroke='var(--mean)' stroke-width='2.4' points='{mean_polyline}'
                  transform='translate({margin_left}, {margin_top})' />
        <text x='{margin_left + chart_width / 2:.2f}' y='{height - 18}' text-anchor='middle'
              fill='#9fb0bf' font-size='14'>Sample Count</text>
        <text x='20' y='{margin_top + chart_height / 2:.2f}' text-anchor='middle'
              fill='#9fb0bf' font-size='14' transform='rotate(-90 20 {margin_top + chart_height / 2:.2f})'>
          Variance (log10)
        </text>
      </svg>
    </section>
  </main>
</body>
</html>
"""


def write_csv(points: list[dict[str, float]], csv_path: Path) -> None:
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["sample_count", "frame_count", "valid", "mean_luma_variance", "max_luma_variance"])
        for point in points:
            writer.writerow(
                [
                    point["sample_count"],
                    point["frame_count"],
                    point["valid"],
                    point["mean_luma_variance"],
                    point["max_luma_variance"],
                ]
            )


def main() -> int:
    args = parse_args()
    log_path = Path(args.log)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    points = parse_points(log_path)
    if not points:
        raise SystemExit(f"No gpu_convergence_curve samples found in {log_path}")

    csv_path = out_dir / f"{args.name}.csv"
    html_path = out_dir / f"{args.name}.html"

    write_csv(points, csv_path)
    html_path.write_text(build_html(points, f"GPU Convergence Curve ({len(points)} samples)"), encoding="utf-8")

    print(csv_path)
    print(html_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
