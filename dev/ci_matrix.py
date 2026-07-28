"""Generate .github/workflows/ci.yml from one product table.

ci.yml is a fully generated file: never edit it by hand. GitHub's `needs` cannot
target a single matrix cell, so a runtime matrix cannot express the pipeline's
per-product edges (release -> its own build, test -> its own release) without
runners polling for their dependencies. This script instead unrolls the product
table into explicit jobs: every edge is a real `needs`, no runner ever waits, and
every node renders flat as "stage (os, framework, config[, abi])".

python3 dev/ci_matrix.py          # check mode: fails when ci.yml is stale
python3 dev/ci_matrix.py --fix    # rewrite ci.yml in place
"""

import argparse
import csv
import json
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORKFLOW = os.path.join(REPO_ROOT, ".github", "workflows", "ci.yml")

# every product CI ships: the target framework and the runner that builds it.
# the x86_64 android and simulator ios products exist only to feed their test
# cells (hosted runners cannot virtualize arm64 guests or drive physical
# iphones), so they only build the Release the cell tests; the shipping apk
# stays arm64 and the shipping ipa targets devices
PRODUCTS = (
    {"os": "macos-latest", "framework": "macos"},
    {"os": "macos-latest", "framework": "ios"},
    {"os": "macos-latest", "framework": "ios", "abi": "simulator",
     "build_types": ("Release",)},
    # glfw builds Debug on windows and ubuntu already; the macos Debug gate would only
    # add "glfw code that is both apple-specific and Debug-only", which the macos
    # framework's own Debug build covers on the apple side. Release still builds here
    # because macos-glfw-release is a test cell (the Vulkan backend on a real GPU)
    {"os": "macos-latest", "framework": "glfw", "build_types": ("Release",)},
    {"os": "windows-latest", "framework": "glfw"},
    # the linux product targets arm64 — its test cell is a jetson, the only runner in
    # the matrix with hardware ray tracing — but it is cross-compiled on an x86 host,
    # because nothing that runs during the build ships an aarch64 linux binary (no
    # LunarG SDK, no DXC). see cmake/aarch64-linux-toolchain.cmake
    {"os": "ubuntu-latest", "framework": "glfw", "target_arch": "aarch64"},
    {"os": "ubuntu-latest", "framework": "android"},
    {"os": "ubuntu-latest", "framework": "android", "abi": "x86_64",
     "build_types": ("Release",)},
)

# the labels the jetson registers itself under. they must match `./config.sh --labels`
# on the board, or its jobs queue forever instead of failing
JETSON_LABELS = "[self-hosted, linux, ARM64, jetson]"

# must hold every tests/coverage.csv triplet column: that table decides who runs the suite
TEST_RUNNERS = {
    # no GPU on hosted windows runners: software vulkan via lavapipe, which can
    # take the suite close to an hour (see TODO.md), hence the generous timeout
    "windows-glfw-release": {
        "suite_args": "--software",
        "suite_timeout": 120,
        "screenshots": "build_system/glfw/output/build/generated/screenshots/",
    },
    # the arm64 linux product on a self-hosted jetson, whose Tegra GPU is the only one
    # in the matrix that exposes VK_KHR_ray_query. that makes it the only cell able to
    # render the gpu path-tracing pipeline, so it carries gpu_render_static;
    # windows-glfw-release keeps the software-rasterizer coverage this cell used to give
    "ubuntu-glfw-release": {
        "runs_on": JETSON_LABELS,
        "suite_args": "",
        "suite_timeout": 60,
        "screenshots": "build_system/glfw/output/build/generated/screenshots/",
    },
    # physical Metal GPU (the runner class the cook stage relies on). its
    # virtualized device exposes no ray tracing, so the gpu path-tracing pipeline
    # (and NRD) silently falls back to forward and cannot be tested here
    "macos-macos-release": {
        "suite_args": "",
        "suite_timeout": 60,
        "screenshots": "~/Documents/sparkle/screenshots/",
    },
    # the Vulkan backend on a real GPU (via MoltenVK), which windows-glfw only
    # exercises through software rasterization
    "macos-glfw-release": {
        "suite_args": "",
        "suite_timeout": 60,
        "screenshots": "build_system/glfw/output/build/generated/screenshots/",
    },
    # the simulator package as spawned headless processes inside the iOS Simulator
    # (see build_system/ios/build.py); like the android cell, the tested product
    # exists only for this cell because no hosted runner can drive a physical
    # iphone. the simulated GPU exposes no ray tracing, so the gpu pipeline stays
    # local-only. 1565x720 matches the published ios ground-truth captures
    "macos-ios-release": {
        "abi": "simulator",
        "suite_args": "--width 1565 --height 720",
        "suite_timeout": 90,
        "screenshots": "build_system/ios/output/device/screenshots/",
    },
    # the x86_64 android package on a KVM-accelerated headless emulator, whose
    # guest Vulkan device is llvmpipe (the windows cell's driver class): no ray
    # tracing, so the gpu pipeline stays local-only. abi keys the artifact and
    # job names apart from the shipping arm64 product, which no hosted runner
    # can emulate. 1560x720 matches the published android ground-truth captures
    "ubuntu-android-release": {
        "abi": "x86_64",
        "suite_args": "--width 1560 --height 720",
        "suite_timeout": 90,
        "screenshots": "build_system/android/output/device/screenshots/",
    },
}

