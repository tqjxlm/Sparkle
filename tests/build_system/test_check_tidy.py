"""Tests for the clang-tidy validation gate."""

import importlib.util
import os
import sys
import unittest
from unittest.mock import patch

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = importlib.util.spec_from_file_location(
    "check_tidy", os.path.join(PROJECT_ROOT, "dev", "check_tidy.py"))
check_tidy = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = check_tidy
SPEC.loader.exec_module(check_tidy)


class CompilerArgsTest(unittest.TestCase):

    @patch.object(check_tidy.sys, "platform", "darwin")
    @patch("subprocess.check_output", return_value="/Xcode/MacOSX.sdk\n")
    def test_macos_supplies_the_sdk_to_the_pip_clang(self, check_output):
        self.assertEqual(check_tidy.compiler_args(), [
            "--extra-arg-before=-isysroot",
            "--extra-arg-before=/Xcode/MacOSX.sdk",
        ])
        check_output.assert_called_once_with(
            ["xcrun", "--show-sdk-path"], text=True)

    @patch.object(check_tidy.sys, "platform", "linux")
    def test_other_platforms_need_no_compiler_args(self):
        self.assertEqual(check_tidy.compiler_args(), [])


if __name__ == "__main__":
    unittest.main()
