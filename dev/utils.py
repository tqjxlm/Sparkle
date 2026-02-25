"""Shared utilities for Sparkle test scripts."""


def extract_log_path(output):
    """Extract log file path from app stdout.

    The C++ Logger prints a ``[LOG_FILE] <path>`` marker to stdout when it
    creates the timestamped log file.  This helper scans captured subprocess
    output for that marker and returns the path, or ``None`` if not found.
    """
    for line in output.splitlines():
        if line.startswith("[LOG_FILE] "):
            return line[len("[LOG_FILE] "):]
    return None
