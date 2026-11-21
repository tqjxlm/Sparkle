---
name: debug-rendering
description: Validate a rendering feature or debug an image artifact in Sparkle. Use before adding or changing a rendering feature, when a render looks wrong, or when a visual test result disagrees with what the image shows.
---

# Debug Rendering

Almost every hard rendering bug is a proxy trusted as ground truth that diverged from reality exactly where it mattered. Check each proxy against the real thing at the resolution of the failure.

| proxy                                 | stands in for                     | how it diverges                                                                     |
| ------------------------------------- | --------------------------------- | ----------------------------------------------------------------------------------- |
| whole-image FLIP / PSNR               | "the image is correct"            | low-passes the error map: blind to uniform over-blur and thin or edge artifacts     |
| an analytic limit ("vanishes as N→∞") | behavior at the real sample count | true in the limit, false at practical N                                             |
| a `--debug_mode` or graph view        | the raw buffer value              | tonemapped and display-scaled: a raw 0.5 reads about 0.8, large values saturate     |
| an omitted or wrong `--scene`         | the scene you meant to render     | renders TestScene, or an empty sky for a value that is not a file path, and exits 0 |
| a region-mean or blurred diff         | the per-pixel error field         | averages thin artifacts (halos, fringes) to about zero                              |
| % of pixels affected, mean deviation  | how bad the error looks           | area- and magnitude-weighted: blind to localized high-frequency error               |
| an imported algorithm's assumption    | its behavior in this renderer     | the invariant held in its source regime, not here                                   |

## Capture

* `python3 run.py --framework <framework> --headless true --test_case screenshot [cvars]` writes `screenshots/screenshot.png` under the external storage path ([docs/Run.md](../../../docs/Run.md#storage)). `multi_frame_screenshot` writes five consecutive frames as `multi_frame_<N>.png`, for temporal artifacts.
* Before analysing anything, confirm in the image itself that the expected geometry, pipeline, debug view and cvars took effect.
* Inspect intermediate passes, not only the final frame: `--render_graph_view <texture>` shows any render graph texture ([docs/RenderGraph.md](../../../docs/RenderGraph.md#viewing-a-texture)), `--debug_mode` selects renderer debug outputs. To read a value that is no texture, write it full-screen from the shader. Undo tone mapping and scale the value out of saturation before reading numbers.

## Two Gates Before Any Fix

Never guess a fix, apply it, and ask the user whether it looks better.

1. **Localize and confirm.** Produce a direct visualization of the wrong pixels (per-pixel overlay, signed diff or highlight) in the simplest setting that shows the defect, e.g. the default static view at low spp. Show the user the few resulting images with a one-line caption each, not the investigation, and get confirmation that this is the artifact they mean.
2. **Agree the gate.** The gate is that confirmed visualization: the fix succeeds only when the highlighted pixels are gone in the same image regenerated after the fix. Agree it before implementing. A number may support the verdict but never replaces it; when the number and the image disagree, the image wins. If asked for a purely quantitative criterion, push back and propose the visualization instead.

A supporting number must match the artifact's nature:

* Per-frame or stochastic artifacts (noise a filter spreads) are measured per frame; averaging frames cancels them.
* Localized artifacts are measured in a band derived from geometry (depth or normal discontinuities), not luma edges, which leak surface texture.
* Always against an independent ground truth.

## Diffing

* Diff against ground truth, not only A against B: only ground truth tells which side is wrong and whether an artifact belongs to the feature (present with the feature on, absent from the raw render).
* Use per-pixel signed diffs (red brighter, blue darker), 1:1 crops of suspect regions, and scanline profiles across edges.
* Thin 1–3 px artifacts average to zero under any whole-image, blurred or region-averaged diff, so never use one as the detector.

## Checklist for a Rendering Feature

1. Derive acceptance from failure modes (over-blur, detail loss, ghosting, halos, fireflies, bias, banding) and attach a check sensitive to each: signed diff against ground truth, high-frequency preservation against the reference, worst tile, edge band. Whole-image FLIP or PSNR are necessary, never sufficient.
2. Validate the instrument before trusting it: the intended scene and cvars took effect, debug views show raw values, and the metric moves on a known-bad input.
3. Test "converges / vanishes / self-disables" claims at real magnitudes and the target sample budget; state where they hold and where they break.
4. Look at the image and the signed diff like a human before computing statistics. Statistics quantify what the eye found; they are not the detector.
5. When porting an algorithm, list its invariants and recheck each in this renderer.
6. A passing metric that contradicts the eye is a metric bug: add a check sensitive to the missed failure.
7. Validate on a representative scene (diffuse, specular, silhouettes, textured detail). TestScene qualifies.
8. Rank errors by perceptual salience, not area or mean magnitude. A sharp halo on under 1% of pixels is usually worse than a smooth darkening over half the frame; an error is never minor because it covers few pixels.
