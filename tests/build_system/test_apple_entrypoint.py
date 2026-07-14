"""Regression tests for Apple process exit-code propagation."""

import os
import unittest


PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class AppleEntrypointTest(unittest.TestCase):

    def test_headless_test_case_returns_application_exit_code(self):
        source_path = os.path.join(
            PROJECT_ROOT, "frameworks", "source", "apple", "AppleMain.mm")
        with open(source_path) as source_file:
            source = source_file.read()

        cleanup = source.index("            app.Cleanup();")
        headless_return = source[cleanup:source.index("        }", cleanup)]
        self.assertIn("return app.GetExitCode();", headless_return)


if __name__ == "__main__":
    unittest.main()
