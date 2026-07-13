"""Tests for the repository-wide format gate."""

import importlib.util
import os
import sys
import unittest
from unittest.mock import call, patch

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = importlib.util.spec_from_file_location(
    "check_format", os.path.join(PROJECT_ROOT, "dev", "check_format.py"))
check_format = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = check_format
SPEC.loader.exec_module(check_format)


class GitFilesTest(unittest.TestCase):

    @patch("subprocess.check_output")
    def test_includes_untracked_files_but_excludes_ignored_and_thirdparty(self, check_output):
        check_output.side_effect = [
            "tracked.py\nuntracked.py\ndeleted.py\nthirdparty/vendor.py\n",
            "deleted.py\n",
        ]

        self.assertEqual(check_format.git_files("*.py"), [
            "tracked.py", "untracked.py"])
        self.assertEqual(check_output.call_args_list, [
            call(
                ["git", "ls-files", "--cached", "--others",
                 "--exclude-standard", "--", "*.py"],
                text=True,
            ),
            call(
                ["git", "ls-files", "--deleted", "--", "*.py"],
                text=True,
            ),
        ])


if __name__ == "__main__":
    unittest.main()
