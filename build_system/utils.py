import urllib.request
import urllib.error
import zipfile
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
        if e.code == 403:
            print(
                f"HTTP 403 Forbidden - The download URL may have changed or requires authentication")
            print(
                f"Please download the Vulkan SDK manually from: https://vulkan.lunarg.com/sdk/home")
            raise e
        else:
            raise e


def extract_zip(zip_path, extract_to):
    print(f"Extracting {zip_path} to {extract_to} ...")
    with zipfile.ZipFile(zip_path, "r") as zip_ref:
        for member in zip_ref.infolist():
            # Skip entries that would create invalid paths
            if member.filename.startswith('/') or '..' in member.filename:
                continue

            target_path = os.path.join(extract_to, member.filename)

            # Ensure the target path is within the extraction directory
            if not target_path.startswith(os.path.abspath(extract_to)):
                continue

            # If file exists and is read-only, make it writable
            if os.path.exists(target_path):
                if os.path.isdir(target_path):
                    os.chmod(target_path, stat.S_IRWXU | stat.S_IRGRP |
                             stat.S_IXGRP | stat.S_IROTH | stat.S_IXOTH)
                else:
                    os.chmod(target_path, stat.S_IWRITE | stat.S_IREAD)
        zip_ref.extractall(extract_to)


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
            sys.exit(1)

        print(f"{description} completed successfully!")
        return return_code

    except Exception as e:
        print(f"{description} failed with exception: {e}")
        sys.exit(1)


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
