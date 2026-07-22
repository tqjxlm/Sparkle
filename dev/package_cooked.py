"""Package a target's cooked content image into a build product archive.

The cook stage emits one self-contained content image per cook target: the raw
assets its plan passes through plus the projected artifacts under cooked/. The
image self-declares its target (cooked/content_target.json) and packaging
asserts it matches the product's target — content is never guessed from
archive names. Packaging replaces the archive's content under the packed root
with the image; build-owned subtrees (compiled shaders) are kept. Packages
always carry cooked content: a missing image is an error. The package stage of
build.py uses this module directly; CI release nodes that package another
platform's archive use the CLI. Internal runtime state (logs, caches the app
wrote on a dev machine) is stripped from the archive; signed packages (apk,
ipa, macos app) must be re-signed afterwards.
"""
import argparse
import json
import os
import posixpath
import shutil
import sys
import tempfile
import zipfile

PACKED_ROOT = {
    "glfw": "build/packed/",
    "macos": "sparkle.app/Contents/Resources/packed/",
    "ios": "Payload/sparkle.app/packed/",
    "android": "assets/packed/",
}

# compiled by the platform build and different per backend; everything else under
# the packed root is content, owned by the cook image
BUILD_OWNED_SUBTREES = ("shaders",)

# the app's internal storage, written when it runs from the build tree; never shipped
INTERNAL_STATE_PREFIX = {
    "glfw": "build/generated/",
    "macos": "sparkle.app/Contents/SharedSupport/",
}

ANDROID_ASSET_ROOT = "assets/"
ANDROID_DIR_MANIFEST = "_dir_manifest.txt"


class PackagingError(RuntimeError):
    pass


def collect_image(image_dir, target):
    """The image's files keyed by packed-root-relative posix path. Raises unless the
    image exists and self-declares the expected target."""
    if not image_dir or not os.path.isfile(os.path.join(image_dir, "cooked", "manifest.json")):
        raise PackagingError(
            f"no cooked content image at {image_dir}; run the cook stage for target {target} first")

    marker_path = os.path.join(image_dir, "cooked", "content_target.json")
    try:
        with open(marker_path, encoding="utf-8") as marker_file:
            declared = json.load(marker_file).get("target")
    except (OSError, ValueError) as error:
        raise PackagingError(f"unreadable content target marker at {marker_path}: {error}") from error
    if declared != target:
        raise PackagingError(
            f"content image at {image_dir} is for target '{declared}', not '{target}'")

    files = {}
    for root, _, names in os.walk(image_dir):
        for name in names:
            path = os.path.join(root, name)
            files[os.path.relpath(path, image_dir).replace(os.sep, "/")] = path
    return files


def android_dir_manifests(file_names):
    """AAssetDir cannot enumerate subdirectories, so every directory under assets/
    with subdirectories carries a _dir_manifest.txt naming them; the android build
    generates the same files through gradle (see AndroidFileManager::ListDirectory)."""
    children = {}
    for name in file_names:
        parts = name.split("/")
        if parts[0] + "/" != ANDROID_ASSET_ROOT:
            continue
        for depth in range(1, len(parts) - 1):
            children.setdefault("/".join(parts[:depth]), set()).add(parts[depth])

    return {posixpath.join(directory, ANDROID_DIR_MANIFEST): "\n".join(sorted(names)) + "\n"
            for directory, names in children.items()}


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


def package_cooked(package, framework, image_dir, target):
    """Returns the number of image files placed into the archive. Raises
    PackagingError on contract violations, including a missing image: packages
    always carry cooked content."""
    packed_root = PACKED_ROOT[framework]
    root_dir = packed_root.split("/")[0] + "/"
    internal_prefix = INTERNAL_STATE_PREFIX.get(framework)

    with zipfile.ZipFile(package) as zip_file:
        names = zip_file.namelist()
    if not any(name.startswith(root_dir) for name in names):
        raise PackagingError(
            f"{package} has no entries under {root_dir}: unexpected package layout")

    image_files = collect_image(image_dir, target)

    claimed = {relative.split("/")[0] for relative in image_files} & set(BUILD_OWNED_SUBTREES)
    if claimed:
        raise PackagingError(f"cook image claims build-owned subtrees: {sorted(claimed)}")

    def dropped(name):
        if internal_prefix and name.startswith(internal_prefix):
            return True
        if framework == "android" and name.startswith(ANDROID_ASSET_ROOT) \
                and posixpath.basename(name) == ANDROID_DIR_MANIFEST:
            return True
        if name.startswith(packed_root):
            return name[len(packed_root):].split("/")[0] not in BUILD_OWNED_SUBTREES
        return False

    fd, temp_path = tempfile.mkstemp(dir=os.path.dirname(package) or ".", suffix=".zip")
    os.close(fd)
    with zipfile.ZipFile(package) as zip_in, \
            zipfile.ZipFile(temp_path, "w", zipfile.ZIP_DEFLATED) as zip_out:
        kept = []
        for item in zip_in.infolist():
            if dropped(item.filename):
                continue
            kept.append(item.filename)
            zip_out.writestr(item, zip_in.read(item.filename))

        placed = [packed_root + relative for relative in sorted(image_files)]
        for name in placed:
            zip_out.write(image_files[name[len(packed_root):]], name)

        if framework == "android":
            for name, content in sorted(android_dir_manifests(kept + placed).items()):
                zip_out.writestr(name, content)
    shutil.move(temp_path, package)

    print(f"replaced content of {package} under {packed_root} with {len(placed)} image files")
    return len(placed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", required=True, choices=sorted(PACKED_ROOT))
    parser.add_argument("--package", required=True, help="product archive (zip/apk/ipa)")
    parser.add_argument("--cooked", required=True, help="cooked content image directory")
    parser.add_argument("--target", required=True,
                        help="the product's cook target (e.g. android, windows-glfw);"
                        " must match the image's self-declared target")
    args = parser.parse_args()

    try:
        package_cooked(args.package, args.framework, args.cooked, args.target)
    except PackagingError as error:
        sys.exit(str(error))


if __name__ == "__main__":
    main()
