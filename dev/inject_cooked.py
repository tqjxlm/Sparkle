"""Append a cooked-content directory into a build product archive at the platform's resource root.

Appending never rewrites existing entries, so executable bits and bundle layout inside the
archive are preserved. Signed packages (apk, ipa, macos app) must be re-signed afterwards.
"""
import argparse
import os
import sys
import zipfile

RESOURCE_PREFIX = {
    "glfw": "build/packed/cooked",
    "macos": "sparkle.app/Contents/Resources/packed/cooked",
    "ios": "Payload/sparkle.app/packed/cooked",
    "android": "assets/packed/cooked",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", required=True, choices=sorted(RESOURCE_PREFIX))
    parser.add_argument("--package", required=True, help="product archive (zip/apk/ipa)")
    parser.add_argument("--cooked", required=True, help="cooked content directory")
    args = parser.parse_args()

    prefix = RESOURCE_PREFIX[args.framework]
    if not os.path.isfile(os.path.join(args.cooked, "manifest.json")):
        sys.exit(f"no manifest.json under {args.cooked}: not a cooked content directory")

    with zipfile.ZipFile(args.package, "a", zipfile.ZIP_DEFLATED) as zf:
        root_dir = prefix.split("/")[0] + "/"
        if not any(name.startswith(root_dir) for name in zf.namelist()):
            sys.exit(f"{args.package} has no entries under {root_dir}: unexpected package layout")

        count = 0
        for root, _, files in os.walk(args.cooked):
            for file in files:
                path = os.path.join(root, file)
                arcname = os.path.join(prefix, os.path.relpath(path, args.cooked))
                zf.write(path, arcname)
                count += 1

    print(f"injected {count} cooked files into {args.package} under {prefix}")
    if count == 0:
        sys.exit("cooked content directory is empty")


if __name__ == "__main__":
    main()