HEAD = """\
# GENERATED FILE — DO NOT EDIT
# the whole workflow is generated by dev/ci_matrix.py: change that script, then run
# `python3 dev/ci_matrix.py --fix`. the pre-commit hook (wired by build.py) and the
# format job both fail when this file differs from the generator's output.

name: CI

# a version tag runs the same pipeline and additionally assembles a github release
# from the shipped packages (the github-release job below)
on:
  push:
    branches: [main]
    tags: ["v*.*.*"]
  pull_request:
    branches: [main]

jobs:
  changes:
    runs-on: ubuntu-latest
    permissions:
      pull-requests: read
    outputs:
      # a tag has no base to diff against and always ships
      code: ${{ github.ref_type == 'tag' && 'true' || steps.filter.outputs.code }}
    steps:
      - uses: actions/checkout@v7
      - uses: dorny/paths-filter@v4
        id: filter
        if: github.ref_type != 'tag'
        with:
          filters: |
            code:
              - 'libraries/**'
              - 'frameworks/**'
              - 'shaders/**'
              - 'thirdparty/**'
              - 'build_system/**'
              - 'resources/**'
              - 'dev/**'
              - 'tests/**'
              - '.github/**'
              - '.gitmodules'
              - 'CMakeLists.txt'
              - 'cmake/**'
              - 'build.py'
              - 'prerequisites.json'
              - 'cook_targets.json'

  # runs unconditionally: it is cheap and also covers files outside the code filter (e.g. docs)
  format:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v7
      - name: Install formatters
        run: pip install clang-format==22.1.5 autopep8==2.3.2
      - name: Check formatting
        run: python3 dev/check_format.py
      - name: Check generated pipeline
        run: python3 dev/ci_matrix.py

  # clang-tidy needs a compile database, which requires a real CMake configure with
  # dependencies fetched; the glfw configure on ubuntu is the cheapest one CI already
  # exercises, and ubuntu runners are the most available class
  tidy:
    needs: changes
    if: needs.changes.outputs.code == 'true'
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v7
      - name: Setup Environment
        id: setup-environment
        uses: ./.github/actions/setup-environment
        with:
          framework: glfw
          os: ubuntu-latest
      # install into the same interpreter that runs check_tidy.py: the runner's
      # `pip` belongs to another python, and PATH holds setup-cpp's own clang-tidy
      - name: Install clang-tidy
        run: python3 -m pip install --break-system-packages clang-tidy==22.1.7
      # dependency downloads during the configure fail on transient network errors, retry cheaply
      - name: Configure
        shell: bash
        run: |
          for attempt in 1 2 3; do
            python3 build.py --framework glfw --config Release --clangd ${{ steps.setup-environment.outputs.build-args }} && exit 0
            echo "configure attempt ${attempt} failed"
            sleep 20
          done
          exit 1
      - name: Check clang-tidy
        run: python3 dev/check_tidy.py

  # stages: build (every product in parallel, the heavy stage) -> cook (macos-release,
  # on the macos runners' real Metal GPU, one content image per cook target) -> release
  # (swap in the product's own image and re-sign; on ubuntu unless apple signing needs
  # a macos runner) -> test (the
  # products tests/coverage.csv covers). Debug builds are compile gates only: nothing
  # releases or tests them. every edge is a real needs edge; jobs are unrolled from the
  # product table because needs cannot target a single matrix cell.
"""

