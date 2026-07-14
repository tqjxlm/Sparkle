"""Behavioral checks for the Android Gradle task graph."""

import os
import shutil
import subprocess
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
ANDROID_ROOT = PROJECT_ROOT / "build_system" / "android"
GRADLEW = ANDROID_ROOT / "gradlew"
WRAPPER_JAR = ANDROID_ROOT / "gradle" / "wrapper" / "gradle-wrapper.jar"


@unittest.skipUnless(os.environ.get("SPARKLE_ANDROID_GRADLE_TEST") == "1"
                     and os.environ.get("ANDROID_HOME") and shutil.which("java")
                     and WRAPPER_JAR.exists(),
                     "set SPARKLE_ANDROID_GRADLE_TEST=1 with an Android toolchain")
class AndroidGradleTaskGraphTest(unittest.TestCase):

    def task_graph(self, variant):
        env = os.environ.copy()
        if "CI" not in env:
            env["GRADLE_USER_HOME"] = str(
                PROJECT_ROOT / "build_cache" / "gradle-test")
        result = subprocess.run(
            [str(GRADLEW), f"assemble{variant}", "--dry-run", "--console=plain"],
            cwd=ANDROID_ROOT, env=env, capture_output=True, text=True,
            timeout=180, check=True)
        return result.stdout

    def test_each_assemble_builds_only_its_native_variant(self):
        for variant, opposite in (("Debug", "Release"),
                                  ("Release", "Debug")):
            with self.subTest(variant=variant):
                tasks = self.task_graph(variant)
                ordered_tasks = [
                    f":app:externalNativeBuild{variant}",
                    ":app:generateAssetManifests",
                    ":app:copyAssets",
                    f":app:merge{variant}Assets",
                ]
                positions = [tasks.index(task) for task in ordered_tasks]
                self.assertEqual(positions, sorted(positions))
                self.assertNotIn(f":app:externalNativeBuild{opposite}", tasks)

    def test_debug_does_not_run_release_lint(self):
        tasks = self.task_graph("Debug")
        self.assertNotIn(":app:lintVitalAnalyzeRelease", tasks)
        self.assertNotIn(":app:generateReleaseLintVitalReportModel", tasks)

    def test_release_lint_reads_the_generated_assets(self):
        tasks = self.task_graph("Release")
        copy_assets = tasks.index(":app:copyAssets")
        self.assertGreater(tasks.index(":app:lintVitalAnalyzeRelease"),
                           copy_assets)
        self.assertGreater(
            tasks.index(":app:generateReleaseLintVitalReportModel"),
            copy_assets)


if __name__ == "__main__":
    unittest.main()
