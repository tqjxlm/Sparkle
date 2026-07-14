"""Tests for the cooked-content packaging module."""

import importlib.util
import os
import sys
import tempfile
import unittest
import zipfile

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = importlib.util.spec_from_file_location(
    "inject_cooked", os.path.join(PROJECT_ROOT, "dev", "inject_cooked.py"))
inject_cooked = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = inject_cooked
SPEC.loader.exec_module(inject_cooked)


class PackageCookedTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.package = os.path.join(self.directory.name, "product.zip")
        self.cooked = os.path.join(self.directory.name, "cooked")

    def make_package(self, extra_entries=()):
        with zipfile.ZipFile(self.package, "w") as zip_file:
            zip_file.writestr("build/sparkle", "binary")
            zip_file.writestr("build/packed/config/cook_list.json", "{}")
            for name in extra_entries:
                zip_file.writestr(name, "state")

    def make_cooked(self):
        os.makedirs(os.path.join(self.cooked, "skylight"), exist_ok=True)
        with open(os.path.join(self.cooked, "manifest.json"), "w") as manifest:
            manifest.write("{}")
        with open(os.path.join(self.cooked, "skylight", "sky.cook"), "w") as artifact:
            artifact.write("payload")

    def entries(self):
        with zipfile.ZipFile(self.package) as zip_file:
            return zip_file.namelist()

    def test_injects_at_the_platform_resource_root_with_posix_names(self):
        self.make_package()
        self.make_cooked()

        count = inject_cooked.package_cooked(self.package, "glfw", self.cooked)

        self.assertEqual(count, 2)
        self.assertIn("build/packed/cooked/skylight/sky.cook", self.entries())

    def test_strips_internal_runtime_state(self):
        self.make_package(extra_entries=["build/generated/logs/output.log",
                                         "build/generated/cooked/manifest.json"])
        self.make_cooked()

        inject_cooked.package_cooked(self.package, "glfw", self.cooked)

        self.assertNotIn("build/generated/logs/output.log", self.entries())
        self.assertNotIn("build/generated/cooked/manifest.json", self.entries())
        self.assertIn("build/sparkle", self.entries())

    def test_refuses_to_double_inject(self):
        self.make_package(extra_entries=["build/packed/cooked/manifest.json"])
        self.make_cooked()

        with self.assertRaises(inject_cooked.PackagingError):
            inject_cooked.package_cooked(self.package, "glfw", self.cooked)

    def test_missing_cooked_content_warns_but_packages(self):
        self.make_package()

        count = inject_cooked.package_cooked(self.package, "glfw", self.cooked)

        self.assertEqual(count, 0)
        self.assertIn("build/sparkle", self.entries())


if __name__ == "__main__":
    unittest.main()