BUILD_JOB = """
  @id@:
    name: build (@os@, @framework@, @config@@name_abi@)
    needs: changes
    if: @condition@
    runs-on: @os@
    steps:
      - name: Checkout code
        uses: actions/checkout@v7

      - name: Build Platform
        uses: ./.github/actions/build-platform
        with:
          framework: @framework@
          os: @os@
          build-type: @config@
@extras@"""

BUILD_IOS_SECRETS = """\
          ios-certificate-p12-base64: ${{ secrets.APPLE_DEVELOPMENT_CERTIFICATE_P12_BASE64 }}
          ios-certificate-password: ${{ secrets.APPLE_DEVELOPMENT_CERTIFICATE_PASSWORD }}
          ios-team-id: ${{ secrets.APPLE_DEVELOPER_TEAM_ID }}
          ios-appstore-issuer-id: ${{ secrets.APPSTORE_ISSUER_ID }}
          ios-appstore-key-id: ${{ secrets.APPSTORE_KEY_ID }}
          ios-appstore-private-key: ${{ secrets.APPSTORE_PRIVATE_KEY }}
"""

COOK_JOB = """
  @id@:
    name: cook (@os@, @framework@, Release)
    needs: @needs@
    runs-on: @runs_on@
    steps:
      - name: Checkout code
        uses: actions/checkout@v7

      - name: Setup Environment
        id: setup-environment
        uses: ./.github/actions/setup-environment
        with:
          framework: @framework@
          os: @os@
@pre_steps@
      - name: Download build product
        uses: actions/download-artifact@v8
        with:
          name: build-@framework@-@os@-Release
          path: build_system/@framework@/product
@extract_step@@pool_download@
      - name: Cook
        shell: bash
        run: |
          # a hung app never fails the step on its own; kill it so the diagnostics below run
          ( sleep 900 && echo "cook watchdog fired" && pkill -f @app_binary@ ) &
          WATCHDOG=$!
          if ! python3 build.py --framework @framework@ --config Release --stage cook --cook_targets @cook_targets@ ${{ steps.setup-environment.outputs.build-args }}; then
            kill $WATCHDOG 2>/dev/null || true
@diagnostics@\
            exit 1
          fi
          kill $WATCHDOG 2>/dev/null || true
          for target in @cook_target_list@; do
            test -f "@image_dir@/$target/cooked/manifest.json"
          done
@upload_steps@@pool_upload@"""

# one artifact per target: a release node downloads only the image its own product packages
COOK_UPLOAD_STEP = """
      - name: Upload cooked content image (@target@)
        uses: actions/upload-artifact@v7
        with:
          name: cooked-image-@target@
          path: @image_dir@/@target@
"""

STEP_POOL_UPLOAD = """
      # the fp16 masters the encode-only families derive their own textures from: cooking
      # them needs a GPU, encoding from them does not
      - name: Upload artifact pool
        uses: actions/upload-artifact@v7
        with:
          name: cooked-pool
          path: @pool_dir@
"""

STEP_POOL_DOWNLOAD = """
      # seeding the app's internal storage makes every master a cache hit, so this node
      # runs no integration and needs no gpu
      - name: Download artifact pool
        uses: actions/download-artifact@v8
        with:
          name: cooked-pool
          path: @pool_dir@
"""

COOK_DIAGNOSTICS_APPLE = """\
            echo "=== app logs ==="; cat ~/Documents/sparkle/logs/*.log 2>/dev/null || true
            echo "=== crash reports ==="
            sleep 10
            for f in ~/Library/Logs/DiagnosticReports/sparkle*; do echo "--- $f"; cat "$f"; done 2>/dev/null || true
"""

