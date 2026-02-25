# REBLUR Denoiser Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement NVIDIA NRD's REBLUR algorithm as an optional denoiser in GPURenderer with split diffuse/specular channels, achieving FLIP <= 0.08 against ground truth.

**Architecture:** Self-contained `ReblurDenoiser` class owned by `GPURenderer`. Path tracer outputs split demodulated diffuse/specular + G-buffer. 7-stage compute pipeline (ClassifyTiles, PrePass, TemporalAccumulation, HistoryFix, Blur, PostBlur, TemporalStabilization) followed by remodulation composite. When disabled, zero regression against existing path.

**Tech Stack:** C++20, Slang shaders, Vulkan/Metal RHI, compute dispatches (16x16 thread groups)

**Design doc:** `docs/plans/2026-02-25-reblur-design.md`

---

## Rules

1. **Tests before commits.** Before every commit, build and run the REBLUR test suite. Do not commit if any test fails — fix the failure first.

   ```bash
   python3 tests/reblur/reblur_test_suite.py --framework glfw
   ```

   The test suite (`tests/reblur/reblur_test_suite.py`) builds once and runs all applicable tests in sequence. It reports a summary table with pass/fail status and timing for each test.

   **Current tests in the suite:**
   1. Smoke test — app launches without crash
   2. Vanilla functional test — GPU pipeline without REBLUR matches CDN ground truth (FLIP <= 0.1)
   3. Split-merge equivalence — split shader with `debug_pass 255` matches CDN ground truth (FLIP ~0.00)
   4. REBLUR screenshot — full denoiser pipeline runs without crash and produces a screenshot
   5. Per-pass validation — spatial passes produce valid output (no NaN/Inf, decreasing variance)
   6. C++ pass validation — native test case exercises full spatial pipeline without crash
   7. Temporal validation — TemporalAccum/HistoryFix produce valid output; multi-frame convergence verified
   8. C++ temporal convergence — 30+ frames temporal pipeline without crash, history buffer cycling

   **Maintaining the test suite:** Whenever a new test case is added to this plan (e.g. a new milestone introduces a new validation), the corresponding test **must** also be added to `tests/reblur/reblur_test_suite.py`. The suite is the single source of truth for what gets run before commits — individual test commands listed in task descriptions are for documentation only.

2. **Verify logs confirm the intended code path.** After each test run, check the output for these log lines:
   - `--pipeline gpu` tests must show: `GPURenderer initializing`
   - `--use_reblur true` tests must show: `REBLUR denoiser enabled` and `ReblurDenoiser: ready`
   - `--use_reblur false` tests must show: `REBLUR denoiser disabled`

   If these lines are missing, the test is not exercising the intended path and the result is invalid.

---

## Milestone 1: Infrastructure

### Task 1: Add `use_reblur` config value

**Files:**
- Modify: `libraries/include/renderer/RenderConfig.h:79` (add field after `spatial_denoise`)
- Modify: `libraries/source/renderer/RenderConfig.cpp:31-55` (register config + validate)

**Step 1: Add config field to RenderConfig.h**

In `RenderConfig.h`, after line 79 (`bool spatial_denoise;`), add:

```cpp
bool use_reblur;
```

**Step 2: Register config in RenderConfig.cpp**

After the `config_spatial_denoise` declaration (~line 31), add:

```cpp
static ConfigValue<bool> config_use_reblur("use_reblur", "use REBLUR denoiser for GPU ray tracing", "renderer", false,
                                           true);
```

In `RenderConfig::Init()`, after `spatial_denoise` registration (~line 55), add:

```cpp
ConfigCollectionHelper::RegisterConfig(this, config_use_reblur, use_reblur);
```

**Step 3: Add validation**

In `RenderConfig::Validate()`, add:

```cpp
if (use_reblur && !IsRayTracingMode())
{
    use_reblur = false;
}
```

**Step 4: Build and verify**

Run: `python build.py --framework glfw`
Expected: Clean build. App launches with `--use_reblur true --pipeline gpu` without crash.

**Step 5: Commit**

```bash
git add libraries/include/renderer/RenderConfig.h libraries/source/renderer/RenderConfig.cpp
git commit -m "feat(reblur): add use_reblur config value"
```

---

### Task 2: Add RG16Float and RG32Float pixel formats

REBLUR needs 2-channel float textures for motion vectors and internal data. The existing `PixelFormat` enum only has 1-channel and 4-channel formats. Add `RG16Float` and `RG32Float`.

**Files:**
- Modify: `libraries/include/io/ImageTypes.h:8-23` (add enum values + update helpers)
- Modify: `libraries/source/rhi/vulkan/VulkanImage.h` (add Vulkan format mapping)
- Modify: `libraries/source/rhi/metal/MetalImage.h` (add Metal format mapping)
- Modify: `libraries/source/rhi/vulkan/VulkanImage.cpp` (if reverse mapping exists)
- Modify: `libraries/source/io/Image.cpp` and `libraries/include/io/Image.h` (if switch statements exist)

**Step 1: Add to PixelFormat enum in ImageTypes.h**

Before `Count`, add:

```cpp
RG16Float,
RG32Float,
```

**Step 2: Update GetFormatChannelCount**

Add cases returning `2` for both new formats.

**Step 3: Update GetPixelSize**

`RG16Float` returns `sizeof(Half) * 2` (4 bytes), `RG32Float` returns `sizeof(float) * 2` (8 bytes).

**Step 4: Update IsSRGBFormat, IsSwizzeldFormat, IsHDRFormat**

All return `false` for both. `IsHDRFormat` should return `true` for both.

