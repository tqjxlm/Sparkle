import os
from build_system.utils import download_file, extract_zip

SCRIPT = os.path.abspath(__file__)
SCRIPTPATH = os.path.dirname(SCRIPT)


def setup_resources():
    packed_dir = os.path.join(SCRIPTPATH, "packed")
    zip_path = os.path.join(SCRIPTPATH, "resources.zip")
    url = "https://drive.usercontent.google.com/download?id=1QbM3lTuDaiozINaNqAkWDHkLF7UBQdRI&export=download&confirm=t"

    if os.path.exists(zip_path):
        print("Resources are up-to-date, skipping setup.")
        return

    print("Setting up resources...")
    os.makedirs(packed_dir, exist_ok=True)
    download_file(url, zip_path)
    try:
        extract_zip(zip_path, packed_dir)
        print("Resources setup completed.")
    except Exception as e:
        print(f"Resource extraction failed: {e}")
        if os.path.exists(zip_path):
            os.remove(zip_path)
            print(f"Removed corrupted zip file: {zip_path}")
        raise
