import os
from build_system.utils import download_file, extract_zip

def setup_resources(script_dir):
    resources_dir = os.path.join(script_dir, "resources")
    packed_dir = os.path.join(resources_dir, "packed")
    zip_path = os.path.join(resources_dir, "resources.zip")
    url = "https://www.dropbox.com/scl/fo/q973wvw155dsrdf6wfkqt/ALzcfn5n44756u4NDKLJHl8?rlkey=l9kyje6xkf1tf6zsiv4bx98lj&st=wzo7vcll&dl=1"

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
