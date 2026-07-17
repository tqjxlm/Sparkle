"""Tests for the CI matrix planner."""

import importlib.util
import json
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


SUITE_KEYS = {"test"}.union(
    *(runner.keys() for runner in ci_matrix.TEST_RUNNERS.values())) - {"abi"}


def without_suite(cell):
    return {key: value for key, value in cell.items() if key not in SUITE_KEYS}


class CiMatrixTest(unittest.TestCase):

    def test_release_covers_every_product_release_config_only(self):
        # Debug products are compile-coverage only: they build but never release
        matrices = ci_matrix.matrices(["Debug", "Release"])
        expected = [ci_matrix.product_cell(product, "Release")
                    for product in ci_matrix.PRODUCTS]
        self.assertEqual([without_suite(cell) for cell in matrices["release"]],
                         expected)

    def test_restricted_products_never_leak_their_restriction(self):
        matrices = ci_matrix.matrices(["Debug", "Release"])
        android_x86 = [cell for cell in matrices["release"]
                       if cell.get("abi") == "x86_64"]
        self.assertEqual([cell["build_type"] for cell in android_x86],
                         ["Release"])
        for cell in android_x86:
            self.assertNotIn("build_types", cell)

    def test_debug_products_still_build(self):
        matrices = ci_matrix.matrices(["Debug", "Release"])
        built = {(cell["os"], cell["framework"], cell.get("abi"),
                  cell["build_type"]) for cell in matrices["build"]}
        for product in ci_matrix.PRODUCTS:
            if "Debug" not in product.get("build_types", ("Debug",)):
                continue
            self.assertIn((product["os"], product["framework"],
                           product.get("abi"), "Debug"), built)

    def test_build_excludes_only_the_standalone_cook_build(self):
        matrices = ci_matrix.matrices(["Debug", "Release"])
        missing = [without_suite(cell) for cell in matrices["release"]
                   if without_suite(cell) not in matrices["build"]]
        self.assertEqual(missing, list(ci_matrix.STANDALONE_BUILDS))

    def test_every_covered_triplet_rides_exactly_one_release_cell(self):
        matrices = ci_matrix.matrices(["Debug", "Release"])
        tested = [cell for cell in matrices["release"] if cell.get("test")]
        self.assertEqual({ci_matrix.triplet(cell) for cell in tested},
                         set(ci_matrix.covered_triplets()))
        for cell in tested:
            runner = ci_matrix.TEST_RUNNERS[ci_matrix.triplet(cell)]
            self.assertEqual(cell.get("abi", ""), runner.get("abi", ""))
            for key, value in runner.items():
                if key != "abi":
                    self.assertEqual(cell[key], value)

    def test_unmatched_coverage_column_fails_loudly(self):
        original = ci_matrix.covered_triplets
        ci_matrix.covered_triplets = lambda: ["windows-macos-release"]
        try:
            with self.assertRaises(LookupError):
                ci_matrix.matrices(["Release"])
        finally:
            ci_matrix.covered_triplets = original

    def test_unrequested_build_types_produce_no_cells(self):
        matrices = ci_matrix.matrices(["Debug"])
        self.assertEqual(matrices["release"], [])
        self.assertNotIn("Release",
                         {cell["build_type"] for cell in matrices["build"]})

    def test_display_name_keys_lead_every_cell(self):
        # GitHub renders matrix job names from the include object's leading values,
        # and wait-for-job awaits cells by those names: os, framework, build_type
        # must stay the first keys in that order
        for matrix in ci_matrix.matrices(["Debug", "Release"]).values():
            for cell in matrix:
                self.assertEqual(list(cell)[:3],
                                 ["os", "framework", "build_type"])

    def test_cli_emits_one_github_output_line_per_matrix(self):
        stdout = subprocess.run(
            [sys.executable, SCRIPT, "--build_types", '"Debug","Release"'],
            capture_output=True, text=True, check=True).stdout
        lines = [line for line in stdout.splitlines() if line]
        self.assertEqual([line.split("=", 1)[0] for line in lines],
                         ["build", "release"])
        for line in lines:
            self.assertTrue(json.loads(line.split("=", 1)[1]))


if __name__ == "__main__":
    unittest.main()
