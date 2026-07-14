"""Tests for the android package re-signing helpers."""

import os
import sys
import tempfile
import unittest
from unittest.mock import patch

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, PROJECT_ROOT)

from build_system.android.build import find_android_sdk, newest_build_tools  # noqa: E402


class BuildToolsTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.sdk = self.directory.name

    def make_build_tools(self, versions):
        for version in versions:
            os.makedirs(os.path.join(self.sdk, "build-tools", version))

    def test_picks_the_newest_version_numerically(self):
        self.make_build_tools(["9.0.0", "35.0.0", "35.0.1"])
        self.assertEqual(os.path.basename(newest_build_tools(self.sdk)), "35.0.1")

    def test_fails_without_build_tools(self):
        with self.assertRaises(RuntimeError):
            newest_build_tools(self.sdk)

    def test_sdk_resolution_prefers_android_home(self):
        with patch.dict(os.environ, {"ANDROID_HOME": self.sdk}):
            self.assertEqual(find_android_sdk(), self.sdk)


if __name__ == "__main__":
    unittest.main()
