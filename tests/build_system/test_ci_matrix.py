"""Tests for the CI pipeline generator."""

import importlib.util
import os
import subprocess
import sys
import unittest

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPT = os.path.join(PROJECT_ROOT, "dev", "ci_matrix.py")
SPEC = importlib.util.spec_from_file_location("ci_matrix", SCRIPT)
ci_matrix = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ci_matrix
SPEC.loader.exec_module(ci_matrix)

JOBS = dict(ci_matrix.jobs())


def release_products():
    return [product for product in ci_matrix.PRODUCTS
            if "Release" in product.get("build_types", ("Release",))]


class CiPipelineTest(unittest.TestCase):

    def test_workflow_matches_its_generator(self):
        with open(ci_matrix.WORKFLOW, encoding="utf-8") as workflow_file:
            self.assertEqual(workflow_file.read(), ci_matrix.generate())

    def test_check_mode_passes_on_a_fresh_workflow(self):
        subprocess.run([sys.executable, SCRIPT], check=True)

    def test_every_product_config_builds(self):
        for product in ci_matrix.PRODUCTS:
            for config in product.get("build_types", ("Debug", "Release")):
                self.assertIn(ci_matrix.slug("build", product, config), JOBS)

    def test_debug_builds_are_compile_gates_only(self):
        for job_id, text in JOBS.items():
            if job_id.startswith("build-") and job_id.endswith("-debug"):
                self.assertIn("github.ref_type != 'tag'", text)
        self.assertEqual(len([job_id for job_id in JOBS
                              if job_id.startswith("release-")]),
                         len(release_products()))

    def test_cook_needs_the_macos_release_build(self):
        self.assertIn("needs: build-macos-macos-release", JOBS["cook"])

    # the bc node encodes from the masters the astc node cooked, on the linux binary
    def test_bc_cook_needs_the_astc_cook_and_the_linux_build(self):
        self.assertIn("needs: [cook, build-ubuntu-glfw-release]", JOBS["cook-bc"])
        self.assertIn("name: cooked-pool", JOBS["cook"])
        self.assertIn("name: cooked-pool", JOBS["cook-bc"])

    def test_release_needs_its_own_build_and_its_family_cook(self):
        for product in release_products():
            build_id = ci_matrix.slug("build", product, "Release")
            family = ci_matrix.cook_family(ci_matrix.product_cook_target(product))
            cook_id = ci_matrix.COOK_JOB_ID[family]
            self.assertIn(f"needs: [{build_id}, {cook_id}]",
                          JOBS[ci_matrix.slug("release", product)])

    def test_every_covered_triplet_tests_after_its_release(self):
        tested = set()
        for product in release_products():
            runner = ci_matrix.suite_runner(product)
            if not runner:
                self.assertNotIn(ci_matrix.slug("test", product), JOBS)
                continue
            text = JOBS[ci_matrix.slug("test", product)]
            self.assertIn(f"needs: {ci_matrix.slug('release', product)}", text)
            self.assertIn(f"timeout-minutes: {runner['suite_timeout']}", text)
            if runner["suite_args"]:
                self.assertIn(runner["suite_args"], text)
            self.assertIn(runner["screenshots"], text)
            tested.add(ci_matrix.tested_triplet(product))
        self.assertEqual(tested, set(ci_matrix.covered_triplets()))

    def test_unmatched_coverage_column_fails_loudly(self):
        original = ci_matrix.covered_triplets
        ci_matrix.covered_triplets = lambda: ["windows-macos-release"]
        try:
            with self.assertRaises(LookupError):
                ci_matrix.jobs()
        finally:
            ci_matrix.covered_triplets = original

    def test_cook_publishes_one_image_artifact_per_target(self):
        for target in ci_matrix.cook_targets():
            family = ci_matrix.cook_family(target)
            text = JOBS[ci_matrix.COOK_JOB_ID[family]]
            self.assertIn(f"name: cooked-image-{target}", text)
            self.assertIn(f"path: {ci_matrix.COOK_IMAGE_DIR[family]}/{target}", text)
            self.assertIn(f"--cook_targets {'+'.join(ci_matrix.family_cook_targets(family))}", text)

    # the generator names the artifacts, the release action consumes them by the same name
    def test_release_downloads_only_its_own_cooked_image(self):
        action = os.path.join(PROJECT_ROOT, ".github", "actions", "release-platform", "action.yml")
        with open(action, encoding="utf-8") as action_file:
            text = action_file.read()
        self.assertIn("name: cooked-image-${{ steps.package.outputs.target }}", text)

    def test_github_release_needs_every_release(self):
        for job_id in JOBS:
            if job_id.startswith("release-"):
                self.assertIn(job_id, JOBS["github-release"])

    def test_gate_needs_every_job(self):
        for job_id in ["changes", "format", "tidy"] + list(JOBS):
            if job_id != "ci":
                self.assertIn(job_id, JOBS["ci"])


if __name__ == "__main__":
    unittest.main()
