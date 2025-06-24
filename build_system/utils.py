import urllib.request
import zipfile
import os
import stat
import subprocess
import sys


def download_file(url, dest):
    print(f"Downloading {url} to {dest} ...")
    urllib.request.urlretrieve(url, dest)


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
                    os.chmod(target_path, stat.S_IRWXU | stat.S_IRGRP | stat.S_IXGRP | stat.S_IROTH | stat.S_IXOTH)
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
