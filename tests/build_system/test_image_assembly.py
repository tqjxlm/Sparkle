"""Tests for the per-target cooked content image assembly in build.py."""

import importlib.util
import json
import os
import sys
import tempfile
import unittest

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, PROJECT_ROOT)
SPEC = importlib.util.spec_from_file_location(
    "build_script", os.path.join(PROJECT_ROOT, "build.py"))
build_script = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = build_script
SPEC.loader.exec_module(build_script)

SOURCE = "textures/road.png"
KEY_ASTC = "texture_astc:textures/road.png#color"
ARTIFACT_ASTC = "cooked/texture_astc/road_00000000.cook"
KEY_BC = "texture_bc:textures/road.png#color"
ARTIFACT_BC = "cooked/texture_bc/road_00000000.cook"
KEY_UNIVERSAL = "brdf_lut:brdf"
ARTIFACT_UNIVERSAL = "cooked/brdf_lut/brdf_00000000.cook"

PRODUCTS = {
    "android": {"artifacts": [KEY_ASTC, KEY_UNIVERSAL],
                "consumed_sources": {SOURCE: [KEY_ASTC]}},
    "linux-glfw": {"artifacts": [KEY_BC, KEY_UNIVERSAL],
                   "consumed_sources": {SOURCE: [KEY_BC]}},
}


class AssembleCookedImageTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = self.directory.name

        self.original_scriptpath = build_script.SCRIPTPATH
        build_script.SCRIPTPATH = self.root
        self.addCleanup(setattr, build_script, "SCRIPTPATH", self.original_scriptpath)

        self.pool_dir = os.path.join(self.root, build_script.COOKED_OUTPUT_DIR["glfw"])
        self.pool_root = os.path.dirname(self.pool_dir)
        os.makedirs(self.pool_dir)

        self.add_file(os.path.join(self.root, "resources", "packed", SOURCE))
        self.add_file(os.path.join(self.root, "resources", "packed", "config", "cook_list.json"))

    def add_file(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as content:
            content.write(b"x")
        return path

    def write_pool(self, manifest, products=PRODUCTS, payloads=(ARTIFACT_ASTC, ARTIFACT_BC,
                                                                ARTIFACT_UNIVERSAL)):
        with open(os.path.join(self.pool_dir, "manifest.json"), "w", encoding="utf-8") as out:
            json.dump(manifest, out)
        with open(os.path.join(self.pool_dir, "cook_products.json"), "w", encoding="utf-8") as out:
            json.dump(products, out)
        for payload in payloads:
            self.add_file(os.path.join(self.pool_root, payload))

    def full_manifest(self):
        return {KEY_ASTC: {"artifact": ARTIFACT_ASTC},
                KEY_BC: {"artifact": ARTIFACT_BC},
                KEY_UNIVERSAL: {"artifact": ARTIFACT_UNIVERSAL}}

    def image_dir(self, target):
        return os.path.join(self.root, build_script.COOKED_IMAGE_DIR["glfw"], target)

    def image_manifest(self, target):
        path = os.path.join(self.image_dir(target), "cooked", "manifest.json")
        with open(path, encoding="utf-8") as manifest_file:
            return json.load(manifest_file)

    def test_projects_only_the_targets_plan(self):
        self.write_pool(self.full_manifest())

        build_script.assemble_cooked_image("glfw", "android")

        image = self.image_dir("android")
        self.assertEqual(set(self.image_manifest("android")), {KEY_ASTC, KEY_UNIVERSAL})
        self.assertTrue(os.path.isfile(os.path.join(image, ARTIFACT_ASTC)))
        self.assertTrue(os.path.isfile(os.path.join(image, ARTIFACT_UNIVERSAL)))
        self.assertFalse(os.path.exists(os.path.join(image, ARTIFACT_BC)))
        self.assertFalse(os.path.exists(os.path.join(image, SOURCE)))
        self.assertTrue(os.path.isfile(os.path.join(image, "config", "cook_list.json")))

        marker_path = os.path.join(image, "cooked", "content_target.json")
        with open(marker_path, encoding="utf-8") as marker:
            self.assertEqual(json.load(marker), {"target": "android"})

    def test_assembles_an_image_per_target(self):
        self.write_pool(self.full_manifest())

        build_script.assemble_cooked_image("glfw", "android")
        build_script.assemble_cooked_image("glfw", "linux-glfw")

        linux_image = self.image_dir("linux-glfw")
        self.assertTrue(os.path.isfile(os.path.join(linux_image, ARTIFACT_BC)))
        self.assertFalse(os.path.exists(os.path.join(linux_image, ARTIFACT_ASTC)))
        self.assertTrue(os.path.isfile(os.path.join(self.image_dir("android"), ARTIFACT_ASTC)))

    def test_source_ships_when_replacement_keys_are_not_projected(self):
        products = {"android": {"artifacts": [KEY_UNIVERSAL],
                                "consumed_sources": {SOURCE: [KEY_ASTC]}}}
        self.write_pool(self.full_manifest(), products=products)

        build_script.assemble_cooked_image("glfw", "android")

        self.assertTrue(os.path.isfile(os.path.join(self.image_dir("android"), SOURCE)))

    def test_missing_manifest_entry_fails(self):
        self.write_pool({KEY_UNIVERSAL: {"artifact": ARTIFACT_UNIVERSAL}})

        with self.assertRaises(RuntimeError):
            build_script.assemble_cooked_image("glfw", "android")

    def test_missing_artifact_payload_fails(self):
        self.write_pool(self.full_manifest(), payloads=(ARTIFACT_BC, ARTIFACT_UNIVERSAL))

        with self.assertRaises(RuntimeError):
            build_script.assemble_cooked_image("glfw", "android")

    def test_escaping_artifact_path_fails(self):
        manifest = self.full_manifest()
        manifest[KEY_ASTC] = {"artifact": "../escape.cook"}
        self.write_pool(manifest)
        self.add_file(os.path.join(os.path.dirname(self.pool_root), "escape.cook"))

        with self.assertRaises(RuntimeError):
            build_script.assemble_cooked_image("glfw", "android")

    def test_absolute_artifact_path_fails(self):
        manifest = self.full_manifest()
        manifest[KEY_ASTC] = {"artifact": self.add_file(os.path.join(self.root, "abs.cook"))}
        self.write_pool(manifest)

        with self.assertRaises(RuntimeError):
            build_script.assemble_cooked_image("glfw", "android")

    def test_unknown_target_fails(self):
        self.write_pool(self.full_manifest())

        with self.assertRaises(RuntimeError):
            build_script.assemble_cooked_image("glfw", "ios")

    def test_missing_products_manifest_fails(self):
        with self.assertRaises(RuntimeError):
            build_script.assemble_cooked_image("glfw", "android")


if __name__ == "__main__":
    unittest.main()
