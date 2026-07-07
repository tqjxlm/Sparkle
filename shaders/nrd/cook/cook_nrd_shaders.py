"""Build-time NRD shader cook: NRD's embedded SPIR-V -> MSL + reflection manifest.

Unpacks the ShaderMake blob headers NRD generates (thirdparty/NRD/_Shaders/*.cs.spirv.h), filters
to the permutations the engine uses, and cross-compiles each with nrd_msl_cook (host tool) into
the intermediate shader output directory — nothing is committed. The runtime looks pipelines up by
a canonical stem derived from nrd::PipelineDesc::shaderIdentifier: "<Blob>.cs.hlsl|<defines sorted
by name>", sanitized to filename characters; the blob permutation keys are already name-sorted, so
both sides canonicalize identically. A pipeline the filter missed fails loudly at runtime with a
re-cook hint.

Invoked by shaders/CMakeLists.txt; not meant to be run by hand.
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

BLOB_SIGNATURE = b"NVSP"
SPIRV_MAGIC = b"\x03\x02\x23\x07"


def sanitize(name):
    return "".join(c if (c.isalnum() or c in ".-") else "_" for c in name)


def parse_blob(nums, path):
    """Yields (permutation string, spirv bytes)."""
    if nums[:4] == SPIRV_MAGIC:
        yield "", bytes(nums)
        return
    if nums[:4] != BLOB_SIGNATURE:
        raise ValueError(f"{path}: unknown blob signature {nums[:4]!r}")
    offset = 4
    while offset + 8 <= len(nums):
        perm_size, data_size = struct.unpack_from("<II", nums, offset)
        if data_size == 0:
            break
        offset += 8
        perm = bytes(nums[offset:offset + perm_size]).decode()
        offset += perm_size
        yield perm, bytes(nums[offset:offset + data_size])
        offset += data_size


def stem_of(blob_name, permutation):
    defines = permutation.split() if permutation else []
    return sanitize("|".join([f"{blob_name}.cs.hlsl"] + defines))


def keep(blob_name, permutation, prefixes, signal):
    if not any(blob_name.startswith(p) for p in prefixes):
        return False
    return "NRD_SIGNAL=" not in permutation or f"NRD_SIGNAL={signal}" in permutation


def nrd_version(nrd_header):
    text = open(nrd_header).read()
    return [re.search(rf"#define NRD_VERSION_{part}\s+(\d+)", text).group(1)
            for part in ("MAJOR", "MINOR", "BUILD")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shaders-dir", required=True, help="NRD _Shaders dir with *.cs.spirv.h")
    parser.add_argument("--tool", required=True, help="nrd_msl_cook host binary")
    parser.add_argument("--platform", required=True, choices=["macos", "ios"])
    parser.add_argument("--nrd-header", required=True, help="NRD.h for the version guard")
    parser.add_argument("--output", required=True)
    parser.add_argument("--blob-prefix", action="append", required=True)
    parser.add_argument("--signal", default="BOTH")
    args = parser.parse_args()

    out_dir = os.path.join(args.output, args.platform)
    shutil.rmtree(out_dir, ignore_errors=True)
    os.makedirs(out_dir)

    manifest_lines = ["nrd_version " + " ".join(nrd_version(args.nrd_header))]
    cooked = 0
    for header in sorted(os.listdir(args.shaders_dir)):
        if not header.endswith(".cs.spirv.h"):
            continue
        text = open(os.path.join(args.shaders_dir, header)).read()
        # one header can carry arrays for several denoisers, so the blob name comes from the symbol
        for symbol, body in re.findall(r"const uint8_t (\w+)\[\] = \{([^}]*)\};", text, re.S):
            blob_name = re.fullmatch(r"g_(\w+)_cs_spirv", symbol).group(1)
            nums = bytes(int(x) for x in body.replace("\n", "").split(",") if x.strip())
            for permutation, spirv in parse_blob(nums, header):
                if not keep(blob_name, permutation, args.blob_prefix, args.signal):
                    continue
                stem = stem_of(blob_name, permutation)
                spv_path = os.path.join(out_dir, stem + ".spv")
                metal_path = os.path.join(out_dir, stem + ".metal")
                with open(spv_path, "wb") as f:
                    f.write(spirv)
                meta = subprocess.run([args.tool, spv_path, metal_path, args.platform],
                                      capture_output=True, text=True)
                os.remove(spv_path)
                if meta.returncode != 0:
                    print(meta.stderr, file=sys.stderr)
                    sys.exit(1)
                tokens = dict(line.split(" ", 1) for line in meta.stdout.strip().splitlines())
                manifest_lines.append(
                    f"pipeline {stem} {stem}.metal {tokens['entry']} {tokens['wg']} {tokens['cb']}")
                for tag in ("srv", "uav", "sampler"):
                    manifest_lines.append(f"{tag} {tokens[tag]}")
                cooked += 1

    with open(os.path.join(out_dir, "manifest.txt"), "w") as f:
        f.write("\n".join(manifest_lines) + "\n")
    print(f"nrd cook ({args.platform}): {cooked} permutations -> {out_dir}")


if __name__ == "__main__":
    main()
