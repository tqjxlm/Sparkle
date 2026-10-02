"""Compare a render graph dump against the pipeline's golden graph shape.

The render_graph_dump test case writes the dump; this evaluator projects it to
one line per pass, access, barrier, physical pass, attachment and resource, leaving
out what depends on the device (GPU times, the image backing a transient), and diffs that
against tests/render_graph/golden/<pipeline>.txt. --update rewrites the golden
from the dump instead. Either way it renders the dump as a page through
dev/render_graph_viewer.py to captures/render_graph_<page>.html.
"""

import argparse
import difflib
import json
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, os.path.join(PROJECT_ROOT, "tests", "rendering"))
sys.path.insert(0, os.path.join(PROJECT_ROOT, "dev"))
from render_test_support import SUPPORTED_FRAMEWORKS, get_captures_dir, get_screenshot_dir  # noqa: E402
from render_graph_viewer import (describe_access, describe_attachment, describe_barrier,  # noqa: E402
                                 describe_barrier_after, describe_physical_pass, render_html)

GOLDEN_DIR = os.path.join(SCRIPT_DIR, "golden")
DUMP_NAME = "render_graph.json"


def project(dump):
    """Each pass with its accesses and barriers; after the last member of a physical pass, the physical pass and its
    attachments."""
    lines = []
    passes = dump["passes"]
    for index, graph_pass in enumerate(passes):
        name = graph_pass["name"]
        if graph_pass["culled"]:
            lines.append(f"{name}: culled ({graph_pass['cull_reason']})")
            continue

        step = f", step {graph_pass['step']}" if graph_pass["step"] else ""
        lines.append(f"{name}: {graph_pass['kind']}{step}")
        lines += [f"  {describe_access(access)}" for access in graph_pass["accesses"]]
        lines += [f"  {describe_barrier(barrier)}" for barrier in graph_pass["barriers"]]
        lines += [f"  {describe_barrier_after(barrier)}" for barrier in graph_pass.get("barriers_after", [])]

        physical = dump["physical_passes"][graph_pass["physical_pass"]]
        if physical["members"][-1] == index:
            lines.append(describe_physical_pass(physical, passes))
            lines += [f"  {describe_attachment(attachment)}" for attachment in physical["attachments"]]

    for resource in dump["resources"]:
        line = f"{resource['name']}: {resource['kind']}"
        if resource["kind"] == "Transient":
            line += f" {resource['format']} {resource['size_class']}"
            if "width" in resource:
                line += f" {resource['width']}x{resource['height']}"
            line += f", physical {resource['physical']}" if "physical" in resource else ", no image"
            line += ", memoryless" if resource.get("memoryless") else ""
        if "first_use" in resource:
            first, last = passes[resource["first_use"]]["name"], passes[resource["last_use"]]["name"]
            line += f", {first}..{last}, {resource['usage']}"
        lines.append(line)
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", required=True,
                        choices=SUPPORTED_FRAMEWORKS)
    parser.add_argument("--golden", required=True,
                        help="golden name under tests/render_graph/golden, e.g. cpu")
    parser.add_argument("--page",
                        help="page name, e.g. the registry case; defaults to the golden name")
    parser.add_argument("--update", action="store_true",
                        help="rewrite the golden from the dump")
    args = parser.parse_args()

    dump_path = os.path.join(get_screenshot_dir(args.framework), DUMP_NAME)
    if not os.path.isfile(dump_path):
        print(f"FAIL: render graph dump not found: {dump_path}", flush=True)
        return 1

    with open(dump_path, encoding="utf-8") as dump_file:
        dump = json.load(dump_file)

    page = args.page or args.golden
    page_path = os.path.join(get_captures_dir(args.framework), f"render_graph_{page}.html")
    with open(page_path, "w", encoding="utf-8") as page_file:
        page_file.write(render_html(dump, page))
    print(f"Rendered {page_path}", flush=True)

    actual = project(dump)

    golden_path = os.path.join(GOLDEN_DIR, f"{args.golden}.txt")
    if args.update:
        with open(golden_path, "w", encoding="utf-8") as golden_file:
            golden_file.write("\n".join(actual) + "\n")
        print(f"Updated {golden_path}", flush=True)
        return 0

    with open(golden_path, encoding="utf-8") as golden_file:
        expected = golden_file.read().splitlines()

    if actual == expected:
        print("PASS", flush=True)
        return 0

    print(f"FAIL: render graph shape differs from {golden_path}", flush=True)
    sys.stdout.writelines(difflib.unified_diff(
        [line + "\n" for line in expected], [line + "\n" for line in actual],
        fromfile=f"golden/{args.golden}.txt", tofile=DUMP_NAME))
    return 1


if __name__ == "__main__":
    sys.exit(main())
