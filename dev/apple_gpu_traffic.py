"""Measure the GPU memory traffic of each render pass of the macOS app with Instruments.

Records the app under dev/instruments/SparklePerformanceLimiters.tracetemplate (Metal System Trace with the Metal
Application instrument's "Performance Limiters" counter set), then integrates the GPU read and write bandwidth counters
over the GPU execution intervals of the app's own encoders and reports bytes per frame for each encoder label (a render
graph physical pass) and their total. GPU counters are device-wide, so the report also states how much of the app's GPU
time other processes' GPU work overlapped; no other sparkle instance may run during a recording.

    python3 dev/apple_gpu_traffic.py [--runs 3] [--seconds 6] [--window 3] [--output DIR] -- --pipeline deferred ...

Arguments after `--` go to the app (`--headless true` is added). Needs a macOS build and Xcode's xctrace.
"""

import argparse
import bisect
import os
import signal
import statistics
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from collections import defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(REPO, "build_system", "macos", "output", "build", "sparkle.app", "Contents", "MacOS", "sparkle")
TEMPLATE = os.path.join(REPO, "dev", "instruments", "SparklePerformanceLimiters.tracetemplate")
COUNTERS = {"GPU Read Bandwidth": "read", "GPU Write Bandwidth": "write"}


def app_pids():
    result = subprocess.run(["pgrep", "-f", APP], capture_output=True, text=True)
    return {int(pid) for pid in result.stdout.split()}


def quit_apps(pids):
    for pid in pids:
        os.kill(pid, signal.SIGTERM)
    deadline = time.monotonic() + 10
    while pids & app_pids() and time.monotonic() < deadline:
        time.sleep(0.2)
    for pid in pids & app_pids():
        os.kill(pid, signal.SIGKILL)


def record(trace, seconds, window, app_args):
    if app_pids():
        sys.exit("another sparkle instance is running; its GPU work would count in the device-wide counters")
    command = ["xcrun", "xctrace", "record", "--no-prompt", "--template", TEMPLATE, "--time-limit", f"{seconds}s",
               "--window", f"{window}s", "--output", trace, "--launch", "--", APP, "--headless", "true", *app_args]
    try:
        subprocess.run(command, capture_output=True, check=False)
    finally:
        # xctrace stops recording at the time limit but leaves the launched app running
        quit_apps(app_pids())
    if not os.path.isdir(trace):
        sys.exit(f"xctrace wrote no trace to {trace}")


def export(trace, schema, path):
    with open(path, "w", encoding="utf-8") as out:
        subprocess.run(["xcrun", "xctrace", "export", "--input", trace, "--xpath",
                        f'/trace-toc/run[@number="1"]/data/table[@schema="{schema}"]'], stdout=out, check=True)


def rows(path):
    """The rows of an exported table as {column: element}, with references to earlier elements resolved."""
    columns, elements = [], {}
    for _, element in ET.iterparse(path, events=("end",)):
        if element.tag == "mnemonic":
            columns.append(element.text)
        if "id" in element.attrib:
            elements[element.attrib["id"]] = element
        if element.tag == "row":
            yield {column: elements.get(cell.attrib["ref"]) if "ref" in cell.attrib else cell
                   for column, cell in zip(columns, element)}


def number(element):
    return float(element.text)


def label(element):
    return element.attrib.get("fmt") or element.text or ""


def merged(intervals):
    """The union of (start, end) intervals as disjoint sorted intervals."""
    union = []
    for start, end in sorted(intervals):
        if union and start <= union[-1][1]:
            union[-1][1] = max(union[-1][1], end)
        else:
            union.append([start, end])
    return union


def overlap(intervals, start, end):
    """The total time the disjoint sorted intervals overlap [start, end)."""
    total = 0.0
    for first, last in intervals:
        if first >= end:
            break
        total += max(0.0, min(last, end) - max(first, start))
    return total


def export_tables(trace, work):
    paths = {schema: os.path.join(work, f"{schema}.xml")
             for schema in ("metal-application-encoders-list", "metal-gpu-intervals", "metal-gpu-counter-intervals")}
    for schema, path in paths.items():
        export(trace, schema, path)
    return paths


