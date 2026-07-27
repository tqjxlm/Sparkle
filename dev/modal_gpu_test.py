"""Run the aggregate suite on a rented NVIDIA GPU through Modal.

The hosted ubuntu runners have no GPU, so the linux cell used to drive the
released package through Mesa lavapipe. Lavapipe does advertise the ray tracing
extensions, but it traverses acceleration structures on the CPU, which puts the
gpu path-tracing pipeline far outside any CI time budget. Modal rents a real
NVIDIA GPU per second, and the same linux package drives it through the very
same Vulkan backend.

The CI job itself stays on the free hosted runner: it downloads and extracts the
release package exactly as before, then this script ships the repository (the
extracted app included) to a GPU container, runs dev/run_tests.py there, and
brings the screenshots and logs back, so the surrounding dump and upload steps
behave as they do on a local runner.

Authentication reads MODAL_TOKEN_ID and MODAL_TOKEN_SECRET from the environment.

    modal run dev/modal_gpu_test.py::probe    # report the container's Vulkan device
    modal run dev/modal_gpu_test.py::main     # run the suite

The probe is the first thing to run against a new Modal account or after an image
change: serverless GPU platforms hand out compute-only driver capabilities by
default, which leaves the Vulkan ICD out of the container even though the GPU
itself is present.
"""

import io
import json
import os
import pathlib
import subprocess
import sys
import tarfile

import modal

REPO = pathlib.Path(__file__).resolve().parents[1]
REMOTE_REPO = "/repo"

# T4 is the cheapest GPU that carries RT cores, so it is the natural fit for a
# gate whose subject is correctness rather than throughput
GPU = os.environ.get("SPARKLE_MODAL_GPU", "T4")

# the container runs the app, it never builds it: sources, submodules and the
# archive we already extracted are all dead weight in the upload
IGNORE = [
    ".git/**",
    "thirdparty/**",
    "build_cache/**",
    "build_system/*/product/**",
]

# where the suite leaves the evidence the CI steps expect to find
OUTPUT_DIRS = (
    "build_system/glfw/output/build/generated/screenshots",
    "build_system/glfw/output/build/generated/logs",
)

ICD_DIR = pathlib.Path("/usr/share/vulkan/icd.d")

image = (
    modal.Image.debian_slim(python_version="3.12")
    .apt_install(
        # the vulkan loader, and vulkaninfo for the probe and the device assertion
        "libvulkan1", "vulkan-tools",
        # the released linux binary links libglfw at runtime even headless
        "libglfw3",
        "libgl1", "libx11-6", "libxrandr2", "libxinerama1", "libxcursor1", "libxi6",
        "curl", "ca-certificates",
    )
    .env({
        # the container runtime injects the vulkan ICD only for a graphics-capable
        # container; ensure_nvidia_icd below covers the platforms that ignore this
        "NVIDIA_DRIVER_CAPABILITIES": "all",
        # the suite nests python processes whose block-buffered output would
        # otherwise reach the live log minutes late and out of order
        "PYTHONUNBUFFERED": "1",
    })
    .add_local_dir(REPO, REMOTE_REPO, ignore=IGNORE)
)

app = modal.App("sparkle-gpu-test", image=image)


def _driver_library():
    """Absolute path of the NVIDIA GLX/Vulkan driver library the runtime mounted."""
    candidates = [
        "/usr/lib/x86_64-linux-gnu/libGLX_nvidia.so.0",
        "/usr/lib64/libGLX_nvidia.so.0",
    ]
    for candidate in candidates:
        if os.path.exists(candidate):
            return candidate

    cache = subprocess.run(["ldconfig", "-p"], capture_output=True, text=True).stdout
    for line in cache.splitlines():
        if "libGLX_nvidia.so.0" in line and "=>" in line:
            return line.split("=>")[-1].strip()
    return None


