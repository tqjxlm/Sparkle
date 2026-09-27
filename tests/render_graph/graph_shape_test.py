"""Compare a render graph dump against the pipeline's golden graph shape.

The render_graph_dump test case writes the dump; this evaluator projects it to
one line per pass, access, barrier, attachment and resource, and diffs that
against tests/render_graph/golden/<pipeline>.txt. --update rewrites the golden
from the dump instead.
"""

import argparse
import difflib
import json
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
sys.path.insert(0, os.path.join(PROJECT_ROOT, "tests", "rendering"))
from render_test_support import SUPPORTED_FRAMEWORKS, get_screenshot_dir  # noqa: E402

GOLDEN_DIR = os.path.join(SCRIPT_DIR, "golden")
DUMP_NAME = "render_graph.json"


def project(dump):
    lines = []
    for graph_pass in dump["passes"]:
        name = graph_pass["name"]
        if graph_pass["culled"]:
            lines.append(f"{name}: culled ({graph_pass['cull_reason']})")
            continue

        lines.append(f"{name}: {graph_pass['kind']}")
        for access in graph_pass["accesses"]:
            clear = " clear" if access.get("clear") else ""
            lines.append(
                f"  access {access['resource']} {access['access']}{clear}")
        for barrier in graph_pass["barriers"]:
            lines.append(f"  barrier {barrier['resource']} {barrier['from_layout']}->{barrier['to_layout']}"
                         f" [{barrier['from']} -> {barrier['to']}]")
        for attachment in graph_pass["attachments"]:
            lines.append(f"  attachment {attachment['resource']} slot {attachment['slot']}:"
                         f" {attachment['load']} ({attachment['load_reason']})"
                         f" / {attachment['store']} ({attachment['store_reason']})")

    for resource in dump["resources"]:
        line = f"{resource['name']}: {resource['kind']}"
        if resource["kind"] == "Transient":
            line += f" {resource['format']} {resource['size_class']}"
            if "width" in resource:
                line += f" {resource['width']}x{resource['height']}"
            line += f", physical {resource['physical']}" if "physical" in resource else ", no image"
        if "first_use" in resource:
            line += f", {resource['first_use']}..{resource['last_use']}, {resource['usage']}"
        lines.append(line)
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", required=True,
                        choices=SUPPORTED_FRAMEWORKS)
    parser.add_argument("--golden", required=True,
                        help="golden name under tests/render_graph/golden, e.g. cpu")
    parser.add_argument("--update", action="store_true",
                        help="rewrite the golden from the dump")
    args = parser.parse_args()

    dump_path = os.path.join(get_screenshot_dir(args.framework), DUMP_NAME)
    if not os.path.isfile(dump_path):
        print(f"FAIL: render graph dump not found: {dump_path}", flush=True)
        return 1

    with open(dump_path, encoding="utf-8") as dump_file:
        actual = project(json.load(dump_file))

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