COOK_DIAGNOSTICS_LINUX = """\
            echo "=== app logs ==="
            cat ~/Documents/sparkle/logs/*.log 2>/dev/null || true
            cat build_system/glfw/output/build/generated/logs/*.log 2>/dev/null || true
"""

RELEASE_JOB = """
  @id@:
    name: release (@os@, @framework@, Release@name_abi@)
    needs: [@build_id@, @cook_id@]
    runs-on: @runs_on@
    steps:
      - name: Checkout code
        uses: actions/checkout@v7

      - name: Release Platform
        uses: ./.github/actions/release-platform
        with:
          framework: @framework@
          os: @os@
          build-type: Release
@extras@\
          macos-certificate-p12-base64: ${{ secrets.APPLE_DEVELOPER_CERTIFICATE_P12_BASE64 }}
          macos-certificate-password: ${{ secrets.APPLE_DEVELOPER_CERTIFICATE_PASSWORD }}
          macos-signing-identity: ${{ secrets.APPLE_SIGNING_IDENTITY }}
          macos-notarization-username: ${{ secrets.APPLE_NOTARIZATION_USERNAME }}
          macos-notarization-password: ${{ secrets.APPLE_NOTARIZATION_PASSWORD }}
          apple-team-id: ${{ secrets.APPLE_DEVELOPER_TEAM_ID }}
          ios-certificate-p12-base64: ${{ secrets.APPLE_DEVELOPMENT_CERTIFICATE_P12_BASE64 }}
          ios-certificate-password: ${{ secrets.APPLE_DEVELOPMENT_CERTIFICATE_PASSWORD }}
          ios-appstore-issuer-id: ${{ secrets.APPSTORE_ISSUER_ID }}
          ios-appstore-key-id: ${{ secrets.APPSTORE_KEY_ID }}
          ios-appstore-private-key: ${{ secrets.APPSTORE_PRIVATE_KEY }}
"""

TEST_JOB_HEAD = """
  @id@:
    name: test (@os@, @framework@, Release@name_abi@)
    needs: @release_id@
@condition@    runs-on: @runs_on@
    steps:
      - name: Checkout code
        uses: actions/checkout@v7
"""

# a self-hosted cell executes whatever the workflow tells it to on someone's own
# hardware, so a fork's pull request must never reach it. the maintainer's own branches
# still do, which is what keeps this a pre-merge gate rather than a post-merge one
SELF_HOSTED_CONDITION = """\
    if: github.event_name == 'push' ||
        github.event.pull_request.head.repo.full_name == github.repository
"""

STEP_SETUP_ENV = """
      # the suite drives the app through build.py, which needs the build prerequisites;
      # the android cell needs the SDK for adb and the emulator
      - name: Setup Environment
        uses: ./.github/actions/setup-environment
        with:
          framework: @framework@
          os: @os@
"""

STEP_MESA = """
      - name: Setup Mesa Lavapipe
        uses: ./.github/actions/setup-mesa
"""

STEP_GLFW_RUNTIME = """
      # the released linux binary links libglfw at runtime even in headless mode
      - name: Install GLFW runtime
        shell: bash
        run: |
          sudo apt-get update -qq
          sudo apt-get install -y libglfw3
"""

STEP_KVM = """
      # the emulator needs hardware virtualization; hosted ubuntu runners expose
      # /dev/kvm but leave it root-only
      - name: Enable KVM
        shell: bash
        run: |
          echo 'KERNEL=="kvm", GROUP="kvm", MODE="0666", OPTIONS+="static_node=kvm"' | sudo tee /etc/udev/rules.d/99-kvm4all.rules
          sudo udevadm control --reload-rules
          sudo udevadm trigger --name-match=kvm

      # the AVD is deliberately NOT cached: the snapshot entry weighs ~4GB, and duplicated
      # per branch it evicts every compiler cache in the repo's 10GB actions-cache quota
      # (a cold windows rebuild costs ~20 minutes); the suite seeds a fresh AVD in ~5
      # minutes instead (cold boot + clean shutdown)
"""

