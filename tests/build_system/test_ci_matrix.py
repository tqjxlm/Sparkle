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

    def summary_rows(self, build_types):
        summary = ci_matrix.summary(build_types)
        lines = summary.splitlines()
        self.assertEqual(lines[0], "## Pipeline dependencies")

        header = "| OS | Framework | Build type | Release gates | Test gates |"
        header_index = lines.index(header)
        explanation = "\n".join(lines[1:header_index])
        self.assertIn("native needs", explanation)
        self.assertIn("script poll", explanation)

        shared_cook = [line for line in lines[1:header_index]
                       if line.startswith("Shared cook:")]
        self.assertEqual(len(shared_cook), 1)
        self.assertIn("`build (macos-latest, macos, Release)`",
                      shared_cook[0])
        self.assertIn("`cook (macos-latest, macos, Release)`",
                      shared_cook[0])
        self.assertIn("native needs", shared_cook[0])

        self.assertEqual(lines[header_index + 1],
                         "| --- | --- | --- | --- | --- |")
        rows = [line for line in lines[header_index + 2:] if line]
        return summary, [tuple(column.strip()
                               for column in line.strip("|").split("|"))
                         for line in rows]

    @staticmethod
    def job_name(stage, cell):
        return f"{stage} ({cell['os']}, {cell['framework']}, {cell['build_type']})"

    def expected_release_gates(self, cell):
        return ("`cook (macos-latest, macos, Release)` (`native needs`) + "
                f"`{self.job_name('build', cell)}` (`script poll`)")

    def expected_test_gates(self, cell, tested):
        if not tested:
            return "not scheduled"
        return ("`cook (macos-latest, macos, Release)` (`native needs`) + "
                f"`{self.job_name('release', cell)}` (`script poll`)")

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

    def test_summary_has_one_accurate_row_per_release_cell(self):
        build_types = ["Debug", "Release"]
        matrices = ci_matrix.matrices(build_types)
        _, rows = self.summary_rows(build_types)
        tested = {(cell["os"], cell["framework"], cell["build_type"])
                  for cell in matrices["test"]}

        self.assertEqual(len(rows), len(matrices["release"]))
        for row, cell in zip(rows, matrices["release"]):
            identity = (cell["os"], cell["framework"], cell["build_type"])
            with self.subTest(cell=identity):
                self.assertEqual(row[:3], identity)
                self.assertEqual(row[3], self.expected_release_gates(cell))
                self.assertEqual(row[4],
                                 self.expected_test_gates(cell,
                                                          identity in tested))

    def test_summary_respects_selected_build_types(self):
        summary, rows = self.summary_rows(["Debug"])
        planned = ci_matrix.matrices(["Debug"])["release"]
        self.assertIn("cook and release are skipped", summary.lower())
        self.assertEqual([row[:3] for row in rows],
                         [(cell["os"], cell["framework"], cell["build_type"])
                          for cell in planned])
        self.assertEqual({row[3] for row in rows},
                         {"skipped with shared cook"})
        self.assertEqual({row[4] for row in rows}, {"not scheduled"})

    def test_cli_emits_one_github_output_line_per_matrix(self):
        stdout = subprocess.run(
            [sys.executable, SCRIPT, "--build_types", '"Debug","Release"'],
            capture_output=True, text=True, check=True).stdout
        lines = [line for line in stdout.splitlines() if line]
        self.assertEqual([line.split("=", 1)[0] for line in lines],
                         ["build", "release", "test"])
        for line in lines:
            self.assertTrue(json.loads(line.split("=", 1)[1]))

    def test_cli_summary_emits_markdown_instead_of_matrix_outputs(self):
        build_types = '"Debug","Release"'
        stdout = subprocess.run(
            [sys.executable, SCRIPT, "--build_types", build_types, "--summary"],
            capture_output=True, text=True, check=True).stdout
        self.assertEqual(stdout.rstrip("\n"),
                         ci_matrix.summary(["Debug", "Release"]))
        self.assertNotIn("build=", stdout)
        self.assertNotIn("release=", stdout)
        self.assertNotIn("test=", stdout)


if __name__ == "__main__":
    unittest.main()
