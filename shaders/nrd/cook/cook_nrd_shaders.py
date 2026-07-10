"""Build-time NRD shader cook: NRD's embedded SPIR-V -> per-platform shader + reflection manifest.

Unpacks the ShaderMake blob headers NRD generates (thirdparty/NRD/_Shaders/*.cs.spirv.h), filters
to the permutations the engine uses, and cooks each into the intermediate shader output directory —
nothing is committed. macos/ios cross-compile to MSL with nrd_msl_cook (host tool); android keeps
the SPIR-V as-is (the Vulkan backend reflects bindings on device), so only the entry point and
workgroup size are parsed here. The runtime looks pipelines up by a canonical stem derived from
nrd::PipelineDesc::shaderIdentifier: "<Blob>.cs.hlsl|<defines sorted by name>", sanitized to
filename characters; the blob permutation keys are already name-sorted, so both sides canonicalize
identically. A pipeline the filter missed fails loudly at runtime with a re-cook hint.

Invoked by shaders/CMakeLists.txt; not meant to be run by hand.
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

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


SPIRV_VERSION_1_4 = 0x00010400
SPV_OP_EXTENSION = 10
SPV_OP_EXECUTION_MODE = 16
SPV_OP_CAPABILITY = 17
SPV_CAPABILITY_COMPUTE_DERIVATIVE_GROUP_QUADS = 5288
SPV_EXECUTION_MODE_DERIVATIVE_GROUP_QUADS = 5289


def strip_compute_derivatives(spirv):
    """DXC declares SPV_KHR_compute_shader_derivatives for QuadReadAcross* in compute only to pin
    the 2x2 quad arrangement; the swaps themselves are core GroupNonUniformQuad. Dropping the
    declarations leaves the implementation-defined linear quad mapping — the semantics the Metal
    backend already ships with (simd-group quads), validated by the NRD quality gates."""
    words = struct.unpack(f"<{len(spirv) // 4}I", spirv)
    out = list(words[:5])
    i = 5
    while i < len(words):
        count = words[i] >> 16
        opcode = words[i] & 0xFFFF
        instruction = words[i:i + count]
        drop = False
        if opcode == SPV_OP_CAPABILITY and instruction[1] == SPV_CAPABILITY_COMPUTE_DERIVATIVE_GROUP_QUADS:
            drop = True
        elif opcode == SPV_OP_EXTENSION:
            raw = b"".join(struct.pack("<I", w) for w in instruction[1:])
            drop = raw.split(b"\x00")[0] == b"SPV_KHR_compute_shader_derivatives"
        elif opcode == SPV_OP_EXECUTION_MODE and instruction[2] == SPV_EXECUTION_MODE_DERIVATIVE_GROUP_QUADS:
            drop = True
        if not drop:
            out.extend(instruction)
        i += count
    return struct.pack(f"<{len(out)}I", *out)


def downgrade_spirv_to_1_4(spirv, validator):
    """DXC's vulkan1.2 target stamps SPIR-V 1.5, but the engine's Vulkan 1.1 instance +
    VK_KHR_spirv_1_4 accepts at most 1.4. NRD's shaders use no 1.5-only features, so rewriting the
    version word suffices; spirv-val proves it per module when the Vulkan SDK is available."""
    version = struct.unpack_from("<I", spirv, 4)[0]
    if version <= SPIRV_VERSION_1_4:
        return spirv
    downgraded = spirv[:4] + struct.pack("<I", SPIRV_VERSION_1_4) + spirv[8:]
    if validator:
        with tempfile.NamedTemporaryFile(suffix=".spv") as probe:
            probe.write(downgraded)
            probe.flush()
            result = subprocess.run([validator, "--target-env", "vulkan1.1spv1.4", probe.name],
                                    capture_output=True, text=True)
            if result.returncode != 0:
                raise ValueError(f"SPIR-V 1.4 downgrade failed validation: {result.stderr}")
    return downgraded


def find_spirv_validator():
    sdk = os.environ.get("VULKAN_SDK")
    if not sdk:
        return None
    for name in ("bin/spirv-val", "Bin/spirv-val.exe"):
        candidate = os.path.join(sdk, name)
        if os.path.exists(candidate):
            return candidate
    return None


def spirv_entry_and_workgroup(spirv):
    words = struct.unpack(f"<{len(spirv) // 4}I", spirv)
    assert words[0] == 0x07230203, "not a SPIR-V module"
    entry = None
    workgroup = None
    i = 5
    while i < len(words):
        opcode = words[i] & 0xFFFF
        count = words[i] >> 16
        if opcode == 15:  # OpEntryPoint: model, id, literal name...
            raw = b"".join(struct.pack("<I", w) for w in words[i + 3:i + count])
            entry = raw.split(b"\x00")[0].decode()
        elif opcode == 16 and words[i + 2] == 17:  # OpExecutionMode LocalSize x y z
            workgroup = (words[i + 3], words[i + 4], words[i + 5])
        i += count
    if not entry or not workgroup:
        raise ValueError("SPIR-V module without compute entry point or LocalSize")
    return entry, workgroup


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shaders-dir", required=True, help="NRD _Shaders dir with *.cs.spirv.h")
    parser.add_argument("--tool", help="nrd_msl_cook host binary (macos/ios only)")
    parser.add_argument("--platform", required=True, choices=["macos", "ios", "android"])
    parser.add_argument("--nrd-header", required=True, help="NRD.h for the version guard")
    parser.add_argument("--output", required=True)
    parser.add_argument("--blob-prefix", action="append", required=True)
    parser.add_argument("--signal", default="BOTH")
    args = parser.parse_args()

    if args.platform != "android" and not args.tool:
        parser.error("--tool is required for macos/ios")

    out_dir = os.path.join(args.output, args.platform)
    shutil.rmtree(out_dir, ignore_errors=True)
    os.makedirs(out_dir)

    manifest_lines = ["nrd_version " + " ".join(nrd_version(args.nrd_header))]
    validator = find_spirv_validator() if args.platform == "android" else None
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
                if args.platform == "android":
                    spirv = strip_compute_derivatives(spirv)
                    spirv = downgrade_spirv_to_1_4(spirv, validator)
                with open(spv_path, "wb") as f:
                    f.write(spirv)
                if args.platform == "android":
                    # bindings are reflected on device; only entry + workgroup go in the manifest
                    entry, wg = spirv_entry_and_workgroup(spirv)
                    manifest_lines.append(
                        f"pipeline {stem} {stem}.spv {entry} {wg[0]} {wg[1]} {wg[2]} -1")
                else:
                    metal_path = os.path.join(out_dir, stem + ".metal")
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