STEP_DOWNLOAD = """
      # the release package already carries the cooked content in its packed resources
      - name: Download app
        uses: actions/download-artifact@v8
        with:
          name: release-@framework@-@os@-Release@artifact_abi@
          path: build_system/@framework@/product
"""

EXTRACT_COMMANDS = {
    "macos-latest": "ditto -x -k @archive@ @extract_dir@/",
    "ubuntu-latest": "unzip -q @archive@ -d @extract_dir@/",
    "ubuntu-24.04-arm": "unzip -q @archive@ -d @extract_dir@/",
    "windows-latest": "python3 -m zipfile -e @archive@ @extract_dir@/",
}

STEP_EXTRACT = """
      # extraction must preserve the executable bit of the binary, which python's zipfile
      # drops; only the windows runner, whose filesystem carries no such bit, may use it
      - name: Extract app
        shell: bash
        run: |
          mkdir -p @extract_dir@
          @extract_command@
"""

STEP_RUN_TESTS = """
      # the step timeout bounds a hung app; the log dump below then still runs
      - name: Run Tests
        timeout-minutes: @suite_timeout@
        shell: bash
        env:
          # the hosted macos runners' paravirtual GPU silently renders MTLHeap-placed resources
          # as solid magenta through MoltenVK; force dedicated allocations instead. no effect on
          # cells that do not run MoltenVK
          MVK_CONFIG_USE_MTLHEAP: 0
          # the suite nests python processes whose block-buffered output would otherwise
          # reach the live log minutes late and out of order
          PYTHONUNBUFFERED: 1
        run: python3 dev/run_tests.py --framework @framework@ --config Release@suite_args@
"""

STEP_DUMP_ANDROID = """
      - name: Dump app logs
        if: failure()
        shell: bash
        run: |
          echo "=== app logs ==="
          cat build_system/android/output/device/logs/*.log 2>/dev/null || true
          echo "=== emulator log ==="
          cat build_system/android/output/emulator.log 2>/dev/null || true
"""

STEP_DUMP_IOS = """
      - name: Dump app logs
        if: failure()
        shell: bash
        run: |
          echo "=== app logs ==="
          cat build_system/ios/output/device/logs/*.log 2>/dev/null || true
"""

STEP_DUMP_LOGS = """
      - name: Dump app logs
        if: failure()
        shell: bash
        run: |
          echo "=== app logs ==="
          cat ~/Documents/sparkle/logs/*.log 2>/dev/null || true
          cat build_system/glfw/output/build/generated/logs/*.log 2>/dev/null || true
"""

STEP_DUMP_CRASH = """
      # the sleep gives ReportCrash time to land the report in DiagnosticReports
      - name: Dump crash reports
        if: failure()
        shell: bash
        run: |
          sleep 10
          for f in ~/Library/Logs/DiagnosticReports/sparkle*; do echo "--- $f"; cat "$f"; done 2>/dev/null || true
"""

STEP_UPLOAD_SCREENSHOTS = """
      - name: Upload Test Screenshot
        if: always()
        uses: actions/upload-artifact@v7
        with:
          name: test-screenshots-@framework@-@os@
          path: @screenshots@
"""

GITHUB_RELEASE_JOB = """
  # pushing a version tag assembles a github release from the shipped packages
  github-release:
    name: github release
    if: github.ref_type == 'tag'
    needs: [@release_ids@]
    runs-on: ubuntu-latest
    permissions:
      contents: write
    steps:
      - name: Download all artifacts
        uses: actions/download-artifact@v8
        with:
          path: artifacts

      - name: Create Release
        uses: softprops/action-gh-release@v3
        with:
          prerelease: ${{ contains(github.ref_name, '-alpha') || contains(github.ref_name, '-beta') || contains(github.ref_name, '-rc') }}
          generate_release_notes: true
          # release-stage products only: build products lack cooked content, and CI also
          # uploads cook artifacts and test screenshots
          files: |
            artifacts/release-*/**/*.zip
            artifacts/release-*/**/*.apk
            artifacts/release-*/**/*.ipa
"""

