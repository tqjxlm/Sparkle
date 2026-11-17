"""Check the expectations a test case checks while the app shuts down.

The exit code is the verdict a case returns before the app shuts down, so a case
about shutdown checks its expectations (TestCase::Expect) after its verdict, and
only the app log records them. The newest app log must carry the case's passing
verdict, at least one expectation passed after it and none failed after it.
"""

import argparse
import glob
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(SCRIPT_DIR), "rendering"))
from render_test_support import SUPPORTED_FRAMEWORKS, get_logs_dir  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", required=True,
                        choices=SUPPORTED_FRAMEWORKS)
    parser.add_argument("--case", required=True,
                        help="the name the test case is registered with")
    args = parser.parse_args()

    logs = glob.glob(os.path.join(get_logs_dir(args.framework), "*.log"))
    if not logs:
        print(f"FAIL: no app log in {get_logs_dir(args.framework)}", flush=True)
        return 1

    log = max(logs, key=os.path.getmtime)
    with open(log, errors="replace") as log_file:
        text = log_file.read()

    verdict = text.find(f"Test case '{args.case}' passed")
    if verdict < 0:
        print(f"FAIL: {log} lacks the passing verdict of '{args.case}'", flush=True)
        return 1

    shutdown = text[verdict:]
    if f"{args.case}: FAILED - " in shutdown:
        print(f"FAIL: {log} has an expectation of '{args.case}' that failed during shutdown", flush=True)
        return 1
    if f"{args.case}: OK - " not in shutdown:
        print(f"FAIL: {log} has no expectation of '{args.case}' checked during shutdown", flush=True)
        return 1

    print(f"PASS: {log} has the expectations of '{args.case}' passed during shutdown", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
