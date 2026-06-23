import os
import subprocess
import unittest
from unittest.mock import patch

from build_system import prerequisites


class FindVisualStudioPathTests(unittest.TestCase):
    def setUp(self):
        self.original_is_windows = prerequisites.is_windows

    def tearDown(self):
        prerequisites.is_windows = self.original_is_windows

    @patch.dict(os.environ, {}, clear=True)
    @patch("build_system.prerequisites.os.path.exists")
    @patch("build_system.prerequisites.subprocess.run")
    def test_accepts_latest_visual_studio_with_cpp_tools(self, mock_run, mock_exists):
        prerequisites.is_windows = True
        vswhere_path = os.path.join(
            r"C:\Program Files", "Microsoft Visual Studio", "Installer", "vswhere.exe")
        vs_path = r"C:\Program Files\Microsoft Visual Studio\18\Enterprise"
        vcvars_path = os.path.join(
            vs_path, "VC", "Auxiliary", "Build", "vcvars64.bat")

        mock_exists.side_effect = lambda path: path in (
            vswhere_path, vcvars_path)
        mock_run.side_effect = [
            subprocess.CompletedProcess([], 0, stdout=vs_path, stderr=""),
            subprocess.CompletedProcess([], 0, stdout="18.0.0", stderr=""),
        ]

        with patch("builtins.print"):
            self.assertEqual(prerequisites.find_visual_studio_path(), vs_path)

        vswhere_args = mock_run.call_args_list[0].args[0]
        self.assertNotIn("-version", vswhere_args)
        self.assertIn(
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", vswhere_args)

    @patch.dict(os.environ, {"VS_PATH": r"C:\CustomVS"}, clear=True)
    @patch("build_system.prerequisites.glob.glob")
    @patch("build_system.prerequisites.os.path.exists")
    @patch("build_system.prerequisites.subprocess.run")
    def test_invalid_vs_path_falls_back_to_vswhere(self, mock_run, mock_exists, mock_glob):
        prerequisites.is_windows = True
        vswhere_path = os.path.join(
            r"C:\Program Files", "Microsoft Visual Studio", "Installer", "vswhere.exe")
        vs_path = r"C:\Program Files\Microsoft Visual Studio\2022\Community"
        vcvars_path = os.path.join(
            vs_path, "VC", "Auxiliary", "Build", "vcvars64.bat")

        mock_glob.return_value = []
        mock_exists.side_effect = lambda path: path in (
            r"C:\CustomVS",
            vswhere_path,
            vcvars_path,
        )
        mock_run.side_effect = [
            subprocess.CompletedProcess([], 0, stdout=vs_path, stderr=""),
            subprocess.CompletedProcess([], 0, stdout="17.0.0", stderr=""),
        ]

        with patch("builtins.print"):
            self.assertEqual(prerequisites.find_visual_studio_path(), vs_path)


if __name__ == "__main__":
    unittest.main()