GATE_JOB = """
  ci:
    if: always()
    needs: [@all_ids@]
    runs-on: ubuntu-latest
    steps:
      - run: exit 1
        if: contains(needs.*.result, 'failure') || contains(needs.*.result, 'cancelled')
"""


def covered_triplets():
    """The triplet columns of the coverage table (its rows pick the cases)."""
    with open(os.path.join(REPO_ROOT, "tests", "coverage.csv"), newline="") as coverage_file:
        header = next(csv.reader(coverage_file))
    return [column for column in header if column != "case"]


def host(product):
    return RUNNER_HOST[product["os"]]


# runner labels say ubuntu, cook targets say linux
RUNNER_SYSTEM = {"macos-latest": "macos", "windows-latest": "windows",
                 "ubuntu-latest": "linux", "ubuntu-24.04-arm": "linux"}

# the triplet name a runner reports itself as. dev/run_tests.py derives the same name
# from sys.platform on the machine running the suite, so both arm64 and x86 linux answer
# to "ubuntu" and tests/coverage.csv keeps one linux column
RUNNER_HOST = {"macos-latest": "macos", "windows-latest": "windows",
               "ubuntu-latest": "ubuntu", "ubuntu-24.04-arm": "ubuntu"}


def product_cook_target(product):
    if product["framework"] != "glfw":
        return product["framework"]
    return f"{RUNNER_SYSTEM[product['os']]}-glfw"


def cook_targets():
    """Every cook-target identity the shipped products need, in stable order."""
    targets = []
    for product in PRODUCTS:
        target = product_cook_target(product)
        if target not in targets:
            targets.append(target)
    return targets


def load_cook_target_families():
    """The cook target -> texture family table. cook_targets.json is the one source: CMake
    compiles it into the engine and build.py validates --cook_targets against it."""
    with open(os.path.join(REPO_ROOT, "cook_targets.json"), encoding="utf-8") as table_file:
        return json.load(table_file)


COOK_TARGET_FAMILY = load_cook_target_families()

# the node that cooks each family. one family produces the fp16 masters (the expensive
# gpu work) and every other family consumes them, which is pure cpu encoding: consumers
# leave the scarce macos runners and stop gating the products that never read them.
# app_binary is what the watchdog kills, pool_dir the app's internal storage the masters
# travel through (COOKED_OUTPUT_DIR in build.py)
COOK_FAMILIES = {
    "astc": {
        "id": "cook",
        "os": "macos-latest",
        "framework": "macos",
        "role": "produce",
        "app_binary": "sparkle.app/Contents/MacOS/sparkle",
        "pool_dir": "build_system/macos/output/build/sparkle.app/Contents/SharedSupport/cooked",
        "diagnostics": COOK_DIAGNOSTICS_APPLE,
    },
    "bc": {
        "id": "cook-bc",
        "os": "ubuntu-latest",
        # this node encodes by running the linux product's own binary, which is arm64
        # even though it is built on x86, so it executes on an arm64 host. the cooked
        # output it produces is architecture-independent
        "runs_on": "ubuntu-24.04-arm",
        "framework": "glfw",
        "role": "consume",
        "app_binary": "build_system/glfw/output/build/sparkle",
        "pool_dir": "build_system/glfw/output/build/generated/cooked",
        "diagnostics": COOK_DIAGNOSTICS_LINUX,
        "pre_steps": STEP_GLFW_RUNTIME,
    },
}


def cook_family(target):
    family = COOK_TARGET_FAMILY.get(target)
    if family is None:
        raise LookupError(f"cook target {target} has no family; update COOK_TARGET_FAMILY")
    return family


def cook_product(family):
    """The product whose build output cooks this family."""
    spec = COOK_FAMILIES[family]
    return {"os": spec["os"], "framework": spec["framework"]}


def cook_image_dir(family):
    return f"build_system/{COOK_FAMILIES[family]['framework']}/output/cooked_image"


def master_producing_family():
    producers = [family for family, spec in COOK_FAMILIES.items() if spec["role"] == "produce"]
    if len(producers) != 1:
        raise LookupError(f"exactly one cook family must produce the masters, got {producers}")
    return producers[0]


