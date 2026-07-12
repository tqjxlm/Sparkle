# USD Import & Export

Sparkle can load USD scenes (`.usda` / `.usdc` / `.usdz`) and export the active scene back to
`.usda`. Both directions go through [tinyusdz](https://github.com/lighttransport/tinyusdz) and are
designed to round-trip each other: `USDExporter` writes exactly the prims `USDLoader` understands.

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
* Prim names are sanitized to valid USD identifiers; the original node name is preserved in the
  `displayName` metadata and restored on import.
* `SpherePrimitive` exports its tessellated unit-sphere mesh, so it reimports as a `MeshPrimitive`.
  Ray-traced silhouettes of reimported spheres are therefore triangle meshes, not analytic spheres.
* All textures are written next to the exported file (`textures/`), so an export is self-contained
  and can be copied around as a directory.
* On import, an orbit camera is reconstructed from the camera `Xform` and `focusDistance`: the
  orbit center is assumed to be the focus point.
* Asset references inside a USD file are resolved in the same storage domain as the file itself
  (packaged resource vs. generated file), see `Path` in
  [libraries/include/core/Path.h](../libraries/include/core/Path.h).

## Known limitations

* Only triangle meshes with a material are exported. Meshes without a material are skipped
  (`USDLoader` also skips them on import).
* Only one camera per scene is supported (the main camera).
* Animated attributes, skeletons and point/area lights are not supported in either direction.
* Export output is `.usda` only. tinyusdz's binary (`.usdc`) writer is still experimental.
* On import, any `UsdPreviewSurface` with a constant `opacity < 1` becomes a `DieletricMaterial`
  (glass) with `eta = ior`. Alpha-blended materials are not supported.
* `UsdPreviewSurface` has a single input per channel, so a texture is exported without its constant
  factor (`base_color` / `emissive_color`). Factors other than 1 are lost in a round trip.
* Exported materials are only guaranteed to round-trip through Sparkle. The `UsdPreviewSurface`
  `normal` input is connected without the conventional `scale`/`bias` of (2, -1), and dielectrics
  are encoded as `opacity = 0`, so external viewers may misread normal maps and render glass as
  invisible.

## Export API

```cpp
#include "io/scene/USDExporter.h"

USDExporter::Export(scene, Path::Internal("usd_export/scene.usda"));
```

The output path must be writable (`PathType::Internal` or `PathType::External`). Call it from the
main thread after scene loading has finished (`Scene::HasPendingAsyncTasks()` is false), e.g. from
a `TestCase` or a UI action.

## Round-trip test

The `usd_round_trip` test case renders the current scene, exports it, loads the exported file back
and renders it again. The driver script first gates the original render against the same ground
truth as `dev/functional_test.py`, then FLIP-compares the reimported render against the original:

```bash
python3 tests/usd/usd_roundtrip_test.py --framework glfw --headless
```

It exercises the default TestScene, which covers every exportable feature: analytic spheres, mesh
primitives, glTF-imported models with full PBR texture sets, dielectric glass, a directional light
and a sky light. CI runs it on the windows/glfw/Release job (see
[.github/actions/functional-test](../.github/actions/functional-test/action.yml)). See
[Test.md](Test.md) for general test-case mechanics.
