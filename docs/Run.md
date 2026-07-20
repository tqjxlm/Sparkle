# Run The App

## Launching

Always run the app through `run.py` during development for robustness; do not run built binaries directly. See [Build.md](Build.md) for build and run commands.

## File System

There are three types of files:

* Packaged files. Mostly assets. Not intended for user access.
* Internal files. Runtime caches. Not intended for user access.
* External files. Runtime products (user configs, screenshots, and logs). Can be accessed by user. See below.

### External Storage Paths

The base storage path varies by platform:

| Platform | Path                                                                                                          |
| -------- | ------------------------------------------------------------------------------------------------------------- |
| Windows  | executable directory (e.g. `build_system/glfw/output/build/generated`)                                        |
| macOS    | `~/Documents/sparkle/`                                                                                        |
| iOS      | App's Document Directory (simulator test runs copy logs and screenshots to `build_system/ios/output/device/`) |
| Android  | `/sdcard/Android/data/io.tqjxlm.sparkle/files/` (use `adb` to access)                                         |

## Config System

* We adopt a Unreal-like console variable (cvar) system. For each cvar, the override rule is: code default->packaged config->user config->last session (with `--load_last_session`)->commandline argument.
* Use `--[cvar_name]` to set a cvar in command line. Example: `--pipeline gpu --max_spp 512 --scene models/mymodel.usd`
* For platforms that cannot use commandline arguments (i.e. mobile platforms), you must use config files to control cvars.
* Not all cvar can be toggled at runtime.
* Use `--help` to show all cvar.

### Config File

* Default config: copied from resources/packed/config/config.json to final package on every build.
* User config: generated at `<external-storage-path>/config/config.json` on first run of the built package. It overrides the default config. See [External Storage Paths](#external-storage-paths) for the platform-specific base path.

### Important Configs

| cvar                | type   | default    | pipelines   | description                                                                                                                                                           |
| ------------------- | ------ | ---------- | ----------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `pipeline`          | string | `forward`  | all         | Rendering pipeline: `cpu`, `gpu`, `forward`, `deferred`                                                                                                               |
| `headless`          | bool   | false      | all         | Run without creating a window and without input. On iOS it applies only to processes launched with arguments, e.g. the simulator test runner (see [Test.md](Test.md)) |
| `scene`             | string | *(empty)*  | all         | Scene to render. Empty = packaged **TestScene** (the default; also the CI ground-truth scene). Other values = model/scene file path under `resources/models/`         |
| `width` / `height`  | uint   | 1280 / 720 | all         | Render resolution                                                                                                                                                     |
| `render_scale`      | float  | 1.0        | all         | Scene render resolution as a fraction of output resolution, `(0, 1]`. The scene is upsampled to `width`x`height` before UI and present                                |
| `validation`        | bool   | false      | vulkan only | Enable Vulkan validation layers                                                                                                                                       |
| `debug_mode`        | string | *(empty)*  | all         | Renderer debug output mode                                                                                                                                            |
| `thread`            | uint   | 64         | cpu         | Max threads for CPU path tracer                                                                                                                                       |
| `nrd`               | bool   | false      | gpu         | Enable NRD denoiser                                                                                                                                                   |
| `screen_log`        | bool   | true       | all         | On-screen log overlay                                                                                                                                                 |
| `target_framerate`  | float  | 60         | gpu         | Target FPS for dynamic SPP                                                                                                                                            |
| `load_last_session` | bool   | false      | all         | Restore last session (camera, config) on startup                                                                                                                      |
| `clear_screenshots` | bool   | false      | all         | Clear old screenshots in the screenshots directory before taking a new screenshot                                                                                     |
| `rebuild_cache`     | bool   | false      | all         | Force rebuild all cook caches                                                                                                                                         |

Search across the project for keyword "ConfigValue" for more available configs.

## Logs

* For latest running logs, see `<external-storage-path>/logs/output.log`. Backup logs from previous runs are also stored there. See [External Storage Paths](#external-storage-paths) for platform-specific base paths.
* Logs will also be redirected to console when running from command line.