def family_cook_targets(family):
    return [target for target in cook_targets() if cook_family(target) == family]


def slug(stage, product, config=None):
    parts = [stage, host(product), product["framework"], product.get("abi"),
             config.lower() if config else None]
    return "-".join(part for part in parts if part)


def name_abi(product):
    return f", {product['abi']}" if "abi" in product else ""


def render(template, **tokens):
    text = template
    for token, value in tokens.items():
        text = text.replace(f"@{token}@", str(value))
    return text


def extract_step(product):
    """Unpack a downloaded archive of this product. The macos app bundle lands inside the
    build directory, a glfw binary at the output root."""
    framework = product["framework"]
    directory = f"build_system/{framework}/output"
    if framework == "macos":
        directory += "/build"
    archive = f"build_system/{framework}/product/{product_cook_target(product)}-Release.zip"
    command = render(EXTRACT_COMMANDS[product["os"]], archive=archive, extract_dir=directory)
    return render(STEP_EXTRACT, extract_dir=directory, extract_command=command)


def cook_job(family):
    spec = COOK_FAMILIES[family]
    product = cook_product(family)
    targets = family_cook_targets(family)
    image_dir = cook_image_dir(family)

    build_id = slug("build", product, "Release")
    if spec["role"] == "produce":
        needs = build_id
        pool_download, pool_upload = "", render(STEP_POOL_UPLOAD, pool_dir=spec["pool_dir"])
    else:
        needs = f"[{COOK_FAMILIES[master_producing_family()]['id']}, {build_id}]"
        pool_download, pool_upload = render(STEP_POOL_DOWNLOAD, pool_dir=spec["pool_dir"]), ""

    upload_steps = "".join(render(COOK_UPLOAD_STEP, target=target, image_dir=image_dir)
                           for target in targets)

    return render(COOK_JOB, id=spec["id"], os=spec["os"], framework=spec["framework"],
                  runs_on=spec.get("runs_on", spec["os"]),
                  needs=needs, pre_steps=spec.get("pre_steps", ""),
                  extract_step=extract_step(product), pool_download=pool_download,
                  app_binary=spec["app_binary"], diagnostics=spec["diagnostics"],
                  cook_targets="+".join(targets), cook_target_list=" ".join(targets),
                  image_dir=image_dir, upload_steps=upload_steps, pool_upload=pool_upload)


def build_job(product, config):
    extras = ""
    if "abi" in product:
        extras += f"          abi: {product['abi']}\n"
    if "target_arch" in product:
        extras += f"          target-arch: {product['target_arch']}\n"
    # the simulator product builds unsigned, so it needs no signing secrets
    if product["framework"] == "ios" and "abi" not in product:
        extras += BUILD_IOS_SECRETS
    condition = "needs.changes.outputs.code == 'true'"
    if config == "Debug":
        condition += " && github.ref_type != 'tag'"
    return render(BUILD_JOB, id=slug("build", product, config), os=product["os"],
                  framework=product["framework"], config=config,
                  name_abi=name_abi(product), condition=condition, extras=extras)


def release_job(product):
    apple = product["framework"] in ("macos", "ios")
    extras = f"          abi: {product['abi']}\n" if "abi" in product else ""
    return render(RELEASE_JOB, id=slug("release", product), os=product["os"],
                  framework=product["framework"], name_abi=name_abi(product),
                  build_id=slug("build", product, "Release"),
                  cook_id=COOK_FAMILIES[cook_family(product_cook_target(product))]["id"],
                  runs_on="macos-latest" if apple else "ubuntu-latest", extras=extras)


