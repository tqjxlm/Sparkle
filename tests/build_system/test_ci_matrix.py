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


class CiMatrixTest(unittest.TestCase):

    def test_release_covers_every_product_and_config(self):
        matrices = ci_matrix.matrices(["Debug", "Release"])
        expected = [dict(product, build_type=build_type)
                    for product in ci_matrix.PRODUCTS
                    for build_type in ("Debug", "Release")]
        self.assertEqual(matrices["release"], expected)

    def test_build_excludes_only_the_standalone_cook_build(self):
        matrices = ci_matrix.matrices(["Debug", "Release"])
        missing = [cell for cell in matrices["release"]
                   if cell not in matrices["build"]]
        self.assertEqual(missing, list(ci_matrix.STANDALONE_BUILDS))

    def test_every_test_row_targets_a_released_product(self):
        matrices = ci_matrix.matrices(["Debug", "Release"])
        released = {(cell["os"], cell["framework"], cell["build_type"])
                    for cell in matrices["release"]}
        for row in matrices["test"]:
            self.assertIn((row["os"], row["framework"], row["build_type"]),
                          released)

    def test_unrequested_build_types_produce_no_cells(self):
        matrices = ci_matrix.matrices(["Debug"])
        self.assertEqual(matrices["test"], [])
        self.assertNotIn("Release",
                         {cell["build_type"] for cell in matrices["release"]})

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
                         ["build", "release", "test"])
        for line in lines:
            self.assertTrue(json.loads(line.split("=", 1)[1]))


if __name__ == "__main__":
    unittest.main()