**Step 5: Add Vulkan mapping in VulkanImage.h**

```cpp
case PixelFormat::RG16Float:
    return VK_FORMAT_R16G16_SFLOAT;
case PixelFormat::RG32Float:
    return VK_FORMAT_R32G32_SFLOAT;
```

And the reverse mapping.

**Step 6: Add Metal mapping in MetalImage.h**

```cpp
case PixelFormat::RG16Float:
    return MTLPixelFormatRG16Float;
case PixelFormat::RG32Float:
    return MTLPixelFormatRG32Float;
```

**Step 7: Update any other switch statements**

Search all files for `case PixelFormat::` and add the new cases where needed. Every switch on PixelFormat must handle the new values (the project uses `UnImplemented(format)` in default cases, so missing cases will crash).

**Step 8: Build and verify**

Run: `python build.py --framework glfw`
Expected: Clean build, no warnings.

**Step 9: Commit**

```bash
git add libraries/include/io/ImageTypes.h libraries/source/rhi/vulkan/VulkanImage.h \
      libraries/source/rhi/metal/MetalImage.h
# plus any other files touched
git commit -m "feat(rhi): add RG16Float and RG32Float pixel formats"
```

---

### Task 3: Create ReblurDenoiser skeleton class

Create the class with empty `Denoise()` method. No shader logic yet.

**Files:**
- Create: `libraries/include/renderer/denoiser/ReblurDenoiser.h`
- Create: `libraries/source/renderer/denoiser/ReblurDenoiser.cpp`

**Step 1: Create header**

```cpp
#pragma once

#include "core/math/Types.h"
#include "rhi/RHIImage.h"
#include "rhi/RHIResource.h"

namespace sparkle
{
class RHIContext;
class RHIComputePass;
class RHIPipelineState;
class RHIBuffer;
class RHIShader;

struct ReblurSettings
{
    float max_blur_radius = 30.f;
    float min_blur_radius = 1.f;
    float diffuse_prepass_blur_radius = 30.f;
    float specular_prepass_blur_radius = 50.f;
    uint32_t max_accumulated_frame_num = 30;
    uint32_t max_fast_accumulated_frame_num = 6;
    uint32_t max_stabilized_frame_num = 63;
    uint32_t history_fix_frame_num = 3;
    float history_fix_stride = 14.f;
    float disocclusion_threshold = 0.01f;
    float lobe_angle_fraction = 0.15f;
    float roughness_fraction = 0.15f;
    float plane_dist_sensitivity = 0.02f;
    float min_hit_dist_weight = 0.1f;
    float hit_dist_params[4] = {3.f, 0.1f, 20.f, -25.f};
    float stabilization_strength = 1.f;
    float antilag_sigma_scale = 2.f;
    float antilag_sensitivity = 3.f;
    float fast_history_sigma_scale = 2.f;
    bool enable_anti_firefly = true;
};

struct ReblurInputBuffers
{
    RHIImage *diffuse_radiance_hit_dist = nullptr;
    RHIImage *specular_radiance_hit_dist = nullptr;
    RHIImage *normal_roughness = nullptr;
    RHIImage *view_z = nullptr;
    RHIImage *motion_vectors = nullptr;
    RHIImage *albedo_metallic = nullptr;
};

struct ReblurMatrices
{
    Mat4 view_to_clip;
    Mat4 view_to_world;
    Mat4 world_to_clip_prev;
    Mat4 world_to_view_prev;
    Mat4 world_prev_to_world;
};

class ReblurDenoiser
{
public:
    ReblurDenoiser(RHIContext *rhi, uint32_t width, uint32_t height);
    ~ReblurDenoiser();

    void Denoise(const ReblurInputBuffers &inputs, const ReblurSettings &settings,
                 const ReblurMatrices &matrices, uint32_t frame_index);

    [[nodiscard]] RHIImage *GetDenoisedDiffuse() const;
    [[nodiscard]] RHIImage *GetDenoisedSpecular() const;

    void Reset();

private:
    void CreateTextures();

    RHIContext *rhi_;
    uint32_t width_;
    uint32_t height_;
    uint32_t internal_frame_index_ = 0;
    bool history_valid_ = false;

    // Denoised output (written by PostBlur or TemporalStabilization)
    RHIResourceRef<RHIImage> denoised_diffuse_;
    RHIResourceRef<RHIImage> denoised_specular_;
};
} // namespace sparkle
```

**Step 2: Create implementation**

```cpp
#include "renderer/denoiser/ReblurDenoiser.h"

#include "rhi/RHI.h"

namespace sparkle
{
ReblurDenoiser::ReblurDenoiser(RHIContext *rhi, uint32_t width, uint32_t height)
    : rhi_(rhi), width_(width), height_(height)
{
    CreateTextures();
}

ReblurDenoiser::~ReblurDenoiser() = default;

void ReblurDenoiser::CreateTextures()
{
    auto make_image = [this](PixelFormat format, const std::string &name) {
        return rhi_->CreateImage(
            RHIImage::Attribute{
                .format = format,
                .sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
                            .filtering_method_min = RHISampler::FilteringMethod::Linear,
                            .filtering_method_mag = RHISampler::FilteringMethod::Linear,
                            .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
                .width = width_,
                .height = height_,
                .usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV,
                .memory_properties = RHIMemoryProperty::DeviceLocal,
            },
            name);
    };

    denoised_diffuse_ = make_image(PixelFormat::RGBAFloat16, "ReblurDenoisedDiffuse");
    denoised_specular_ = make_image(PixelFormat::RGBAFloat16, "ReblurDenoisedSpecular");
}

void ReblurDenoiser::Denoise(const ReblurInputBuffers & /*inputs*/, const ReblurSettings & /*settings*/,
                             const ReblurMatrices & /*matrices*/, uint32_t /*frame_index*/)
{
    // TODO: implement pipeline stages
    internal_frame_index_++;
}

RHIImage *ReblurDenoiser::GetDenoisedDiffuse() const
{
    return denoised_diffuse_.get();
}

RHIImage *ReblurDenoiser::GetDenoisedSpecular() const
{
    return denoised_specular_.get();
}

void ReblurDenoiser::Reset()
{
    internal_frame_index_ = 0;
    history_valid_ = false;
}
} // namespace sparkle
```

