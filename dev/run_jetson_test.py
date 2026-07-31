"""Run the jetson test cell on demand, from a clone.

The jetson is a board someone switches on, not a hosted runner, so its suite is
launched rather than triggered. This resolves which CI run's linux package to test —
by default the newest successful one for the current branch — and dispatches
.github/workflows/jetson-test.yml against it.

Resolution happens here rather than in the workflow so the board itself needs no gh
CLI and no credentials of its own; it only ever receives a run id.

    python3 dev/run_jetson_test.py                    # newest successful run, whole coverage
    python3 dev/run_jetson_test.py --case gpu_render_static
    python3 dev/run_jetson_test.py --run-id 12345678 --watch

Requires the gh CLI, authenticated (`gh auth login`).
"""

import argparse
import json
import subprocess
import sys

WORKFLOW = "jetson-test.yml"
CI_WORKFLOW = "ci.yml"


def gh(*args, capture=True):
    """Run a gh command, reporting its own diagnostics rather than a traceback."""
    try:
        result = subprocess.run(["gh", *args], text=True,
                                capture_output=capture, check=False)
    except FileNotFoundError:
        raise SystemExit("gh not found. Install the GitHub CLI and run `gh auth login`.")
    if result.returncode:
        raise SystemExit((result.stderr or result.stdout or "").strip()
                         or f"gh {' '.join(args)} failed ({result.returncode})")
    return (result.stdout or "").strip()


def current_branch():
    return subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"],
                          text=True, capture_output=True, check=True).stdout.strip()


def newest_successful_run(branch):
    """The run whose release package a manual test should use.

    A failed run may still have produced a good package, but defaulting to one would
    make the suite's subject ambiguous; ask for it by id when that is what you want."""
    runs = json.loads(gh("run", "list", "--workflow", CI_WORKFLOW, "--branch", branch,
                         "--status", "success", "--limit", "1",
                         "--json", "databaseId,headSha,createdAt") or "[]")
    if not runs:
        raise SystemExit(
            f"No successful {CI_WORKFLOW} run on '{branch}' to take a package from."
            " Pass --run-id explicitly, or push and let CI build one.")
    run = runs[0]
    print(f"Testing the package from run {run['databaseId']}"
          f" ({run['headSha'][:9]}, {run['createdAt']})")
    return str(run["databaseId"])


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--run-id", help="CI run whose linux package to test"
                        " (default: newest successful run on this branch)")
    parser.add_argument("--case", default="",
                        help="run one registry case instead of the triplet's coverage")
    parser.add_argument("--ref", help="branch to dispatch from (default: current)")
    parser.add_argument("--watch", action="store_true",
                        help="follow the run to completion and exit with its status")
    args = parser.parse_args()

    ref = args.ref or current_branch()
    run_id = args.run_id or newest_successful_run(ref)

    gh("workflow", "run", WORKFLOW, "--ref", ref,
       "-f", f"run_id={run_id}", "-f", f"case={args.case}")
    print(f"Dispatched {WORKFLOW} on {ref}.")

    if not args.watch:
        print(f"Follow it with: gh run watch --workflow {WORKFLOW}")
        return 0

    # the dispatch returns before the run is queryable, so let gh find the newest one
    gh("run", "watch", "--workflow", WORKFLOW, "--exit-status", capture=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
