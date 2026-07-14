"""Tests for build.py stage selection and validation."""

import importlib.util
import os
import sys
import tempfile
import unittest
from unittest.mock import patch

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, PROJECT_ROOT)
SPEC = importlib.util.spec_from_file_location(
    "build_script", os.path.join(PROJECT_ROOT, "build.py"))
build_script = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = build_script
SPEC.loader.exec_module(build_script)


class ResolveStagesTest(unittest.TestCase):
    def resolve(self, stage_args=None, skip_build=False, cook=False, archive=False,
                framework="macos"):
        return build_script.resolve_stages(stage_args, skip_build, cook, archive, framework)

    def test_default_is_build_and_cook(self):
        with patch.object(build_script, "HOST_COOK_FRAMEWORK", "glfw"):
            self.assertEqual(self.resolve(framework="macos"), ["build", "cook"])
            self.assertEqual(self.resolve(framework="android"), ["build", "cook"])

    def test_default_degrades_to_build_when_the_host_cannot_cook(self):
        with patch.object(build_script, "HOST_COOK_FRAMEWORK", None):
            self.assertEqual(self.resolve(framework="android"), ["build"])
            self.assertEqual(self.resolve(framework="glfw"), ["build", "cook"])

    def test_all_selects_every_stage_in_canonical_order(self):
        self.assertEqual(self.resolve(["all"]), ["build", "cook", "package"])

    def test_explicit_stages_run_in_canonical_order(self):
        self.assertEqual(self.resolve(["package", "build"]), ["build", "package"])

    def test_explicit_stage_never_implies_upstream(self):
        self.assertEqual(self.resolve(["package"]), ["package"])
        self.assertEqual(self.resolve(["cook"]), ["cook"])

    def test_legacy_archive_means_build_and_package(self):
        self.assertEqual(self.resolve(archive=True), ["build", "package"])

    def test_legacy_cook_means_build_and_cook(self):
        self.assertEqual(self.resolve(cook=True), ["build", "cook"])

    def test_legacy_skip_build_cook_means_cook_only(self):
        self.assertEqual(self.resolve(skip_build=True, cook=True), ["cook"])

    def test_legacy_skip_build_alone_selects_nothing(self):
        self.assertEqual(self.resolve(skip_build=True), [])

    def test_explicit_stage_overrides_legacy_flags(self):
        self.assertEqual(self.resolve(["cook"], skip_build=False, archive=True), ["cook"])


class ValidateStagesTest(unittest.TestCase):
    def missing_binary(self):
        return patch.dict(build_script.BINARY_PATH, {"glfw": os.path.join("no", "such", "binary")})

    def existing_binary(self):
        binary = tempfile.NamedTemporaryFile(dir=PROJECT_ROOT, suffix=".bin")
        self.addCleanup(binary.close)
        relative = os.path.relpath(binary.name, PROJECT_ROOT)
        return patch.dict(build_script.BINARY_PATH, {"glfw": relative})

    def setUp(self):
        patcher = patch.object(build_script, "HOST_COOK_FRAMEWORK", "glfw")
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_cooker_framework_delegates_cross_compiled_targets_to_the_host(self):
        self.assertEqual(build_script.cooker_framework("android"), "glfw")
        self.assertEqual(build_script.cooker_framework("ios"), "glfw")
        self.assertEqual(build_script.cooker_framework("macos"), "macos")

    def test_cook_fails_on_a_host_without_a_cooker_framework(self):
        with patch.object(build_script, "HOST_COOK_FRAMEWORK", None):
            with self.assertRaises(RuntimeError):
                build_script.validate_stages(["build", "cook"], "android")

    def test_default_cooked_dir_is_none_when_the_host_cannot_cook(self):
        with patch.object(build_script, "HOST_COOK_FRAMEWORK", None):
            self.assertIsNone(build_script.default_cooked_dir("android"))
        with patch.object(build_script, "HOST_COOK_FRAMEWORK", "glfw"):
            self.assertTrue(build_script.default_cooked_dir("android").endswith("cooked"))

    def test_products_are_named_by_target_except_glfw(self):
        self.assertEqual(build_script.product_name("android", "Release", ".apk"),
                         "android-Release.apk")
        self.assertEqual(build_script.product_name("macos", "Release", ".zip"),
                         "macos-Release.zip")
        self.assertIn("-glfw-Release.zip", build_script.product_name("glfw", "Release", ".zip"))

    def test_cook_for_cross_target_needs_the_host_cooker_binary(self):
        with self.missing_binary():
            with self.assertRaises(RuntimeError):
                build_script.validate_stages(["build", "cook"], "android")
        with self.existing_binary():
            build_script.validate_stages(["build", "cook"], "android")

    def test_cook_without_build_needs_an_existing_binary(self):
        with self.missing_binary():
            with self.assertRaises(RuntimeError):
                build_script.validate_stages(["cook"], "glfw")

    def test_cook_without_build_accepts_an_existing_binary(self):
        with self.existing_binary():
            build_script.validate_stages(["cook"], "glfw")

    def test_cook_with_build_stage_needs_no_binary_for_host_frameworks(self):
        with self.missing_binary():
            build_script.validate_stages(["build", "cook"], "glfw")

    def test_package_alone_is_valid_for_any_framework(self):
        build_script.validate_stages(["package"], "android")


if __name__ == "__main__":
    unittest.main()
