"""Tests for run.py stage selection and argument forwarding."""

import contextlib
import importlib.util
import io
import os
import sys
import unittest
from unittest.mock import patch

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, PROJECT_ROOT)
SPEC = importlib.util.spec_from_file_location(
    "run_script", os.path.join(PROJECT_ROOT, "run.py"))
run_script = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = run_script
SPEC.loader.exec_module(run_script)

build = run_script.build


class LaunchStagesTest(unittest.TestCase):
    def test_desktop_launch_builds_and_cooks(self):
        with patch.object(build, "HOST_COOK_FRAMEWORK", "glfw"):
            self.assertEqual(run_script.launch_stages("glfw"), ["build", "cook"])
            self.assertEqual(run_script.launch_stages("macos"), ["build", "cook"])

    def test_device_launch_packages_the_product_it_installs(self):
        with patch.object(build, "HOST_COOK_FRAMEWORK", "glfw"):
            self.assertEqual(run_script.launch_stages("android"),
                             ["build", "cook", "package"])
            self.assertEqual(run_script.launch_stages("ios"),
                             ["build", "cook", "package"])

    def test_launch_degrades_when_the_host_cannot_cook(self):
        with patch.object(build, "HOST_COOK_FRAMEWORK", None):
            self.assertEqual(run_script.launch_stages("android"), ["build", "package"])


class ParseRunArgsTest(unittest.TestCase):
    def test_skip_build_is_consumed_and_the_rest_forwarded(self):
        run_args, forwarded = run_script.parse_run_args(
            ["--skip_build", "--framework", "glfw", "--test_case", "smoke"])
        self.assertTrue(run_args.skip_build)
        self.assertEqual(forwarded, ["--framework", "glfw", "--test_case", "smoke"])

    def test_stage_selection_is_rejected(self):
        for stage_arg in (["--stage", "cook"], ["--stage=cook"]):
            with self.assertRaises(SystemExit):
                with contextlib.redirect_stderr(io.StringIO()):
                    run_script.parse_run_args(stage_arg)


if __name__ == "__main__":
    unittest.main()
