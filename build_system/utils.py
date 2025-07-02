import urllib.request
import urllib.error
import zipfile
import tarfile
import os
import stat
import subprocess
import sys
import shutil
import platform
import time


def download_file(url, dest):
    print(f"Downloading {url} to {dest} ...")
    try:
        # Add headers to avoid 403 errors from some servers
        req = urllib.request.Request(url, headers={
            'User-Agent': 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36'
        })
        with urllib.request.urlopen(req) as response:
            with open(dest, 'wb') as f:
                f.write(response.read())
    except urllib.error.HTTPError as e:
        print(f"HTTP request failed. error code: {e.code}")
        raise e


def extract_zip(zip_path, extract_to):
    print(f"Extracting {zip_path} to {extract_to} ...")
    with zipfile.ZipFile(zip_path, "r") as zip_ref:
        for member in zip_ref.infolist():
            # Skip entries that would create invalid paths
            if member.filename.startswith('/') or '..' in member.filename:
                continue

            target_path = os.path.join(extract_to, member.filename)

            # remove destination to avoid permission issues
            if os.path.exists(target_path):
                if os.path.isdir(target_path):
                    robust_rmtree(target_path)
                else:
                    os.remove(target_path)

        # Extract all files
        zip_ref.extractall(extract_to)


def extract_archive(archive_path, extract_to):
    """Extract various archive formats (zip, tar.gz, tar.xz, tar.bz2) to destination."""
    print(f"Extracting {archive_path} to {extract_to} ...")

    # Create extraction directory if it doesn't exist
    os.makedirs(extract_to, exist_ok=True)

    # Determine archive type by extension
    archive_path_lower = archive_path.lower()

    if archive_path_lower.endswith('.zip'):
        extract_zip(archive_path, extract_to)
    elif archive_path_lower.endswith(('.tar.gz', '.tgz')):
        with tarfile.open(archive_path, 'r:gz') as tar_ref:
            tar_ref.extractall(extract_to)
    elif archive_path_lower.endswith(('.tar.xz', '.txz')):
        with tarfile.open(archive_path, 'r:xz') as tar_ref:
            tar_ref.extractall(extract_to)
    elif archive_path_lower.endswith(('.tar.bz2', '.tbz2')):
        with tarfile.open(archive_path, 'r:bz2') as tar_ref:
            tar_ref.extractall(extract_to)
    elif archive_path_lower.endswith('.tar'):
        with tarfile.open(archive_path, 'r') as tar_ref:
            tar_ref.extractall(extract_to)
    else:
        raise ValueError(f"Unsupported archive format: {archive_path}")


def run_command_with_logging(cmd, log_file_path, description):
    """
    Run a command and log output to both console and file.
    """
    print(f"{description}... See {log_file_path} for detailed logs.")
    print(f"Running: {' '.join(cmd)}")

    try:
        with open(log_file_path, "w") as log:
            process = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                universal_newlines=True, bufsize=1)

            # Read output line by line and write to both console and file
            if process.stdout:
                for line in process.stdout:
                    print(line, end='')  # Print to console
                    log.write(line)      # Write to log file
                    log.flush()          # Ensure immediate write

            return_code = process.wait()

        if return_code != 0:
            print(f"{description} failed! Check {log_file_path} for details.")
            raise Exception()

        print(f"{description} completed successfully!")
        return return_code

    except Exception as e:
        print(f"{description} failed with exception: {e}")
        raise Exception()


def force_remove_readonly(func, path, exc_info):
    """
    Error handler for Windows readonly file removal.
    """
    if os.path.exists(path):
        # Make the file writable and try again
        os.chmod(path, stat.S_IWRITE | stat.S_IREAD)
        func(path)


def robust_rmtree(path, max_retries=3, retry_delay=1.0):
    """
    Robustly remove a directory tree, handling Windows file permission issues.

    Args:
        path: Path to directory to remove
        max_retries: Maximum number of retry attempts
        retry_delay: Delay between retries in seconds
    """
    if not os.path.exists(path):
        return

    is_windows = platform.system() == "Windows"

    for attempt in range(max_retries):
        try:
            if is_windows:
                # On Windows, use onexc callback to handle readonly files
                shutil.rmtree(path, onexc=force_remove_readonly)
            else:
                shutil.rmtree(path)
            return
        except (OSError, PermissionError) as e:
            if attempt < max_retries - 1:
                print(
                    f"Retry {attempt + 1}/{max_retries}: Failed to remove {path}, retrying in {retry_delay}s...")
                print(f"Error: {e}")
                time.sleep(retry_delay)

                # On Windows, try additional cleanup strategies
                if is_windows:
                    try:
                        # Try to reset file attributes recursively
                        for root, dirs, files in os.walk(path, topdown=False):
                            for name in files:
                                file_path = os.path.join(root, name)
                                try:
                                    os.chmod(
                                        file_path, stat.S_IWRITE | stat.S_IREAD)
                                except:
                                    pass
                            for name in dirs:
                                dir_path = os.path.join(root, name)
                                try:
                                    os.chmod(dir_path, stat.S_IWRITE |
                                             stat.S_IREAD | stat.S_IEXEC)
                                except:
                                    pass
                    except:
                        pass
            else:
                print(
                    f"Failed to remove {path} after {max_retries} attempts: {e}")
                raise