def ensure_nvidia_icd():
    """Point the Vulkan loader at the NVIDIA driver, writing the manifest if needed.

    The NVIDIA container runtime installs the ICD manifest only when the container
    asks for graphics capabilities, and serverless GPU platforms commonly grant the
    compute-only set regardless of what the image requests. The driver library is
    mounted either way, so the manifest the loader wants can simply be written.
    Returns the manifest path, or None when no NVIDIA driver is present at all."""
    existing = sorted(ICD_DIR.glob("*nvidia*.json")) if ICD_DIR.is_dir() else []
    if existing:
        return str(existing[0])

    library = _driver_library()
    if library is None:
        return None

    ICD_DIR.mkdir(parents=True, exist_ok=True)
    manifest = ICD_DIR / "nvidia_icd.json"
    manifest.write_text(json.dumps({
        "file_format_version": "1.0.0",
        "ICD": {"library_path": library, "api_version": "1.3.277"},
    }))
    print(f"wrote vulkan ICD manifest for {library}", flush=True)
    return str(manifest)


def vulkan_summary():
    """vulkaninfo's device summary, or the failure that stopped it."""
    result = subprocess.run(["vulkaninfo", "--summary"],
                            capture_output=True, text=True)
    return result.stdout if result.returncode == 0 else (
        f"vulkaninfo failed (exit {result.returncode})\n{result.stderr}")


def device_report():
    """What the loader ends up with: the device name and its ray tracing story."""
    manifest = ensure_nvidia_icd()
    summary = vulkan_summary()

    extensions = subprocess.run(["vulkaninfo"], capture_output=True, text=True).stdout
    names = [line.split("=", 1)[1].strip()
             for line in summary.splitlines() if "deviceName" in line]
    return {
        "icd": manifest,
        "summary": summary,
        "devices": names,
        "ray_query": "VK_KHR_ray_query" in extensions,
        "acceleration_structure": "VK_KHR_acceleration_structure" in extensions,
    }


def require_gpu_device(report):
    """Fail loudly rather than test the wrong thing.

    A missing ICD leaves the loader with no device at all, and a stray software
    ICD leaves it with llvmpipe — which would quietly turn this cell back into the
    lavapipe cell it replaced, at GPU prices. Neither shows up in an exit code."""
    if not report["devices"]:
        raise RuntimeError(
            "no vulkan device in the container: the NVIDIA ICD is missing and could"
            f" not be reconstructed.\n{report['summary']}")

    software = [name for name in report["devices"] if "llvmpipe" in name.lower()]
    if software:
        raise RuntimeError(
            f"vulkan resolved to a software device {software}: the point of this"
            f" runner is the physical GPU.\n{report['summary']}")

    if not (report["ray_query"] and report["acceleration_structure"]):
        raise RuntimeError(
            "the device exposes no ray query / acceleration structure support, so the"
            f" gpu pipeline would silently fall back to forward.\n{report['summary']}")


def collect_outputs():
    """Tar the screenshots and logs so the caller's CI steps can find them."""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
        for relative in OUTPUT_DIRS:
            path = pathlib.Path(REMOTE_REPO) / relative
            if path.is_dir():
                archive.add(path, arcname=relative)
    return buffer.getvalue()


@app.function(gpu=GPU, timeout=3600)
def probe_device():
    """Report the container's Vulkan device without running anything else."""
    return device_report()


@app.function(gpu=GPU, timeout=3600)
def run_suite(suite_args):
    """Run the aggregate suite against the mounted package, on the GPU."""
    report = device_report()
    print(report["summary"], flush=True)
    require_gpu_device(report)

    command = [sys.executable, "dev/run_tests.py",
               "--framework", "glfw", "--config", "Release", *suite_args]
    print(f"=== {' '.join(command)}", flush=True)
    code = subprocess.run(command, cwd=REMOTE_REPO).returncode
    return code, collect_outputs()


@app.local_entrypoint()
def probe():
    """Print the rented device's Vulkan capabilities and stop."""
    report = probe_device.remote()
    print(report["summary"])
    print(f"ICD manifest: {report['icd']}")
    print(f"ray query: {report['ray_query']},"
          f" acceleration structure: {report['acceleration_structure']}")
    require_gpu_device(report)
    print("PASS: the container holds a ray tracing capable GPU")


@app.local_entrypoint()
def main(suite_args: str = ""):
    """Run the suite remotely, then land its screenshots and logs in the work tree."""
    code, payload = run_suite.remote(suite_args.split())

    with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as archive:
        archive.extractall(REPO, filter="data")
    print(f"restored {', '.join(OUTPUT_DIRS)}", flush=True)

    if code:
        raise SystemExit(code)
