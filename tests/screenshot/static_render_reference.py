"""Published reference-render location and acceptance policy."""

import os
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))

GROUND_TRUTH_URL_BASE = "https://pub-70861c9d28254fff97386336cba96153.r2.dev/sparkle"
DEFAULT_SCENE = "TestScene"
FLIP_THRESHOLD = 0.1


def download_ground_truth(framework, scene, pipeline):
    filename = f"{scene}_{pipeline}_{framework}.png"
    url = f"{GROUND_TRUTH_URL_BASE}/{filename}"

    sys.path.insert(0, PROJECT_ROOT)
    from build_system.utils import download_file

    destination = os.path.join(
        tempfile.gettempdir(), f"sparkle_ground_truth_{filename}")
    download_file(url, destination)
    return destination
