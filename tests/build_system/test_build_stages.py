"""Tests for build.py stage selection and validation."""

import importlib.util
import os
import sys
import tempfile
import unittest
import zipfile
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

    def test_default_is_build_and_cook_for_desktop(self):
        with patch.object(build_script, "HOST_COOK_FRAMEWORK", "glfw"):
            self.assertEqual(self.resolve(framework="macos"), ["build", "cook"])
            self.assertEqual(self.resolve(framework="glfw"), ["build", "cook"])

    def test_default_includes_package_for_device_frameworks(self):
        with patch.object(build_script, "HOST_COOK_FRAMEWORK", "glfw"):
            self.assertEqual(self.resolve(framework="android"), ["build", "cook", "package"])
            self.assertEqual(self.resolve(framework="ios"), ["build", "cook", "package"])

    def test_default_degrades_to_build_when_the_host_cannot_cook(self):
        with patch.object(build_script, "HOST_COOK_FRAMEWORK", None):
            self.assertEqual(self.resolve(framework="android"), ["build", "package"])
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

    def test_default_image_dir_is_none_when_the_host_cannot_cook(self):
        with patch.object(build_script, "HOST_COOK_FRAMEWORK", None):
            self.assertIsNone(build_script.default_cooked_image_dir("android"))
        with patch.object(build_script, "HOST_COOK_FRAMEWORK", "glfw"):
            self.assertTrue(build_script.default_cooked_image_dir("android")
                            .endswith("cooked_image"))

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


class SetupTest(unittest.TestCase):
    def run_setup(self, stages, generate_only=False, configure_only=False, clangd=False):
        args = {"stages": stages, "generate_only": generate_only,
                "configure_only": configure_only, "clangd": clangd}
        with patch.object(build_script, "run_git_submodule_update") as submodules, \
                patch("resources.setup.setup_resources") as resources, \
                patch("ide.setup.setup_ide") as ide:
            build_script.setup(args)
        return {"submodules": submodules.called, "resources": resources.called,
                "ide": ide.called}

    def test_build_stage_runs_every_setup_step(self):
        self.assertEqual(self.run_setup(["build", "cook"]),
                         {"submodules": True, "resources": True, "ide": True})

    def test_cook_only_needs_resources_but_no_submodules(self):
        self.assertEqual(self.run_setup(["cook"]),
                         {"submodules": False, "resources": True, "ide": False})

    def test_package_only_skips_all_setup(self):
        self.assertEqual(self.run_setup(["package"]),
                         {"submodules": False, "resources": False, "ide": False})

    def test_run_without_stages_skips_all_setup(self):
        self.assertEqual(self.run_setup([]),
                         {"submodules": False, "resources": False, "ide": False})

    def test_configure_modes_set_up_like_a_build(self):
        for mode in ("generate_only", "configure_only", "clangd"):
            with self.subTest(mode=mode):
                self.assertEqual(self.run_setup([], **{mode: True}),
                                 {"submodules": True, "resources": True, "ide": True})


class PackageProjectTest(unittest.TestCase):
    class FakeBuilder:
        def __init__(self):
            self.resigned = []

        def archive(self, args):
            return "unused"

        def resign_package(self, package_path):
            self.resigned.append(package_path)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.product = os.path.join(self.directory.name, "android-Release.apk")
        self.image = os.path.join(self.directory.name, "image")
        self.builder = self.FakeBuilder()

        with zipfile.ZipFile(self.product, "w") as zip_file:
            zip_file.writestr("assets/packed/config/cook_list.json", "{}")

    def package(self, image_dir):
        args = {"framework": "android", "config": "Release", "cooked": image_dir}
        with patch.object(build_script, "copy_build_products", return_value=self.product):
            build_script.package_project(self.builder, args)
        return args

    def test_packaged_content_triggers_resign_and_records_the_product(self):
        os.makedirs(os.path.join(self.image, "cooked"))
        with open(os.path.join(self.image, "cooked", "manifest.json"), "w") as manifest:
            manifest.write("{}")

        args = self.package(self.image)

        self.assertEqual(self.builder.resigned, [self.product])
        self.assertEqual(args["product_path"], self.product)

    def test_no_cooked_content_skips_resign_but_records_the_product(self):
        args = self.package(self.image)

        self.assertEqual(self.builder.resigned, [])
        self.assertEqual(args["product_path"], self.product)


if __name__ == "__main__":
    unittest.main()
