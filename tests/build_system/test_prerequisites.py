import hashlib
import os
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from build_system import prerequisites


class InstallVulkanSdkTests(unittest.TestCase):
    VERSION = "test-version"
    ARCHIVE_CONTENT = b"verified Vulkan SDK archive"

    @property
    def digest(self):
        return hashlib.sha256(self.ARCHIVE_CONTENT).hexdigest()

    @property
    def versions(self):
        return {
            "VulkanSDK": self.VERSION,
            "VulkanSDK_SHA256": {"linux": self.digest},
        }

    def create_linux_archive(self, build_cache_dir):
        archive_path = os.path.join(
            build_cache_dir, f"vulkansdk-linux-x86_64-{self.VERSION}.tar.xz")
        with open(archive_path, "wb") as archive:
            archive.write(self.ARCHIVE_CONTENT)
        return archive_path

    def extract_linux_sdk(self, _archive_path, destination):
        include_dir = os.path.join(
            destination, self.VERSION, "x86_64", "include", "vulkan")
        os.makedirs(include_dir)
        with open(os.path.join(include_dir, "vulkan.h"), "w"):
            pass

    @patch("build_system.prerequisites.platform.system", return_value="Linux")
    @patch("build_system.prerequisites.load_prerequisites_versions")
    @patch("build_system.prerequisites.extract_archive")
    @patch("build_system.prerequisites.download_file")
    def test_verifies_fresh_download(self, mock_download, mock_extract, mock_versions, _mock_system):
        mock_versions.return_value = self.versions
        mock_download.side_effect = lambda _url, path: self.create_linux_archive(os.path.dirname(path))
        mock_extract.side_effect = self.extract_linux_sdk

        with tempfile.TemporaryDirectory() as build_cache_dir:
            sdk_path = prerequisites.install_vulkan_sdk(build_cache_dir)

        self.assertTrue(sdk_path.endswith(os.path.join("VulkanSDK", self.VERSION)))
        mock_download.assert_called_once()

    @patch("build_system.prerequisites.platform.system", return_value="Linux")
    @patch("build_system.prerequisites.load_prerequisites_versions")
    @patch("build_system.prerequisites.extract_archive")
    @patch("build_system.prerequisites.download_file")
    def test_verifies_cached_download(self, mock_download, mock_extract, mock_versions, _mock_system):
        mock_versions.return_value = self.versions
        mock_extract.side_effect = self.extract_linux_sdk

        with tempfile.TemporaryDirectory() as build_cache_dir:
            self.create_linux_archive(build_cache_dir)
            prerequisites.install_vulkan_sdk(build_cache_dir)

        mock_download.assert_not_called()

    @patch("build_system.prerequisites.platform.system", return_value="Linux")
    @patch("build_system.prerequisites.load_prerequisites_versions")
    @patch("build_system.prerequisites.download_file")
    def test_removes_cached_download_with_wrong_hash(self, mock_download, mock_versions, _mock_system):
        mock_versions.return_value = self.versions

        with tempfile.TemporaryDirectory() as build_cache_dir:
            archive_path = self.create_linux_archive(build_cache_dir)
            versions = self.versions
            versions["VulkanSDK_SHA256"]["linux"] = "0" * 64
            mock_versions.return_value = versions
            with self.assertRaisesRegex(RuntimeError, "SHA-256 verification"):
                prerequisites.install_vulkan_sdk(build_cache_dir)
            self.assertFalse(os.path.exists(archive_path))

        mock_download.assert_not_called()


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
