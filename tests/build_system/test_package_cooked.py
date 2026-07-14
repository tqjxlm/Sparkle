"""Tests for the cooked-content packaging module."""

import importlib.util
import os
import sys
import tempfile
import unittest
import zipfile

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = importlib.util.spec_from_file_location(
    "package_cooked", os.path.join(PROJECT_ROOT, "dev", "package_cooked.py"))
package_cooked = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = package_cooked
SPEC.loader.exec_module(package_cooked)


class PackageCookedTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.package = os.path.join(self.directory.name, "product.zip")
        self.image = os.path.join(self.directory.name, "image")

    def make_package(self, prefix="build/packed/", extra_entries=()):
        with zipfile.ZipFile(self.package, "w") as zip_file:
            zip_file.writestr("build/sparkle", "binary")
            zip_file.writestr(prefix + "shaders/forward.spv", "bytecode")
            zip_file.writestr(prefix + "config/cook_list.json", "{}")
            zip_file.writestr(prefix + "models/stale.bin", "raw")
            for name in extra_entries:
                zip_file.writestr(name, "state")

    def make_image(self, extra_files=()):
        os.makedirs(os.path.join(self.image, "cooked", "skylight"))
        os.makedirs(os.path.join(self.image, "config"))
        with open(os.path.join(self.image, "cooked", "manifest.json"), "w") as manifest:
            manifest.write("{}")
        with open(os.path.join(self.image, "cooked", "skylight", "sky.cook"), "w") as artifact:
            artifact.write("payload")
        with open(os.path.join(self.image, "config", "cook_list.json"), "w") as config:
            config.write('{"from": "image"}')
        for relative in extra_files:
            path = os.path.join(self.image, relative)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as extra:
                extra.write("content")

    def entries(self):
        with zipfile.ZipFile(self.package) as zip_file:
            return zip_file.namelist()

    def read_entry(self, name):
        with zipfile.ZipFile(self.package) as zip_file:
            return zip_file.read(name).decode()

    def test_replaces_the_asset_tree_with_the_image(self):
        self.make_package()
        self.make_image()

        count = package_cooked.package_cooked(self.package, "glfw", self.image)

        self.assertEqual(count, 3)
        entries = self.entries()
        self.assertIn("build/packed/cooked/skylight/sky.cook", entries)
        self.assertNotIn("build/packed/models/stale.bin", entries)
        self.assertEqual(self.read_entry("build/packed/config/cook_list.json"),
                         '{"from": "image"}')

    def test_build_owned_shaders_survive_replacement(self):
        self.make_package()
        self.make_image()

        package_cooked.package_cooked(self.package, "glfw", self.image)

        self.assertIn("build/packed/shaders/forward.spv", self.entries())
        self.assertIn("build/sparkle", self.entries())

    def test_replacement_is_idempotent(self):
        self.make_package()
        self.make_image()

        package_cooked.package_cooked(self.package, "glfw", self.image)
        first = sorted(self.entries())
        package_cooked.package_cooked(self.package, "glfw", self.image)

        self.assertEqual(sorted(self.entries()), first)

    def test_rejects_an_image_that_claims_build_owned_subtrees(self):
        self.make_package()
        self.make_image(extra_files=["shaders/rogue.spv"])

        with self.assertRaises(package_cooked.PackagingError):
            package_cooked.package_cooked(self.package, "glfw", self.image)

    def test_strips_internal_runtime_state(self):
        self.make_package(extra_entries=["build/generated/logs/output.log",
                                         "build/generated/cooked/manifest.json"])
        self.make_image()

        package_cooked.package_cooked(self.package, "glfw", self.image)

        self.assertNotIn("build/generated/logs/output.log", self.entries())
        self.assertNotIn("build/generated/cooked/manifest.json", self.entries())

    def test_missing_image_warns_but_keeps_the_raw_package(self):
        self.make_package(extra_entries=["build/generated/logs/output.log"])

        count = package_cooked.package_cooked(self.package, "glfw", self.image)

        self.assertEqual(count, 0)
        self.assertIn("build/packed/models/stale.bin", self.entries())
        self.assertNotIn("build/generated/logs/output.log", self.entries())

    def test_android_dir_manifests_cover_the_final_tree(self):
        self.make_package(prefix="assets/packed/",
                          extra_entries=["assets/_dir_manifest.txt",
                                         "assets/packed/_dir_manifest.txt",
                                         "assets/packed/shaders/_dir_manifest.txt",
                                         "assets/packed/models/_dir_manifest.txt"])
        self.make_image()

        package_cooked.package_cooked(self.package, "android", self.image)

        entries = self.entries()
        self.assertEqual(sorted(entries), sorted(set(entries)))
        self.assertEqual(self.read_entry("assets/_dir_manifest.txt"), "packed\n")
        self.assertEqual(self.read_entry("assets/packed/_dir_manifest.txt"),
                         "config\ncooked\nshaders\n")
        self.assertEqual(self.read_entry("assets/packed/cooked/_dir_manifest.txt"),
                         "skylight\n")
        self.assertNotIn("assets/packed/models/_dir_manifest.txt", entries)
        self.assertNotIn("assets/packed/config/_dir_manifest.txt", entries)

    def test_rejects_an_unexpected_package_layout(self):
        with zipfile.ZipFile(self.package, "w") as zip_file:
            zip_file.writestr("sparkle", "binary")
        self.make_image()

        with self.assertRaises(package_cooked.PackagingError):
            package_cooked.package_cooked(self.package, "glfw", self.image)


if __name__ == "__main__":
    unittest.main()
