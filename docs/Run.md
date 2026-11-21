# Run the App

`run.py` is the development entry point: it runs the [pipeline stages](Build.md#pipeline-stages) a launch consumes, then launches the app. Use it instead of launching built binaries directly.

## Launching

``` shell
python3 run.py --framework=<framework> [build options] [app options]
```

Build options go to `build.py`; every other option goes to the app as a config value (see [Config System](#config-system)).

* Desktop runs build and cook, and the app reads the cook output from the build tree.
* Device runs (`android`, `ios`) also package, then install the product on a connected device, which only receives cooked content through its package. The android run reports the device's cook activity after pulling its log.
* `--skip_build` runs no stage and launches the existing binary; on a device it installs the raw build output, which cooks on the fly.

``` shell
# Android debug build on a connected device
python3 run.py --framework android

# iOS release build on a connected device, gpu path tracer
export APPLE_DEVELOPER_TEAM_ID=ABC123DEF4
python3 run.py --framework ios --apple_auto_sign --config Release --pipeline gpu
```

## Storage

The app sees three kinds of files: packaged files (assets), internal files (runtime caches such as cook artifacts) and external files (user config, screenshots and logs), which are the only ones meant for users. External files live under a per-platform base path:

| Platform | External storage path                                                                                              |
| -------- | ------------------------------------------------------------------------------------------------------------------ |
| Windows  | executable directory (e.g. `build_system/glfw/output/build/generated`)                                             |
| macOS    | `~/Documents/sparkle/`                                                                                             |
| iOS      | the app's Documents directory (simulator test runs copy logs and screenshots to `build_system/ios/output/device/`) |
| Android  | `/sdcard/Android/data/io.tqjxlm.sparkle/files/` (use `adb` to access)                                              |

* `logs/output.log` holds the latest run's log, next to backups of earlier runs. Command-line runs also log to the console.
* `config/config.json` is the user config.
* `screenshots/` receives screenshots and render graph dumps.

## Config System

Settings are console variables (cvars), declared in code as `ConfigValue`. Each cvar resolves in this order, later overriding earlier: code default, packaged config ([resources/packed/config/config.json](../resources/packed/config/config.json)), user config (created on first run), last session (with `--load_last_session`), command line.

* Set a cvar on the command line with `--<name> <value>`, e.g. `--pipeline gpu --max_spp 512 --scene models/mymodel.usd`.
* Mobile platforms take no command line: edit the user config instead.
* Not every cvar can change at runtime.
* `--help` lists every cvar.

### General Settings

| cvar                   | default    | description                                                                                                                                                             |
| ---------------------- | ---------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `pipeline`             | `forward`  | rendering pipeline: `cpu`, `gpu`, `forward`, `deferred`                                                                                                                 |
| `scene`                | *(empty)*  | scene file under `resources/`; empty loads the packaged [TestScene](Test.md#ground-truth). A value that is no file path logs an error and renders an empty scene        |
| `headless`             | false      | run without a window and without input. On iOS it applies only to processes launched with arguments, e.g. by the simulator test runner                                  |
| `width` / `height`     | 1280 / 720 | output resolution. A window resize sets it: the window size in UI coordinates on desktop, `height` at the window's aspect ratio on mobile                               |
| `render_scale`         | 1.0        | scene render resolution as a fraction of the output resolution, `(0, 1]`; the scene is upsampled before UI and present                                                  |
| `debug_mode`           | `Color`    | renderer debug output, one of `RenderConfig::DebugMode` (enum values parse case-insensitively)                                                                          |
| `validation`           | false      | Vulkan validation layer (the packaged config turns it on, see [Test.md](Test.md#validation-layer))                                                                      |
| `validate_sync`        | false      | Vulkan synchronization validation; needs `validation`                                                                                                                   |
| `sampler_anisotropy`   | true       | off creates every sampler without anisotropic filtering                                                                                                                 |
| `vulkan.upload_replay` | `auto`     | copy each upload into a new read-only texture again after its frame: `on`, `off`, `auto` (on for the Apple Paravirtual device, see [CI.md](CI.md#hosted-runner-quirks)) |
| `screen_log`           | true       | on-screen log overlay                                                                                                                                                   |
| `load_last_session`    | false      | restore the last session (camera, config) on startup                                                                                                                    |
| `clear_screenshots`    | false      | clear old screenshots before taking a new one                                                                                                                           |
| `rebuild_cache`        | false      | ignore internal cook artifacts and recook (see [Cooking.md](Cooking.md#artifacts))                                                                                      |
| `test_case`            | *(empty)*  | run a test case (see [Test.md](Test.md))                                                                                                                                |

Subsystems document their own settings: [path tracers](PathTracer.md#settings), [denoisers](Denoiser.md#settings), [render graph](RenderGraph.md#knobs).
