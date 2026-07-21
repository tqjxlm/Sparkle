"""Package the cooked content image into a build product archive.

The cook stage emits a self-contained content image: every raw asset passed
through unchanged plus the derived artifacts under cooked/. Packaging replaces
the archive's content under the packed root with that image; build-owned
subtrees (compiled shaders) are kept. A missing image only warns: the package
then ships the build's raw assets and cooks at runtime. The package stage of
build.py uses this module directly; CI release nodes that package another
platform's archive use the CLI. Internal runtime state (logs, caches the app
wrote on a dev machine) is stripped from the archive; signed packages (apk,
ipa, macos app) must be re-signed afterwards.
"""
import argparse
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

# one compressed-texture family ships per product; the other is dead weight for its GPUs
TEXTURE_FAMILY_DIRS = {"astc": "cooked/texture_astc/", "bc": "cooked/texture_bc/"}


def texture_family(framework, package):
    """ASTC for Apple and Android GPUs, BC for other desktop GPUs. glfw products
    carry their host system in the package name (see build.py copy_build_products)."""
    if framework != "glfw":
        return "astc"
    system = os.path.basename(package).split("-")[0]
    return "astc" if system == "macos" else "bc"


ANDROID_ASSET_ROOT = "assets/"
ANDROID_DIR_MANIFEST = "_dir_manifest.txt"


class PackagingError(RuntimeError):
    pass


def collect_image(image_dir):
    """The image's files keyed by packed-root-relative posix path, or None when
    there is no usable image."""
    if not image_dir or not os.path.isfile(os.path.join(image_dir, "cooked", "manifest.json")):
        return None

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


def package_cooked(package, framework, image_dir):
    """Returns the number of image files placed into the archive. Raises
    PackagingError on contract violations.

    A package without cooked content is functional (each cook job falls back to
    raw assets and cooks at runtime), so a missing image only warns.
    """
    packed_root = PACKED_ROOT[framework]
    root_dir = packed_root.split("/")[0] + "/"
    internal_prefix = INTERNAL_STATE_PREFIX.get(framework)

    with zipfile.ZipFile(package) as zip_file:
        names = zip_file.namelist()
    if not any(name.startswith(root_dir) for name in names):
        raise PackagingError(
            f"{package} has no entries under {root_dir}: unexpected package layout")

    image_files = collect_image(image_dir)
    if image_files is None:
        print(f"WARNING: no cooked content image at {image_dir}: the package will cook at runtime")
        if internal_prefix:
            strip_internal_state(package, internal_prefix)
        return 0

    family = texture_family(framework, package)
    dropped_families = tuple(prefix for name, prefix in TEXTURE_FAMILY_DIRS.items() if name != family)
    image_files = {relative: path for relative, path in image_files.items()
                   if not relative.startswith(dropped_families)}

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
    args = parser.parse_args()

    try:
        package_cooked(args.package, args.framework, args.cooked)
    except PackagingError as error:
        sys.exit(str(error))


if __name__ == "__main__":
    main()