**Step 3: Add to CMakeLists or build system**

Check how existing source files are discovered (likely glob pattern). The new files should be auto-discovered if placed in the correct directory structure.

**Step 4: Build**

Run: `python build.py --framework glfw`
Expected: Clean build.

**Step 5: Commit**

```bash
git add libraries/include/renderer/denoiser/ReblurDenoiser.h \
      libraries/source/renderer/denoiser/ReblurDenoiser.cpp
git commit -m "feat(reblur): add ReblurDenoiser skeleton class"
```

---

### Task 4: Create split path tracer shader

Create `ray_trace_split.cs.slang` that outputs demodulated diffuse/specular + G-buffer data. This shader replaces `ray_trace.cs.slang` when REBLUR is enabled.

**Files:**
- Create: `shaders/ray_trace/ray_trace_split.cs.slang`
- Create: `shaders/include/reblur_common.h.slang`

**Step 1: Create reblur_common.h.slang**

Shared utilities for hit distance normalization and data packing:

```slang
#ifndef REBLUR_COMMON_H
#define REBLUR_COMMON_H

// Hit distance normalization parameters
struct HitDistanceParams
{
    float A; // constant term (world units)
    float B; // view-Z linear scale
    float C; // roughness-based scale (>= 1)
    float D; // roughness exponential decay
};

float NormalizeHitDistance(float hit_dist, float view_z, float roughness, HitDistanceParams params)
{
    float scale = (params.A + view_z * params.B) * lerp(1.0, params.C, exp2(params.D * roughness * roughness));
    return saturate(hit_dist / scale);
}

// Octahedral normal encoding (maps unit sphere to [0,1]^2)
float2 OctWrap(float2 v)
{
    return (1.0 - abs(v.yx)) * (v.xy >= 0.0 ? 1.0 : -1.0);
}

float2 EncodeNormalOct(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = n.z >= 0.0 ? n.xy : OctWrap(n.xy);
    return n.xy * 0.5 + 0.5;
}

float3 DecodeNormalOct(float2 f)
{
    f = f * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.xy += n.xy >= 0.0 ? -t : t;
    return normalize(n);
}

// Luminance
float GetLuminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

#endif // REBLUR_COMMON_H
```

**Step 2: Create ray_trace_split.cs.slang**

Copy `ray_trace.cs.slang` and modify to output split channels. Key changes:

1. Add output UAVs for each auxiliary buffer (bindings after `imageData`)
2. Add `HitDistanceParams` to UBO
3. Add `use_reblur` flag to UBO
4. Modify `SamplePixel` to return a `PathTracerOutput` struct
5. At the primary hit, separate diffuse/specular lobes
6. Write auxiliary data (normal, roughness, viewZ, albedo) from primary hit
7. Compute and normalize hit distance from primary hit to secondary hit

The shader structure:

```slang
// Additional UAV outputs for REBLUR (set 1, bindings 7-12)
[[vk::binding(7, 1)]] RWTexture2D<float4> diffuseOutput;
[[vk::binding(8, 1)]] RWTexture2D<float4> specularOutput;
[[vk::binding(9, 1)]] RWTexture2D<float4> normalRoughnessOutput;
[[vk::binding(10, 1)]] RWTexture2D<float> viewZOutput;
[[vk::binding(11, 1)]] RWTexture2D<float2> motionVectorOutput;
[[vk::binding(12, 1)]] RWTexture2D<float4> albedoOutput;

// Extended UBO with REBLUR params
struct ReblurUBO
{
    float4 hit_dist_params; // A, B, C, D
    float4x4 view_matrix;
};
```

The main change is in `SamplePixel`: at bounce 0, store the primary hit data (normal, roughness, albedo, metallic, viewZ). Then trace the secondary ray. The secondary hit distance becomes the raw hit distance for normalization. Diffuse and specular are separated based on BRDF lobe selection.

**Step 3: Build**

Run: `python build.py --framework glfw`
Expected: Clean build (shader compiles).

**Step 4: Commit**

```bash
git add shaders/ray_trace/ray_trace_split.cs.slang shaders/include/reblur_common.h.slang
git commit -m "feat(reblur): add split path tracer and REBLUR shader utilities"
```

---

### Task 5: Wire split path tracer into GPURenderer

When `use_reblur` is enabled, GPURenderer allocates auxiliary buffers, creates the split path tracer pipeline, and dispatches it instead of the original.

**Files:**
- Modify: `libraries/include/renderer/renderer/GPURenderer.h`
- Modify: `libraries/source/renderer/renderer/GPURenderer.cpp`

**Step 1: Add members to GPURenderer.h**

After the existing member declarations, add:

