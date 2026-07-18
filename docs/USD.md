# USD Import & Export

Sparkle can load USD scenes (`.usda` / `.usdc` / `.usdz`) and export the active scene back to `.usda`. Both directions go through [tinyusdz](https://github.com/lighttransport/tinyusdz) and are designed to round-trip each other: `USDExporter` writes exactly the prims `USDLoader` understands.

* Import: [libraries/source/io/scene/USDLoader.cpp](../libraries/source/io/scene/USDLoader.cpp)
* Export: [libraries/source/io/scene/USDExporter.cpp](../libraries/source/io/scene/USDExporter.cpp)

## Scene <-> USD mapping

| Scene object                        | USD prim                                                          |
| ----------------------------------- | ----------------------------------------------------------------- |
| `SceneNode`                         | `Xform` with a single `xformOp:transform` matrix                  |
| `MeshPrimitive` / `SpherePrimitive` | child `GeomMesh` (points, vertex normals, `primvars:st`, `primvars:tangents`) |
| `Material` (PBR family)             | `Material` + `UsdPreviewSurface` under the root `/_materials` scope |
| `DieletricMaterial`                 | `UsdPreviewSurface` with `opacity = 0` and `ior = eta`            |
| material textures                   | `UsdUVTexture` + `UsdPrimvarReader_float2`, image files under `textures/` next to the `.usda` |
| `DirectionalLight`                  | child `DistantLight` (color carries intensity, direction from the parent `Xform`) |
| `SkyLight`                          | child `DomeLight` (`inputs:texture:file` for sky maps, color-only otherwise) |
| `CameraComponent`                   | child `GeomCamera` (`focalLength`/`verticalAperture` in mm, `fStop`, `focusDistance`) |

Notes:

* The stage is authored Z-up with 1 meter per unit, matching the engine's conventions.
* Prim names are sanitized to valid USD identifiers; the original node name is preserved in the `displayName` metadata and restored on import.
* `SpherePrimitive` exports its tessellated unit-sphere mesh, so it reimports as a `MeshPrimitive`. Ray-traced silhouettes of reimported spheres are therefore triangle meshes, not analytic spheres.
* All textures are written next to the exported file (`textures/`), so an export is self-contained and can be copied around as a directory.
* On import, an orbit camera is reconstructed from the camera `Xform` and `focusDistance`: the orbit center is assumed to be the focus point.
* Asset references inside a USD file are resolved in the same storage domain as the file itself (packaged resource vs. generated file), see `Path` in [libraries/include/core/Path.h](../libraries/include/core/Path.h).

## Known limitations

* Only triangle meshes with a material are exported. Meshes without a material are skipped (`USDLoader` also skips them on import).
* Only one camera per scene is supported (the main camera).
* Animated attributes, skeletons and point/area lights are not supported in either direction.
* Export output is `.usda` only. tinyusdz's binary (`.usdc`) writer is experimental.
* On import, any `UsdPreviewSurface` with a constant `opacity < 1` becomes a `DieletricMaterial` (glass) with `eta = ior`. Alpha-blended materials are not supported.
* `UsdPreviewSurface` has a single input per channel, so a texture is exported without its constant factor (`base_color` / `emissive_color`). Factors other than 1 are lost in a round trip.
* Exported materials are only guaranteed to round-trip through Sparkle. The `UsdPreviewSurface` `normal` input is connected without the conventional `scale`/`bias` of (2, -1), and dielectrics are encoded as `opacity = 0`, so external viewers may misread normal maps and render glass as invisible.

## Export API

```cpp
#include "io/scene/USDExporter.h"

USDExporter::Export(scene, Path::Internal("usd_export/scene.usda"));
```

The output path must be writable (`PathType::Internal` or `PathType::External`). Call it from the main thread after scene loading has finished (`Scene::HasPendingAsyncTasks()` is false), e.g. from a `TestCase` or a UI action.

## Round-trip test

The `usd_round_trip` test case builds a procedural test scene, renders it, exports it, loads the exported file back and renders it again. The driver script first gates the original render against the screenshot ground truth, then FLIP-compares the reimported render against the original:

```bash
python3 tests/usd/usd_roundtrip_test.py --framework glfw --headless
```

The procedural scene (`BuildTestScene` in [tests/usd/UsdRoundTripTest.cpp](../tests/usd/UsdRoundTripTest.cpp)) covers every exportable feature: analytic spheres, mesh primitives, glTF-imported models with full PBR texture sets, dielectric glass, a directional light and a sky light. With an explicit `--scene`, the test round-trips that scene instead (no ground-truth gate). CI runs it on the windows/glfw/Release test job (see [.github/workflows/ci.yml](../.github/workflows/ci.yml)). See [Test.md](Test.md) for general test-case mechanics.

## Packaged TestScene

The app's default scene (empty `--scene`) is `resources/packed/TestScene.usda`: the export of the round-trip test's procedural scene, with texture references re-pointed at the packaged originals (the exporter writes verbatim texture copies, so this is lossless). It must stay at the resource root: asset paths resolve relative to the USD file and tinyusdz rejects `..` in them, so the shared `models/...` and `skymap/...` files are only reachable from there.

To regenerate it after changing `BuildTestScene`, the exporter or the loader:

1. Run the round-trip test (above). It validates the procedural scene against the ground truth and writes the export to `usd_export/scene.usda` in internal storage (`build/generated/usd_export/` under the glfw output directory, `sparkle.app/Contents/SharedSupport/usd_export/` for macos).
2. Copy `usd_export/scene.usda` over `resources/packed/TestScene.usda` and replace each `textures/...` asset path with the corresponding packaged file (the sky map under `skymap/`, the glTF textures under `models/`).
3. Run `dev/run_tests.py --case forward_render_static` without `--scene` to confirm the packaged scene still matches the ground truth.

Since `SpherePrimitive` bakes to a triangle mesh on export, ray-traced pipelines render the packaged scene's spheres as tessellated meshes instead of analytic spheres.
