"""Package cooked content into a build product archive at the platform's resource root.

The package stage of build.py uses this module directly; CI release nodes that package
another platform's archive use the CLI. Internal runtime state (logs, caches the app wrote
on a dev machine) is stripped from the archive; signed packages (apk, ipa, macos app) must
be re-signed afterwards.
"""
import argparse
import os
import posixpath
import shutil
import sys
import tempfile
import zipfile

RESOURCE_PREFIX = {
    "glfw": "build/packed/cooked",
    "macos": "sparkle.app/Contents/Resources/packed/cooked",
    "ios": "Payload/sparkle.app/packed/cooked",
    "android": "assets/packed/cooked",
}

# the app's internal storage, written when it runs from the build tree; never shipped
INTERNAL_STATE_PREFIX = {
    "glfw": "build/generated/",
    "macos": "sparkle.app/Contents/SharedSupport/",
}


class PackagingError(RuntimeError):
    pass


def strip_internal_state(package, internal_prefix):
    """Rewrite the archive without internal-state entries; no-op when none exist."""
    with zipfile.ZipFile(package) as zip_file:
        if not any(name.startswith(internal_prefix) for name in zip_file.namelist()):
            return 0

    stripped = 0
    fd, temp_path = tempfile.mkstemp(dir=os.path.dirname(package) or ".", suffix=".zip")
    os.close(fd)
    with zipfile.ZipFile(package) as zip_in, \
            zipfile.ZipFile(temp_path, "w", zipfile.ZIP_DEFLATED) as zip_out:
        for item in zip_in.infolist():
            if item.filename.startswith(internal_prefix):
                stripped += 1
                continue
            zip_out.writestr(item, zip_in.read(item.filename))
    shutil.move(temp_path, package)
    print(f"stripped {stripped} internal-state entries under {internal_prefix}")
    return stripped


def package_cooked(package, framework, cooked_dir):
    """Returns the number of injected files. Raises PackagingError on contract violations.

    A package without cooked content is functional (each cook job falls back to raw
    assets and cooks at runtime), so a missing cooked directory only warns.
    """
    prefix = RESOURCE_PREFIX[framework]

    with zipfile.ZipFile(package) as zip_file:
        names = zip_file.namelist()
    root_dir = prefix.split("/")[0] + "/"
    if not any(name.startswith(root_dir) for name in names):
        raise PackagingError(
            f"{package} has no entries under {root_dir}: unexpected package layout")
    if any(name.startswith(prefix + "/") for name in names):
        raise PackagingError(
            f"{package} already contains cooked content under {prefix}")

    internal_prefix = INTERNAL_STATE_PREFIX.get(framework)
    if internal_prefix:
        strip_internal_state(package, internal_prefix)

    if not cooked_dir or not os.path.isfile(os.path.join(cooked_dir, "manifest.json")):
        print(f"WARNING: no cooked content at {cooked_dir}: the package will cook at runtime")
        return 0

    count = 0
    with zipfile.ZipFile(package, "a", zipfile.ZIP_DEFLATED) as zip_file:
        for root, _, files in os.walk(cooked_dir):
            for file in files:
                path = os.path.join(root, file)
                relative = os.path.relpath(path, cooked_dir).replace(os.sep, "/")
                zip_file.write(path, posixpath.join(prefix, relative))
                count += 1

    print(f"injected {count} cooked files into {package} under {prefix}")
    if count == 0:
        raise PackagingError("cooked content directory is empty")
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", required=True, choices=sorted(RESOURCE_PREFIX))
    parser.add_argument("--package", required=True, help="product archive (zip/apk/ipa)")
    parser.add_argument("--cooked", required=True, help="cooked content directory")
    args = parser.parse_args()

    try:
        package_cooked(args.package, args.framework, args.cooked)
    except PackagingError as error:
        sys.exit(str(error))


if __name__ == "__main__":
    main()
