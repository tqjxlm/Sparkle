"""Tests for the repository test-suite orchestrator."""

import importlib.util
import os
import sys
import tempfile
import unittest
from unittest.mock import patch

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = importlib.util.spec_from_file_location(
    "run_tests", os.path.join(PROJECT_ROOT, "dev", "run_tests.py"))
run_tests = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = run_tests
SPEC.loader.exec_module(run_tests)


class RunTestsCommandTest(unittest.TestCase):

    def test_python_unit_tests_are_a_preflight_gate(self):
        self.assertEqual(run_tests.preflight_steps(), [
            (
                "python unit tests",
                [sys.executable, "-m", "unittest", "discover",
                 "-s", "tests/build_system", "-p", "test_*.py"],
            ),
        ])

    def test_hardware_run_uses_build_entry_point(self):
        command = run_tests.app_command(
            framework="macos",
            config="Release",
            software=False,
            test_case="screenshot",
            test_args=["--pipeline", "forward"],
        )

        self.assertEqual(command[:6], [
            sys.executable,
            os.path.join(PROJECT_ROOT, "build.py"),
            "--framework",
            "macos",
            "--config",
            "Release",
        ])
        self.assertIn("--skip_build", command)
        self.assertIn("--run", command)
        self.assertEqual(command[-4:], [
            "--test_case", "screenshot", "--pipeline", "forward"])

    def test_software_run_uses_lavapipe_wrapper(self):
        command = run_tests.app_command(
            framework="glfw",
            config="Release",
            software=True,
            test_case="cooker_request",
            test_args=["--headless", "true"],
        )

        self.assertEqual(command, [
            sys.executable,
            os.path.join(PROJECT_ROOT, "dev", "run_without_gpu.py"),
            "--test_case",
            "cooker_request",
            "--headless",
            "true",
        ])

    def test_suite_uses_test_owned_static_render_comparator(self):
        steps = run_tests.test_steps(
            framework="glfw",
            config="Release",
            software=True,
            headless=True,
            pipelines=["forward", "deferred"],
            scene=None,
            other_args=[],
            test_python="test-python",
            require_cooked=False,
        )

        commands = [command for _, command in steps]
        flattened = "\n".join(" ".join(command) for command in commands)
        self.assertIn("tests/screenshot/static_render_test.py", flattened)
        self.assertNotIn("tests/screenshot/screenshot_test.py", flattened)
        self.assertNotIn("functional_test.py", flattened)

    def test_suite_checks_scene_load_failure_propagation(self):
        steps = run_tests.test_steps(
            framework="macos",
            config="Release",
            software=False,
            headless=True,
            pipelines=["forward"],
            scene=None,
            other_args=[],
            test_python="test-python",
            require_cooked=False,
        )

        self.assertIn("scene load failure", dict(steps))

    def test_custom_scene_path_is_separate_from_its_ground_truth_name(self):
        scene = os.path.join("resources", "custom", "Atrium.usda")
        steps = run_tests.test_steps(
            framework="macos",
            config="Release",
            software=False,
            headless=True,
            pipelines=["forward"],
            scene=scene,
            other_args=[],
            test_python="test-python",
            require_cooked=False,
        )
        commands = dict(steps)

        self.assertIn(scene, commands["screenshot (forward)"])
        self.assertEqual(commands["compare (forward)"][-2:], [
            "--scene", "Atrium"])
        self.assertIn(scene, commands["usd round trip"])
        self.assertEqual(commands["compare (round trip)"][-2:], [
            "--scene", scene])

    def test_cook_gate_ignores_log_content_from_before_the_suite(self):
        with tempfile.TemporaryDirectory() as directory:
            log = os.path.join(directory, "sparkle.log")
            with open(log, "w") as log_file:
                log_file.write("] cooking stale artifact\n")
            previous_sizes = {log: os.path.getsize(log)}
            with open(log, "a") as log_file:
                log_file.write("cook artifact hit\n")

            with patch.object(run_tests, "log_pattern",
                              return_value=os.path.join(directory, "*.log")):
                passed, detail = run_tests.cook_gate("glfw", previous_sizes)

        self.assertTrue(passed)
        self.assertIn("1 cook artifact hits", detail)

    def test_cook_gate_rejects_runtime_cooking(self):
        with tempfile.TemporaryDirectory() as directory:
            log = os.path.join(directory, "sparkle.log")
            with open(log, "w") as log_file:
                log_file.write("initial log\n")
            previous_sizes = {log: os.path.getsize(log)}
            with open(log, "a") as log_file:
                log_file.write("] cooking new artifact\n")

            with patch.object(run_tests, "log_pattern",
                              return_value=os.path.join(directory, "*.log")):
                passed, detail = run_tests.cook_gate("glfw", previous_sizes)

        self.assertFalse(passed)
        self.assertIn("1 on-the-fly cook", detail)

    def test_cook_gate_exempts_cooker_request_fixtures(self):
        with tempfile.TemporaryDirectory() as directory:
            log = os.path.join(directory, "sparkle.log")
            with open(log, "w") as log_file:
                log_file.write("initial log\n")
            previous_sizes = {log: os.path.getsize(log)}
            with open(log, "a") as log_file:
                log_file.write(
                    "] cooking cooker_request_test_dup: assets/duplicate/contract.bin\n")
                log_file.write("cook artifact hit\n")

            with patch.object(run_tests, "log_pattern",
                              return_value=os.path.join(directory, "*.log")):
                passed, _ = run_tests.cook_gate("glfw", previous_sizes)

        self.assertTrue(passed)

    def test_ibl_parity_runs_only_on_a_physical_gpu_framework(self):
        def suite(framework, software, require_cooked=False):
            return dict(run_tests.test_steps(
                framework=framework,
                config="Release",
                software=software,
                headless=True,
                pipelines=["forward"],
                scene=None,
                other_args=[],
                test_python="test-python",
                require_cooked=require_cooked,
            ))

        self.assertIn("ibl parity", suite("macos", software=False))
        self.assertNotIn("ibl parity", suite("glfw", software=True))
        self.assertNotIn("ibl parity", suite("glfw", software=False))
        # its deliberate artifact recook would trip the --require_cooked gate
        self.assertNotIn("ibl parity", suite(
            "macos", software=False, require_cooked=True))


if __name__ == "__main__":
    unittest.main()
