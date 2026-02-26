# Run The App

## File System

There are three types of files:

* Packaged files. Mostly assets. Not intended for user access.
* Internal files. Runtime caches. Not intended for user access.
* External files. Runtime products (user configs, screenshots, and logs). Can be accessed by user. See below.

### External Storage Paths

The base storage path varies by platform:

| Platform | Path                                                                   |
| -------- | ---------------------------------------------------------------------- |
| Windows  | executable directory (e.g. `build_system/glfw/output/build/generated`) |
| macOS    | `~/Documents/sparkle/`                                                 |
| iOS      | App's Document Directory (use `ios-deploy` to access)                  |
| Android  | `/sdcard/Android/data/io.tqjxlm.sparkle/files/` (use `adb` to access)  |

## Config System

* We adopt a Unreal-like console variable (cvar) system. For each cvar, the override rule is: code default->packaged config->user config->commandline argument.
* Use `--[cvar_name]` to set a cvar in command line. Example: `--pipeline gpu --max-spp 512 --scene models/mymodel.usd`
* For platforms that cannot use commanline arguments (i.e. mobile platforms), you must use config files to control cvars.
* Not all cvar can be toggled at runtime.
* Use `--help` to show all cvar.

### Config File

* Default config: copied from resources/config/config.json to final package on every build.
* User config: generated at `<external-storage-path>/config/config.json` on first run of the built package. It overrides the default config. See [External Storage Paths](#external-storage-paths) for the platform-specific base path.

### Important Configs

| cvar                | type   | default    | pipelines         | description                                                                                                  |
| ------------------- | ------ | ---------- | ----------------- | ------------------------------------------------------------------------------------------------------------ |
| `pipeline`          | string | `forward`  | all               | Rendering pipeline: `cpu`, `gpu`, `forward`, `deferred`                                                      |
| `scene`             | string | *(empty)*  | all               | Scene to render. Empty = default test scene. Other values = path under `resources/models/`                   |
| `width` / `height`  | uint   | 1280 / 720 | all               | Render resolution                                                                                            |
| `max-spp`           | uint   | 2048       | cpu, gpu          | Max accumulated samples per pixel                                                                            |
| `spp`               | uint   | 1          | cpu, gpu          | Rays per sample per frame                                                                                    |
| `bounce`            | uint   | 8          | cpu, gpu          | Max ray bounces per path                                                                                     |
| `thread`            | uint   | 64         | cpu               | Max threads for CPU path tracer                                                                              |
| `validation`        | bool   | false      | vulkan only       | Enable Vulkan validation layers                                                                              |
| `vsync`             | bool   | false      | all               | Enable vsync                                                                                                 |
| `ssao`              | bool   | false      | forward, deferred | Enable SSAO                                                                                                  |
| `diffuse_ibl`       | bool   | true       | forward, deferred | Enable diffuse IBL                                                                                           |
| `specular_ibl`      | bool   | true       | forward, deferred | Enable specular IBL                                                                                          |
| `spatial_denoise`   | bool   | false      | gpu               | Spatial denoise post-process                                                                                 |
| `reblur_hit_distance_reconstruction_mode` | uint | 1 | gpu | ReBLUR hit-distance reconstruction mode (`0`=off, `1`=3x3, `2`=5x5)                                        |
| `enable_nee`        | bool   | false      | gpu               | Next event estimation                                                                                        |
| `debug_mode`        | string | *(empty)*  | all               | Renderer debug output mode                                                                                   |
| `screen_log`        | bool   | true       | all               | On-screen log overlay                                                                                        |
| `rebuild_cache`     | bool   | false      | all               | Force rebuild all cook caches                                                                                |
| `target_framerate`  | float  | 60         | gpu               | Target FPS for dynamic SPP                                                                                   |
| `load_last_session` | bool   | false      | all               | Restore last session (camera, config) on startup                                                             |
| `clear_screenshots` | bool   | false      | all               | Clear old screenshots in the screenshots directory before taking a new screenshot                            |
| `headless`          | bool   | false      | all               | Run without creating a window and without input (desktop GLFW and macOS frameworks; not supported on mobile) |

Search across the project for keyword "ConfigValue" for more available configs.

## Logs

* For latest running logs, see `<external-storage-path>/logs/output.log`. Backup logs from previous runs are also stored there. See [External Storage Paths](#external-storage-paths) for platform-specific base paths.
* Logs will also be redirected to console when running from command line.
