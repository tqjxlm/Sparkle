"""Consistency checks for the test-case registry and its CI coverage."""

import csv
import importlib.util
import json
import os
import re
import sys
import unittest

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, os.path.join(PROJECT_ROOT, *path))
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


ci_matrix = load_module("ci_matrix", ("dev", "ci_matrix.py"))
run_tests = load_module("run_tests", ("dev", "run_tests.py"))

with open(os.path.join(PROJECT_ROOT, "tests", "registry.json")) as registry_file:
    REGISTRY = json.load(registry_file)
COVERAGE = run_tests.load_coverage()

REGISTRAR_PATTERN = re.compile(r'TestCaseRegistrar<\w+>\s+\w+\("([^"]+)"\)')


def cpp_registrations():
    names = set()
    for root, _, files in os.walk(os.path.join(PROJECT_ROOT, "tests")):
        for name in files:
            if not name.endswith((".cpp", ".mm")):
                continue
            with open(os.path.join(root, name), errors="replace") as source:
                names.update(REGISTRAR_PATTERN.findall(source.read()))
    return names


class RegistryTest(unittest.TestCase):

    def test_case_names_are_unique(self):
        names = [case["name"] for case in REGISTRY]
        self.assertEqual(sorted(names), sorted(set(names)))

    def test_cases_run_registered_test_cases(self):
        registrations = cpp_registrations()
        for case in REGISTRY:
            self.assertIn(case["test_case"], registrations,
                          f"case {case['name']}")

    def test_evaluator_scripts_exist(self):
        for case in REGISTRY:
            evaluator = case.get("evaluator")
            if evaluator:
                self.assertTrue(
                    os.path.isfile(os.path.join(PROJECT_ROOT, evaluator["script"])),
                    f"{case['name']}: {evaluator['script']}")


class CoverageTest(unittest.TestCase):

    def test_coverage_rows_pick_from_the_registry(self):
        with open(os.path.join(PROJECT_ROOT, "tests", "coverage.csv"),
                  newline="") as coverage_file:
            rows = [row["case"] for row in csv.DictReader(coverage_file)]
        names = {case["name"] for case in REGISTRY}
        self.assertLessEqual(set(rows), names)
        self.assertEqual(sorted(rows), sorted(set(rows)), "duplicate rows")

    def test_coverage_picks_existing_registry_cases(self):
        names = {case["name"] for case in REGISTRY}
        for triplet, picks in COVERAGE.items():
            self.assertLessEqual(set(picks), names, f"triplet {triplet}")
            self.assertEqual(sorted(picks), sorted(set(picks)),
                             f"triplet {triplet} picks a case twice")

    def test_every_covered_triplet_picks_at_least_one_case(self):
        for triplet, picks in COVERAGE.items():
            self.assertTrue(picks, f"triplet {triplet} picks no cases")

    def test_every_covered_triplet_has_a_ci_runner(self):
        for triplet in COVERAGE:
            self.assertIn(triplet, ci_matrix.TEST_RUNNERS)


if __name__ == "__main__":
    unittest.main()