```cpp
// REBLUR denoiser (null when disabled)
std::unique_ptr<class ReblurDenoiser> reblur_;

// Auxiliary buffers for split path tracer
RHIResourceRef<RHIImage> diffuse_signal_;
RHIResourceRef<RHIImage> specular_signal_;
RHIResourceRef<RHIImage> normal_roughness_;
RHIResourceRef<RHIImage> view_z_;
RHIResourceRef<RHIImage> motion_vectors_;
RHIResourceRef<RHIImage> albedo_metallic_;

// Split path tracer pipeline
RHIResourceRef<RHIShader> split_pt_shader_;
RHIResourceRef<RHIPipelineState> split_pt_pipeline_;
RHIResourceRef<RHIBuffer> split_pt_uniform_buffer_;
```

**Step 2: Add forward declaration**

```cpp
#include <memory>
// Forward declare
class ReblurDenoiser;
```

**Step 3: In InitRenderResources(), conditionally create REBLUR resources**

After existing initialization, add:

```cpp
if (render_config_.use_reblur)
{
    InitReblurResources();
}
```

Implement `InitReblurResources()` as a private method that:
1. Creates 6 auxiliary images (diffuse, specular, normal_roughness, viewZ, motion_vectors, albedo)
2. Creates split path tracer shader and pipeline
3. Creates `ReblurDenoiser` instance

**Step 4: In Render(), use split path when REBLUR enabled**

```cpp
if (reblur_)
{
    // Transition all aux buffers to StorageWrite
    // Dispatch split path tracer
    // Transition aux buffers to Read
    // Run REBLUR denoiser
    // Composite into scene_texture_
}
else
{
    // Original path (unchanged)
}
```

**Step 5: Build and test**

Run: `python build.py --framework glfw`
Run: `python3 build.py --framework glfw --run --test_case screenshot --headless true --pipeline gpu --use_reblur true`
Expected: Screenshot captured (may be black/incorrect since denoiser is skeleton).

Run: `python3 build.py --framework glfw --run --test_case screenshot --headless true --pipeline gpu --use_reblur false`
Expected: Screenshot matches existing baseline (no regression).

**Step 6: Commit**

```bash
git add libraries/include/renderer/renderer/GPURenderer.h \
      libraries/source/renderer/renderer/GPURenderer.cpp
git commit -m "feat(reblur): wire split path tracer and ReblurDenoiser into GPURenderer"
```

---

### Task 6: Create composite shader and pass

The composite pass remodulates the denoised signal: `color = denoisedDiffuse * albedo + denoisedSpecular`.

**Files:**
- Create: `shaders/ray_trace/reblur_composite.cs.slang`
- Modify: `libraries/source/renderer/renderer/GPURenderer.cpp` (add composite dispatch)

**Step 1: Create composite shader**

```slang
#include "reblur_common.h.slang"

[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
};

[[vk::binding(1, 0)]] Texture2D<float4> denoisedDiffuse;
[[vk::binding(2, 0)]] Texture2D<float4> denoisedSpecular;
[[vk::binding(3, 0)]] Texture2D<float4> albedoMetallic;
[[vk::binding(4, 0)]] SamplerState linearSampler;
[[vk::binding(5, 0)]] RWTexture2D<float4> outputImage;

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
        return;

    float4 diff = denoisedDiffuse[pixel];
    float4 spec = denoisedSpecular[pixel];
    float4 albedo = albedoMetallic[pixel];

    float3 color = diff.rgb * albedo.rgb + spec.rgb;

    outputImage[pixel] = float4(color, 1.0);
}
```

**Step 2: Add shader declaration in GPURenderer.cpp**

Follow the `RayTracingComputeShader` pattern:

```cpp
class ReblurCompositeShader : public RHIShaderInfo
{
    REGISTGER_SHADER(ReblurCompositeShader, RHIShaderStage::Compute,
                     "shaders/ray_trace/reblur_composite.cs.slang", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)
    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(denoisedDiffuse, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(denoisedSpecular, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(albedoMetallic, RHIShaderResourceReflection::ResourceType::Texture2D)
    USE_SHADER_RESOURCE(linearSampler, RHIShaderResourceReflection::ResourceType::Sampler)
    USE_SHADER_RESOURCE(outputImage, RHIShaderResourceReflection::ResourceType::StorageImage2D)
    END_SHADER_RESOURCE_TABLE

    struct UniformBufferData
    {
        Vector2UInt resolution;
    };
};
```

**Step 3: Wire into Render()**

After denoiser runs, dispatch composite to write into `scene_texture_`.

**Step 4: Build and test**

Run with `--use_reblur true`. The output should show something (the composite produces diff*albedo + spec from the denoised signals).

**Step 5: Commit**

```bash
git add shaders/ray_trace/reblur_composite.cs.slang \
      libraries/source/renderer/renderer/GPURenderer.cpp
git commit -m "feat(reblur): add composite pass for demodulated signal remodulation"
```

---

### Task 7: Verify split-merge equivalence with vanilla pipeline

Validate that the split path tracer produces an image identical to the vanilla GPU pipeline when the denoiser and composite are bypassed. This catches bugs in radiance accumulation, sky handling, emissive handling, and NEE in the split shader.

**Approach:** The split shader accumulates a `total_radiance` field that mirrors vanilla's `pixel_color` exactly (same contributions, same order). In `main()`, this is clamped and written to `imageData` using the same moving-average logic as vanilla. Debug pass 255 in `GPURenderer::RenderReblurPath()` returns early after the split shader dispatch, skipping the denoiser and composite entirely.

**Step 1: Add `total_radiance` to `SplitPathOutput`**

Add a `float3 total_radiance` field, initialize to zero, and accumulate at every radiance event (sky, emissive, NEE) before demodulation — matching vanilla's `pixel_color` accumulation.

**Step 2: Fix `imageData` write in `main()`**

