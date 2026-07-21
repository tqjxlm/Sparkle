"""Tests for the consumed-texture-source stripping in build.py."""

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
KEY = "texture_astc:textures/road.png#color"
ARTIFACT = "cooked/texture_astc/road_00000000.cook"


class StripConsumedTextureSourcesTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.image = self.directory.name
        os.makedirs(os.path.join(self.image, "cooked"))

    def write_manifests(self, consumed, store=None):
        with open(os.path.join(self.image, "cooked", "texture_sources.json"), "w",
                  encoding="utf-8") as manifest_file:
            json.dump(consumed, manifest_file)
        if store is not None:
            with open(os.path.join(self.image, "cooked", "manifest.json"), "w",
                      encoding="utf-8") as store_file:
                json.dump(store, store_file)

    def add_file(self, relative):
        path = os.path.join(self.image, relative)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as content:
            content.write(b"x")
        return path

    def test_strips_source_with_verified_artifacts(self):
        source = self.add_file(SOURCE)
        self.add_file(ARTIFACT)
        self.write_manifests({SOURCE: [KEY]}, {KEY: {"artifact": ARTIFACT}})

        self.assertEqual(build_script.strip_consumed_texture_sources(self.image), 1)
        self.assertFalse(os.path.exists(source))

    def test_keeps_source_when_artifact_file_is_missing(self):
        source = self.add_file(SOURCE)
        self.write_manifests({SOURCE: [KEY]}, {KEY: {"artifact": ARTIFACT}})

        self.assertEqual(build_script.strip_consumed_texture_sources(self.image), 0)
        self.assertTrue(os.path.exists(source))

    def test_keeps_source_when_manifest_entry_is_missing(self):
        source = self.add_file(SOURCE)
        self.add_file(ARTIFACT)
        self.write_manifests({SOURCE: [KEY]}, {})

        self.assertEqual(build_script.strip_consumed_texture_sources(self.image), 0)
        self.assertTrue(os.path.exists(source))

    def test_keeps_source_without_recorded_artifacts(self):
        source = self.add_file(SOURCE)
        self.write_manifests({SOURCE: []})

        self.assertEqual(build_script.strip_consumed_texture_sources(self.image), 0)
        self.assertTrue(os.path.exists(source))

    def test_rejects_escaping_entry_without_deleting(self):
        outside = self.add_file("outside.png")
        inner = os.path.join(self.image, "image")
        os.makedirs(os.path.join(inner, "cooked"))
        with open(os.path.join(inner, "cooked", "texture_sources.json"), "w",
                  encoding="utf-8") as manifest_file:
            json.dump({"../outside.png": [KEY]}, manifest_file)

        with self.assertRaises(RuntimeError):
            build_script.strip_consumed_texture_sources(inner)
        self.assertTrue(os.path.exists(outside))

    def test_rejects_absolute_entry(self):
        target = self.add_file("victim.png")
        self.write_manifests({target: [KEY]})

        with self.assertRaises(RuntimeError):
            build_script.strip_consumed_texture_sources(self.image)
        self.assertTrue(os.path.exists(target))

    def test_rejects_legacy_list_manifest(self):
        source = self.add_file(SOURCE)
        self.write_manifests([SOURCE])

        with self.assertRaises(RuntimeError):
            build_script.strip_consumed_texture_sources(self.image)
        self.assertTrue(os.path.exists(source))


if __name__ == "__main__":
    unittest.main()
