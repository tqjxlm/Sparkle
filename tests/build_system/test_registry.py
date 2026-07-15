"""Consistency checks for the test-case registry and its CI coverage."""

import importlib.util
import json
import os
import re
import sys
import unittest

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = importlib.util.spec_from_file_location(
    "ci_matrix", os.path.join(PROJECT_ROOT, "dev", "ci_matrix.py"))
ci_matrix = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ci_matrix
SPEC.loader.exec_module(ci_matrix)

with open(os.path.join(PROJECT_ROOT, "tests", "registry.json")) as registry_file:
    REGISTRY = json.load(registry_file)
with open(os.path.join(PROJECT_ROOT, "tests", "coverage.json")) as coverage_file:
    COVERAGE = json.load(coverage_file)

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
            cell = ci_matrix.test_cell(triplet)
            self.assertEqual(list(cell)[:3], ["os", "framework", "build_type"])


if __name__ == "__main__":
    unittest.main()
