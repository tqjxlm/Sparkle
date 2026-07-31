"""Tests for run.py stage selection and argument forwarding."""

import contextlib
import importlib.util
import io
import os
import sys
import unittest

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, PROJECT_ROOT)
SPEC = importlib.util.spec_from_file_location(
    "run_script", os.path.join(PROJECT_ROOT, "run.py"))
run_script = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = run_script
SPEC.loader.exec_module(run_script)


class LaunchStagesTest(unittest.TestCase):
    def test_desktop_launch_builds_and_cooks(self):
        self.assertEqual(run_script.launch_stages("glfw"), ["build", "cook"])
        self.assertEqual(run_script.launch_stages("macos"), ["build", "cook"])

    def test_device_launch_packages_the_product_it_installs(self):
        self.assertEqual(run_script.launch_stages("android"),
                         ["build", "cook", "package"])
        self.assertEqual(run_script.launch_stages("ios"),
                         ["build", "cook", "package"])


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


class StagelessEnvironmentTest(unittest.TestCase):
    """A launch that runs no stage must need no build toolchain.

    This is what lets a machine that only runs the suite — the jetson cell — be a
    runner rather than a builder: it downloads a package and launches it."""

    def setUp(self):
        import build
        self.build = build
        self.reached = []
        self.original = {name: getattr(build, name) for name in
                         ("find_cmake", "find_slangc", "find_ispc",
                          "find_and_set_vulkan_sdk")}
        for name in self.original:
            setattr(build, name, self._probe(name))

    def tearDown(self):
        for name, function in self.original.items():
            setattr(self.build, name, function)

    def _probe(self, name):
        def probe(*_args, **_kwargs):
            self.reached.append(name)
            if name == "find_and_set_vulkan_sdk":
                raise RuntimeError("no SDK here")
            return f"/absent/{name}"
        return probe

    def test_a_launch_without_stages_reaches_for_no_build_tool(self):
        args = {"framework": "glfw", "stages": [], "config": "Release"}
        with contextlib.redirect_stdout(io.StringIO()):
            self.build.check_environment(args)
        self.assertEqual(self.reached, ["find_and_set_vulkan_sdk"])
        self.assertNotIn("cmake_executable", args)

    def test_a_build_still_requires_its_toolchain(self):
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(RuntimeError):
                self.build.check_environment(
                    {"framework": "glfw", "stages": ["build"], "config": "Release"})
        self.assertIn("find_cmake", self.reached)