Replace the broken remodulation-based `imageData` write with:
```slang
float3 combined_clamped = min(output.total_radiance, output_limit);
this_combined += combined_clamped;
// ... after SPP loop:
float moving_average = (float)total_sample_count / (total_sample_count + spp);
float3 all_samples = lerp(this_combined / float(spp), imageData[pixel].rgb, moving_average);
imageData[pixel] = float4(all_samples, 1.f);
```

**Step 3: Add debug pass 255 early return in GPURenderer**

After the split shader dispatch, check `render_config_.reblur_debug_pass == 255` and return early, transitioning `scene_texture_` to Read for tone mapping.

**Step 4: Run equivalence test**

```bash
python3 dev/functional_test.py --framework glfw --pipeline gpu --headless --skip_build -- --use_reblur true --reblur_debug_pass 255
```

Expected FLIP ~0.00 against CDN ground truth. The split shader's `imageData` output should be bit-identical to vanilla's (same RNG seed, same accumulation logic).

**Step 5: Commit**

```bash
git commit -m "fix(reblur): lossless split-merge debug pass for M1 equivalence test"
```

---

## Milestone 2: Spatial-Only Denoising

### Task 8: Implement shared REBLUR shader infrastructure

Create the shared shader headers that all REBLUR passes will use.

**Files:**
- Create: `shaders/include/reblur_config.h.slang`
- Create: `shaders/include/reblur_data.h.slang`
- Create: `shaders/include/poisson_samples.h.slang`

**Step 1: Create poisson_samples.h.slang**