def test_job(product, runner):
    framework, os_name = product["framework"], product["os"]
    # a self-hosted cell is a machine someone maintains: its GPU driver and runtime
    # libraries are installed once, not reinstalled per job, and it needs no
    # software rasterizer because it has a real GPU
    self_hosted = "runs_on" in runner
    linux_glfw = framework == "glfw" and RUNNER_SYSTEM[os_name] == "linux"
    text = render(TEST_JOB_HEAD, id=slug("test", product), os=os_name,
                  framework=framework, name_abi=name_abi(product),
                  release_id=slug("release", product),
                  runs_on=runner.get("runs_on", os_name),
                  condition=SELF_HOSTED_CONDITION if self_hosted else "")
    if os_name == "macos-latest" or framework == "android":
        text += render(STEP_SETUP_ENV, framework=framework, os=os_name)
    if not self_hosted and (os_name == "windows-latest" or linux_glfw):
        text += STEP_MESA
    if linux_glfw and not self_hosted:
        text += STEP_GLFW_RUNTIME
    if framework == "android":
        text += STEP_KVM
    text += render(STEP_DOWNLOAD, framework=framework, os=os_name,
                   artifact_abi=f"-{product['abi']}" if "abi" in product else "")
    if framework in ("glfw", "macos"):
        text += extract_step(product)
    suite_args = f" {runner['suite_args']}" if runner["suite_args"] else ""
    text += render(STEP_RUN_TESTS, suite_timeout=runner["suite_timeout"],
                   framework=framework, suite_args=suite_args)
    if framework == "android":
        text += STEP_DUMP_ANDROID
    elif framework == "ios":
        # spawned simulator processes crash-report on the host like any other process
        text += STEP_DUMP_IOS + STEP_DUMP_CRASH
    elif os_name == "macos-latest" or linux_glfw:
        text += STEP_DUMP_LOGS
        if os_name == "macos-latest":
            text += STEP_DUMP_CRASH
    text += render(STEP_UPLOAD_SCREENSHOTS, framework=framework, os=os_name,
                   screenshots=runner["screenshots"])
    return text


def suite_runner(product):
    """The product's suite config when tests/coverage.csv covers it.
    A TEST_RUNNERS abi picks which product of the triplet runs the suite."""
    triplet = f"{host(product)}-{product['framework']}-release"
    if triplet not in covered_triplets():
        return None
    runner = TEST_RUNNERS[triplet]
    if runner.get("abi", "") != product.get("abi", ""):
        return None
    return runner


def tested_triplet(product):
    return f"{host(product)}-{product['framework']}-release"


def jobs():
    """Ordered (id, text) pairs of the generated region."""
    generated = []
    tested = set()
    for product in PRODUCTS:
        for config in product.get("build_types", ("Debug", "Release")):
            generated.append((slug("build", product, config),
                              build_job(product, config)))
    for family, spec in COOK_FAMILIES.items():
        generated.append((spec["id"], cook_job(family)))
    for product in PRODUCTS:
        if "Release" not in product.get("build_types", ("Release",)):
            continue
        generated.append((slug("release", product), release_job(product)))
        runner = suite_runner(product)
        if runner:
            generated.append((slug("test", product), test_job(product, runner)))
            tested.add(tested_triplet(product))
    untested = set(covered_triplets()) - tested
    if untested:
        raise LookupError(f"tests/coverage.csv triplets without a product: {sorted(untested)}")
    release_ids = [job_id for job_id, _ in generated if job_id.startswith("release-")]
    generated.append(("github-release",
                      render(GITHUB_RELEASE_JOB, release_ids=", ".join(release_ids))))
    all_ids = ["changes", "format", "tidy"] + [job_id for job_id, _ in generated]
    generated.append(("ci", render(GATE_JOB, all_ids=", ".join(all_ids))))
    return generated


def generate():
    return HEAD + "".join(text for _, text in jobs())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fix", action="store_true",
                        help="rewrite ci.yml instead of checking it")
    args = parser.parse_args()

    if args.fix:
        with open(WORKFLOW, "w", encoding="utf-8", newline="\n") as workflow_file:
            workflow_file.write(generate())
        print(f"regenerated {WORKFLOW}")
        return
    with open(WORKFLOW, encoding="utf-8") as workflow_file:
        current = workflow_file.read()
    if current != generate():
        sys.exit(f"{WORKFLOW} does not match its generator:"
                 " regenerate with python3 dev/ci_matrix.py --fix")


if __name__ == "__main__":
    main()
