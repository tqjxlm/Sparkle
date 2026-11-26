"""Check that the app's last run rendered with the expected pipeline.

A device without hardware ray tracing falls back from the gpu pipeline to
forward and the run still passes, so a case about one pipeline requires the
"effective pipeline: <name>" line RenderConfig logs in the newest app log.
--present and --absent require further lines in, or missing from, that log.
"""

import argparse
import glob
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)
from render_test_support import SUPPORTED_FRAMEWORKS, get_logs_dir  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", required=True,
                        choices=SUPPORTED_FRAMEWORKS)
    parser.add_argument("--pipeline", required=True,
                        help="the pipeline as the log names it, e.g. Gpu")
    parser.add_argument("--present", action="append", default=[],
                        help="a line the log must contain (repeatable)")
    parser.add_argument("--absent", action="append", default=[],
                        help="a line the log must not contain (repeatable)")
    args = parser.parse_args()

    logs = glob.glob(os.path.join(get_logs_dir(args.framework), "*.log"))
    if not logs:
        print(f"FAIL: no app log in {get_logs_dir(args.framework)}", flush=True)
        return 1

    log = max(logs, key=os.path.getmtime)
    marker = f"effective pipeline: {args.pipeline}"
    with open(log, errors="replace") as log_file:
        text = log_file.read()

    if marker not in text:
        print(f"FAIL: {log} lacks '{marker}'; the run rendered with another pipeline", flush=True)
        return 1

    missing = [line for line in args.present if line not in text]
    found = [line for line in args.absent if line in text]
    for line in missing:
        print(f"FAIL: {log} lacks '{line}'", flush=True)
    for line in found:
        print(f"FAIL: {log} has '{line}'", flush=True)
    if missing or found:
        return 1

    print(f"PASS: {log} has '{marker}'", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