Pre-computed 8-sample Poisson disk (from NRD's `g_Special8`):

```slang
#ifndef POISSON_SAMPLES_H
#define POISSON_SAMPLES_H

static const float2 g_Special8[8] = {
    float2(-0.4706069, -0.4427112),
    float2(-0.9057375,  0.3003471),
    float2(-0.3487388,  0.4037880),
    float2( 0.1023042,  0.6439373),
    float2( 0.6157020,  0.1873830),
    float2( 0.3550950, -0.6522653),
    float2(-0.0697460, -0.9581780),
    float2( 0.7679530, -0.4071750),
};

#endif
```

**Step 2: Create reblur_config.h.slang**

Constants and tuning parameters:

```slang
#ifndef REBLUR_CONFIG_H
#define REBLUR_CONFIG_H

#define REBLUR_PRE_BLUR_POISSON_SAMPLE_NUM    8
#define REBLUR_POISSON_SAMPLE_NUM             8
#define REBLUR_PRE_BLUR_FRACTION_SCALE        2.0
#define REBLUR_BLUR_FRACTION_SCALE            1.0
#define REBLUR_POST_BLUR_FRACTION_SCALE       0.5
#define REBLUR_POST_BLUR_RADIUS_SCALE         2.0
#define REBLUR_HISTORY_FIX_FILTER_RADIUS      2
#define REBLUR_ANTI_FIREFLY_FILTER_RADIUS     4
#define REBLUR_FAST_HISTORY_CLAMPING_RADIUS   2
#define REBLUR_DENOISING_RANGE                1000.0
#define REBLUR_TILE_SIZE                      16

#endif
```

**Step 3: Create reblur_data.h.slang**

Packing/unpacking helpers for internal data:

```slang
#ifndef REBLUR_DATA_H
#define REBLUR_DATA_H

#include "reblur_common.h.slang"

// Internal data packing: accumSpeed (6 bits) + material ID (4 bits) + accumSpeedFast (6 bits) = 16 bits
// Stored in .x of RG16Float
#define REBLUR_ACCUMSPEED_BITS    6
#define REBLUR_MAX_ACCUM_FRAME    ((1 << REBLUR_ACCUMSPEED_BITS) - 1)

float2 PackInternalData(float accum_speed, float accum_speed_fast)
{
    return float2(accum_speed / REBLUR_MAX_ACCUM_FRAME, accum_speed_fast / REBLUR_MAX_ACCUM_FRAME);
}

void UnpackInternalData(float2 packed, out float accum_speed, out float accum_speed_fast)
{
    accum_speed = packed.x * REBLUR_MAX_ACCUM_FRAME;
    accum_speed_fast = packed.y * REBLUR_MAX_ACCUM_FRAME;
}

// Non-linear accumulation speed: f = 1 / (1 + N)
float GetNonLinearAccumSpeed(float accum_speed)
{
    return 1.0 / (1.0 + accum_speed);
}

#endif
```

**Step 4: Build**

Run: `python build.py --framework glfw`

**Step 5: Commit**

```bash
git add shaders/include/reblur_config.h.slang shaders/include/reblur_data.h.slang \
      shaders/include/poisson_samples.h.slang
git commit -m "feat(reblur): add shared shader infrastructure (config, data packing, Poisson samples)"
```

---

### Task 9: Implement ClassifyTiles pass

**Files:**
- Create: `shaders/ray_trace/reblur_classify_tiles.cs.slang`
- Modify: `libraries/include/renderer/denoiser/ReblurDenoiser.h` (add pass members)
- Modify: `libraries/source/renderer/denoiser/ReblurDenoiser.cpp` (implement pass)

**Step 1: Create shader**

```slang
#include "reblur_config.h.slang"

[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    float denoising_range;
};

[[vk::binding(1, 0)]] Texture2D<float> inViewZ;
[[vk::binding(2, 0)]] SamplerState pointSampler;
[[vk::binding(3, 0)]] RWTexture2D<float> outTiles;

groupshared uint s_isSky;

[shader("compute")]
[numthreads(REBLUR_TILE_SIZE, REBLUR_TILE_SIZE, 1)]
void main(uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
    if (GTid.x == 0 && GTid.y == 0)
        s_isSky = 1;

    GroupMemoryBarrierWithGroupSync();

    uint2 pixel = Gid.xy * REBLUR_TILE_SIZE + GTid.xy;

    if (pixel.x < resolution.x && pixel.y < resolution.y)
    {
        float viewZ = inViewZ[pixel];
        if (viewZ <= denoising_range)
            InterlockedAnd(s_isSky, 0);
    }

    GroupMemoryBarrierWithGroupSync();

    if (GTid.x == 0 && GTid.y == 0)
        outTiles[Gid.xy] = float(s_isSky);
}
```

**Step 2: Add to ReblurDenoiser**

Add shader declaration, pipeline state, compute pass, tiles texture, and uniform buffer. Implement `ClassifyTiles()` private method.

**Step 3: Add tiles texture creation**

In `CreateTextures()`:

```cpp
uint32_t tile_w = (width_ + 15) / 16;
uint32_t tile_h = (height_ + 15) / 16;
tiles_ = make_image(PixelFormat::R32_FLOAT, tile_w, tile_h, "ReblurTiles");
```

(Using R32_FLOAT since R8 is not available as UAV format.)

**Step 4: Call from Denoise()**

```cpp
void ReblurDenoiser::Denoise(...)
{
    ClassifyTiles(inputs, settings);
    // ... rest of pipeline (spatial passes follow)
}
```

**Step 5: Build and test**

Run with `--use_reblur true`. Verify no crash. Optionally debug-visualize the tile map.

**Step 6: Commit**

```bash
git add shaders/ray_trace/reblur_classify_tiles.cs.slang \
      libraries/include/renderer/denoiser/ReblurDenoiser.h \
      libraries/source/renderer/denoiser/ReblurDenoiser.cpp
git commit -m "feat(reblur): implement ClassifyTiles pass"
```

---

### Task 10: Implement Blur pass (primary spatial filter)

The blur pass is the core spatial denoiser. Implement it before PrePass (which is optional) to get visual feedback faster.

**Files:**
- Create: `shaders/ray_trace/reblur_blur.cs.slang`
- Modify: `libraries/source/renderer/denoiser/ReblurDenoiser.cpp`

**Step 1: Create blur shader**

The shader performs 8-sample Poisson disk bilateral filtering:

Key logic per sample:
1. Compute sample position using Poisson disk + per-frame rotator
2. Read neighbor normal, viewZ, hit distance
3. Compute bilateral weights: geometry * normal * hitDistance * Gaussian
4. Accumulate weighted color

The blur radius is: `maxBlurRadius * sqrt(hitDistFactor * nonLinearAccumSpeed)`, clamped to `[minBlurRadius, maxBlurRadius]`.

For this first version without temporal accumulation, `nonLinearAccumSpeed = 1.0` (no history), so the radius is `maxBlurRadius * sqrt(hitDistFactor)`.

**Step 2: Add uniform buffer struct**

```slang
struct ReblurBlurUBO
{
    uint2 resolution;
    float max_blur_radius;
    float min_blur_radius;
    float lobe_angle_fraction;
    float plane_dist_sensitivity;
    float min_hit_dist_weight;
    float denoising_range;
    float4 rotator; // per-frame rotation for Poisson disk
    uint frame_index;
    uint blur_pass_index; // 0=pre, 1=blur, 2=post
};
```

**Step 3: Add to ReblurDenoiser pipeline**

Create shader declaration, pipeline, compute pass. Wire into `Denoise()` after ClassifyTiles.

**Step 4: Create temp textures**

Add `diff_temp1_`, `diff_temp2_`, `spec_temp1_`, `spec_temp2_` (all RGBAFloat16) for ping-pong.

**Step 5: Build and test**

Run with `--use_reblur true --spp 1 --max_spp 2`. The output should show blurred/denoised image (may be overblurred since no temporal history to shrink radius yet).

**Step 6: Commit**

```bash
git add shaders/ray_trace/reblur_blur.cs.slang
git commit -m "feat(reblur): implement primary Blur pass with Poisson disk bilateral filter"
```

---

### Task 11: Implement PrePass (spatial pre-filter)

**Files:**
- Create: `shaders/ray_trace/reblur_prepass.cs.slang`
- Modify: `libraries/source/renderer/denoiser/ReblurDenoiser.cpp`

**Step 1: Create prepass shader**

Similar structure to Blur but with:
- Larger blur radius (`diffusePrepassBlurRadius`, `specularPrepassBlurRadius`)
- `FRACTION_SCALE = 2.0` (more aggressive normal rejection)
- Also outputs `specHitDistForTracking` (for later use in temporal accumulation)

**Step 2: Add specHitDistForTracking texture**

`RHIResourceRef<RHIImage> spec_hit_dist_for_tracking_` (RGBAFloat16 or R32_FLOAT).

**Step 3: Wire into pipeline**

`ClassifyTiles → PrePass → Blur → Composite`

**Step 4: Build and test**

Expected: PrePass reduces variance before blur, resulting in slightly cleaner output.

**Step 5: Commit**

```bash
git add shaders/ray_trace/reblur_prepass.cs.slang
git commit -m "feat(reblur): implement PrePass spatial pre-filter"
```

---

### Task 12: Implement PostBlur pass

**Files:**
- Create: `shaders/ray_trace/reblur_post_blur.cs.slang`
- Modify: `libraries/source/renderer/denoiser/ReblurDenoiser.cpp`

**Step 1: Create post-blur shader**

Same structure as Blur but with `RADIUS_SCALE = 2.0` and `FRACTION_SCALE = 0.5`.

In this spatial-only milestone, PostBlur also writes:
- `prev_viewZ` copy (for next frame's temporal accumulation)
- `prev_normal_roughness` copy
- Final denoised output to `denoised_diffuse_`/`denoised_specular_`

**Step 2: Add previous-frame textures**

```cpp
RHIResourceRef<RHIImage> prev_view_z_;
RHIResourceRef<RHIImage> prev_normal_roughness_;
```

**Step 3: Wire into pipeline**

`ClassifyTiles → PrePass → Blur → PostBlur → Composite`

**Step 4: Take screenshot and verify edge preservation**

Run: `python3 build.py --framework glfw --run --test_case screenshot --headless true --pipeline gpu --use_reblur true --spp 1 --max_spp 4`
Expected: Denoised image with preserved edges (no blur across normal boundaries).

**Step 5: Commit**

```bash
git add shaders/ray_trace/reblur_post_blur.cs.slang
git commit -m "feat(reblur): implement PostBlur pass, complete spatial-only pipeline"
```

---

## Milestone 3: Temporal Accumulation

### Task 13: Implement Temporal Accumulation pass

The most complex pass. For the static camera milestone, this simplifies significantly because motion vectors are zero and reprojection is identity.

**Files:**
- Create: `shaders/ray_trace/reblur_temporal_accumulation.cs.slang`
- Modify: `libraries/include/renderer/denoiser/ReblurDenoiser.h` (add history textures)
- Modify: `libraries/source/renderer/denoiser/ReblurDenoiser.cpp`

**Step 1: Add history textures**

```cpp
// History buffers (persistent across frames)
RHIResourceRef<RHIImage> diff_history_;
RHIResourceRef<RHIImage> spec_history_;
RHIResourceRef<RHIImage> diff_fast_history_;
RHIResourceRef<RHIImage> spec_fast_history_;
RHIResourceRef<RHIImage> internal_data_;       // accumSpeed, materialID
RHIResourceRef<RHIImage> prev_internal_data_;
```

**Step 2: Create temporal accumulation shader**

For static camera (motion vectors = 0):
1. Previous pixel = current pixel (identity reprojection)
2. Read previous viewZ and normal from `prev_view_z_` and `prev_normal_roughness_`
3. Disocclusion test: compare plane distance between current and previous
4. If not disoccluded: `accumSpeed = min(prevAccumSpeed + 1, maxAccumulatedFrameNum)`
5. If disoccluded: `accumSpeed = 0` (reset)
6. Blend: `output = lerp(current, history, accumSpeed / (accumSpeed + 1))`
7. Write `internal_data_` with packed `accumSpeed`

**Step 3: Add uniform buffer with matrices**

Even though motion vectors are zero for now, pass the matrices through UBO for future use:

```slang
struct TemporalAccumUBO
{
    uint2 resolution;
    float4x4 world_to_clip_prev;
    float4x4 world_to_view_prev;
    float max_accumulated_frame_num;
    float max_fast_accumulated_frame_num;
    float disocclusion_threshold;
    float denoising_range;
    uint frame_index;
    uint reset_history;
};
```

**Step 4: Wire into pipeline**

`ClassifyTiles → PrePass → TemporalAccum → Blur → PostBlur → Composite`

Note the order change: blur passes now come AFTER temporal accumulation.

**Step 5: Build and test convergence**

Run with `--use_reblur true --spp 1 --max_spp 64` and take screenshots at different frame counts.
Expected: Image should converge over time (cleaner with each frame).

**Step 6: Commit**

```bash
git add shaders/ray_trace/reblur_temporal_accumulation.cs.slang
git commit -m "feat(reblur): implement TemporalAccumulation pass with linear history blending"
```

---

### Task 14: Implement History Fix pass

**Files:**
- Create: `shaders/ray_trace/reblur_history_fix.cs.slang`
- Modify: `libraries/source/renderer/denoiser/ReblurDenoiser.cpp`

**Step 1: Create history fix shader**

5x5 bilateral kernel with large stride (14px). Only active for recently disoccluded pixels.

```slang
// Only apply if accumSpeed < historyFixFrameNum
if (accum_speed >= history_fix_frame_num)
{
    // Pass through
    outDiff[pixel] = inDiff[pixel];
    outSpec[pixel] = inSpec[pixel];
    return;
}

// Wide-stride 5x5 bilateral reconstruction
float stride = history_fix_stride * (1.0 - accum_speed / history_fix_frame_num);
```

**Step 2: Wire into pipeline**

`ClassifyTiles → PrePass → TemporalAccum → HistoryFix → Blur → PostBlur → Composite`

**Step 3: Test**

On frame 0 (first frame, no history), the history fix should fill the image with reasonable data from the wide neighborhood. On subsequent frames, it should only affect newly revealed pixels.

**Step 4: Commit**

```bash
git add shaders/ray_trace/reblur_history_fix.cs.slang
git commit -m "feat(reblur): implement HistoryFix pass for disoccluded region reconstruction"
```

---

### Task 15: Run convergence tests

**Step 1: Build**

```bash
python3 build.py --framework glfw
```

**Step 2: Take screenshots at different convergence states**

Run with increasing `--max_spp` values (4, 16, 64) and compare against 2048spp ground truth.

**Step 3: Verify temporal convergence**

After 30+ frames with static camera, the denoised output should approach the ground truth. Target: FLIP <= 0.12 at this stage (before tuning).

**Step 4: Commit test results/observations**

```bash
git commit -m "test(reblur): verify temporal convergence with static camera"
```

---

## Milestone 4: Temporal Stabilization

### Task 16: Implement Temporal Stabilization pass

**Files:**
- Create: `shaders/ray_trace/reblur_temporal_stabilization.cs.slang`
- Modify: `libraries/include/renderer/denoiser/ReblurDenoiser.h` (add stabilized history)
- Modify: `libraries/source/renderer/denoiser/ReblurDenoiser.cpp`

**Step 1: Add stabilized history textures (ping-pong)**

```cpp
RHIResourceRef<RHIImage> diff_stabilized_[2];  // ping-pong
RHIResourceRef<RHIImage> spec_stabilized_[2];  // ping-pong
```

**Step 2: Create shader**

Uses shared memory to load neighborhood luminance:

```slang
groupshared float s_DiffLuma[BUFFER_Y][BUFFER_X];
groupshared float s_SpecLuma[BUFFER_Y][BUFFER_X];
```

Core logic:
1. Load neighborhood luminance into shared memory
2. Compute local mean and variance
3. Clamp stabilized history to `[mean - sigma * scale, mean + sigma * scale]`
4. Blend with current data
5. Write final output

**Step 3: Update PostBlur to output to stabilization input**

When temporal stabilization is enabled, PostBlur writes to history buffers instead of final output. The stabilization pass produces the final denoised output.

**Step 4: Wire into pipeline**

`ClassifyTiles → PrePass → TemporalAccum → HistoryFix → Blur → PostBlur → TempStab → Composite`

**Step 5: Build and verify flicker reduction**

Run with `--use_reblur true --spp 1 --max_spp 64` and observe temporal stability.

**Step 6: Commit**

```bash
git add shaders/ray_trace/reblur_temporal_stabilization.cs.slang
git commit -m "feat(reblur): implement TemporalStabilization pass with variance clamping"
```

---

## Milestone 5: Quality Tuning & Tests

### Task 17: Parameter tuning

**Step 1: Run with default NRD parameters**

Take screenshot and measure FLIP against ground truth.

**Step 2: Adjust parameters if FLIP > 0.08**

Key tuning knobs:
- `maxBlurRadius`: Increase if too noisy, decrease if over-blurred
- `maxAccumulatedFrameNum`: Increase for more temporal stability
- `lobeAngleFraction`: Decrease for sharper normal edges
- `hitDistParams`: Adjust A/B/C/D based on scene scale
- `stabilizationStrength`: Increase if flickering persists

**Step 3: Commit tuned defaults**

```bash
git commit -m "tune(reblur): adjust default parameters for FLIP <= 0.08 target"
```

---

### Task 18: Create REBLUR smoke test

**Files:**
- Create: `tests/reblur/ReblurSmokeTest.cpp`

**Step 1: Create test case**

```cpp
#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"

namespace sparkle
{
class ReblurSmokeTest : public TestCase
{
public:
    Result Tick(AppFramework &app) override
    {
        frame_count_++;

        // Wait for convergence (30 frames with REBLUR)
        if (frame_count_ < 30)
            return Result::Pending;

        if (app.IsScreenshotCompleted())
            return Result::Pass;

        if (!requested_)
        {
            app.RequestTakeScreenshot();
            requested_ = true;
        }

        // Safety timeout (honours test_timeout cvar from CI)
        uint32_t timeout = app.GetAppConfig().test_timeout;
        if (timeout > 0 && frame_count_ > timeout)
        {
            Log(Error, "ReblurSmokeTest timed out after {} frames", timeout);
            return Result::Fail;
        }

        return Result::Pending;
    }

private:
    uint32_t frame_count_ = 0;
    bool requested_ = false;
};

static TestCaseRegistrar<ReblurSmokeTest> reblur_smoke_registrar("reblur_smoke");
} // namespace sparkle
```

**Step 2: Add test to the suite**

Add the `reblur_smoke` test as a new entry in `tests/reblur/reblur_test_suite.py` so it runs as part of the standard pre-commit suite.

**Step 3: Verify via suite**

```bash
python3 tests/reblur/reblur_test_suite.py --framework glfw
```

**Step 4: Commit**

```bash
git add tests/reblur/ReblurSmokeTest.cpp tests/reblur/reblur_test_suite.py
git commit -m "test(reblur): add REBLUR smoke test (waits 30 frames then screenshots)"
```

---

### Task 19: Run all end-to-end tests

Run the full test suite and ensure all tests pass:

```bash
python3 tests/reblur/reblur_test_suite.py --framework glfw
```

At this point the suite should include all tests from M1-M5. Verify the summary shows 0 failures.

**Additional quality checks (not yet in suite — add when implemented):**

1. **Quality target:** REBLUR denoised output at 64 SPP should achieve FLIP <= 0.08 against 2048 SPP ground truth.

2. **Temporal stability:** Capture 10 consecutive screenshots after convergence. Per-pixel luminance std-dev should be < 0.02.

**Step 1: Document results**

Update the design doc with actual measured metrics.

**Step 2: Commit**

```bash
git commit -m "test(reblur): all end-to-end tests passing within thresholds"
```

---

## Summary

| Milestone | Tasks | Key Deliverable |
|-----------|-------|-----------------|
| M1: Infrastructure | 1-7 | Split path tracer + composite roundtrip verified |
| M2: Spatial-only | 8-12 | Edge-preserving bilateral blur working |
| M3: Temporal | 13-15 | History accumulation converging under static camera |
| M4: Stabilization | 16 | Temporal stabilization reducing flicker |
| M5: Quality & Tests | 17-19 | FLIP <= 0.08, all tests green |

Total: 19 tasks, ~6 milestones (M6 camera motion deferred to future).