def measure(paths):
    """Bytes read and written, GPU time and the time other processes' GPU work overlapped, per encoder label, over the
    exported tables; and the number of frames."""
    labels = {}
    for row in rows(paths["metal-application-encoders-list"]):
        if label(row["process"]).startswith("sparkle ("):
            labels[label(row["encoder-id"])] = label(row["encoder-label"])

    # GPU time per encoder (vertex and fragment channels overlap, so they are merged) and per other process
    ours, frames, theirs = defaultdict(list), {}, []
    for row in rows(paths["metal-gpu-intervals"]):
        start = number(row["start"])
        interval = (start, start + number(row["duration"]))
        encoder = label(row["encoder-id"])
        if encoder in labels:
            ours[encoder].append(interval)
            frames[encoder] = label(row["frame-number"])
        else:
            theirs.append(interval)
    ours = {encoder: merged(intervals) for encoder, intervals in ours.items()}

    per_label = defaultdict(lambda: {"read": 0.0, "write": 0.0, "read_ns": 0.0, "write_ns": 0.0, "gpu_ns": 0.0,
                                     "shared_ns": 0.0})
    others = merged(theirs)
    for encoder, intervals in ours.items():
        totals = per_label[labels[encoder]]
        for start, end in intervals:
            totals["gpu_ns"] += end - start
            totals["shared_ns"] += overlap(others[max(0, bisect.bisect_left(others, [start]) - 1):], start, end)

    # a sample averages the bandwidth over tens of microseconds, about a pass's length, so its bytes go to the GPU work
    # that ran in it, in proportion to the time each ran
    work = sorted([(start, end, labels[encoder]) for encoder, intervals in ours.items() for start, end in intervals] +
                  [(start, end, None) for start, end in theirs])
    work_starts = [start for start, _, _ in work]
    longest = max((end - start for start, end, _ in work), default=0.0)
    for row in rows(paths["metal-gpu-counter-intervals"]):
        kind = COUNTERS.get(label(row["name"]))
        if not kind:
            continue
        first = number(row["start"])
        last = first + number(row["duration"])
        candidates = work[bisect.bisect_left(work_starts, first - longest):bisect.bisect_left(work_starts, last)]
        shares = [(owner, min(end, last) - max(start, first)) for start, end, owner in candidates
                  if end > first and start < last]
        busy = sum(share for _, share in shares)
        # GB/s over ns gives bytes
        sample_bytes = number(row["value"]) * (last - first)
        for owner, share in shares:
            if owner:
                per_label[owner][kind] += sample_bytes * share / busy
                per_label[owner][f"{kind}_ns"] += share

    # the GPU samples its counter groups in turn, so the bandwidth counters cover part of each pass's GPU time: scale
    # the bytes up to the whole of it
    for totals in per_label.values():
        for kind in COUNTERS.values():
            if totals[f"{kind}_ns"] > 0:
                totals[kind] *= totals["gpu_ns"] / totals[f"{kind}_ns"]
    return per_label, len(set(frames.values()))


def per_frame(per_label, frames):
    """MB read and written and GPU ms per frame of each encoder label, and their total."""
    if not frames:
        sys.exit("the trace holds no GPU work of the app")
    table = {name: {"read": totals["read"] / frames / 1e6, "write": totals["write"] / frames / 1e6,
                    "gpu": totals["gpu_ns"] / frames / 1e6} for name, totals in per_label.items()}
    table["total"] = {key: sum(row[key] for row in table.values()) for key in ("read", "write", "gpu")}
    return table


def report(runs):
    tables = [per_frame(per_label, frames) for per_label, frames in runs]
    for index, ((per_label, frames), table) in enumerate(zip(runs, tables)):
        gpu_ns = sum(totals["gpu_ns"] for totals in per_label.values())
        shared = sum(totals["shared_ns"] for totals in per_label.values()) / gpu_ns
        sampled = sum(totals["write_ns"] for totals in per_label.values()) / gpu_ns
        print(f"run {index + 1}: {frames} frames, read {table['total']['read']:.3f} MB/frame, write"
              f" {table['total']['write']:.3f} MB/frame; other processes' GPU work overlapped {shared:.1%} of the"
              f" app's GPU time, the bandwidth counters sampled {sampled:.1%} of it")

    def median(name, key):
        return statistics.median(table[name][key] for table in tables if name in table)

    names = sorted(set().union(*tables) - {"total"}, key=lambda name: -median(name, "write")) + ["total"]
    print(f"median of {len(runs)} runs")
    print(f"{'encoder':40s} {'read MB/frame':>14s} {'write MB/frame':>15s} {'GPU ms/frame':>13s}")
    for name in names:
        print(f"{name:40s} {median(name, 'read'):14.3f} {median(name, 'write'):15.3f} {median(name, 'gpu'):13.3f}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--seconds", type=int, default=6, help="recording length, including startup")
    parser.add_argument("--window", type=int, default=3, help="seconds at the end of the recording that are kept")
    parser.add_argument("--runs", type=int, default=3,
                        help="recordings whose median is reported; single recordings vary by about 20%%")
    parser.add_argument("--output", default=os.path.join(REPO, "build_system", "macos", "output", "gpu_traffic"),
                        help="directory for the trace and its exported tables")
    parser.add_argument("app_args", nargs="*", help="arguments for the app, after --")
    args = parser.parse_args()

    runs = []
    for run in range(args.runs):
        work = os.path.join(args.output, f"run{run + 1}")
        os.makedirs(work, exist_ok=True)
        trace = os.path.join(work, "recording.trace")
        subprocess.run(["rm", "-rf", trace], check=True)
        record(trace, args.seconds, args.window, args.app_args)
        runs.append(measure(export_tables(trace, work)))
    report(runs)


if __name__ == "__main__":
    main()
