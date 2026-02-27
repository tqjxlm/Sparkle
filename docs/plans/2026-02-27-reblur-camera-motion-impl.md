# REBLUR Camera Motion Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Enable the REBLUR denoiser to handle a moving camera via surface motion (SMB) reprojection with bilinear/Catmull-Rom history sampling, parallax-based disocclusion, anti-lag, and framerate scaling.

**Architecture:** Previous-frame matrices are stored in CameraRenderProxy and flow through GPURenderer → ReblurDenoiser → shaders. The split path tracer computes screen-space motion vectors from world-space hit positions projected through current and previous clip matrices. Temporal accumulation uses these MVs for reprojection with an adaptive bilinear/Catmull-Rom filter. Anti-lag and framerate scaling are added to temporal stabilization. A CameraAnimator provides deterministic programmatic camera paths for automated testing.

**Tech Stack:** C++17, Slang shaders (HLSL-like), Vulkan RHI abstraction, Python test scripts, FLIP image quality metric.

**Design doc:** [2026-02-27-reblur-camera-motion-design.md](2026-02-27-reblur-camera-motion-design.md)

---

### Task 1: Store Previous-Frame Matrices in CameraRenderProxy

**Files:**
- Modify: `libraries/include/renderer/proxy/CameraRenderProxy.h:152-174`
- Modify: `libraries/source/renderer/proxy/CameraRenderProxy.cpp:10-62`

**Step 1: Add previous-frame matrix members to CameraRenderProxy.h**

Add after line 161 (after `view_projection_matrix_`):

```cpp
// Previous-frame matrices (stored before each frame's update)
TransformMatrix view_matrix_prev_;
Mat4 projection_matrix_prev_;
Mat4 view_projection_matrix_prev_;
Vector3 position_prev_ = Vector3::Zero();
```

Add public getters after line 128 (after `GetViewProjectionMatrix`):

```cpp
[[nodiscard]] TransformMatrix GetViewMatrixPrev() const
{
    return view_matrix_prev_;
}

[[nodiscard]] Mat4 GetProjectionMatrixPrev() const
{
    return projection_matrix_prev_;
}

[[nodiscard]] Mat4 GetViewProjectionMatrixPrev() const
{
    return view_projection_matrix_prev_;
}

[[nodiscard]] Vector3 GetPositionPrev() const
{
    return position_prev_;
}
```

**Step 2: Save previous-frame matrices at the start of Update()**

In `CameraRenderProxy.cpp`, add at the beginning of `Update()` (after line 12):

```cpp
// Save previous-frame matrices before any updates
view_matrix_prev_ = view_matrix_;
projection_matrix_prev_ = projection_matrix_;
view_projection_matrix_prev_ = view_projection_matrix_;
position_prev_ = posture_.position;
```

**Step 3: Build and verify compilation**

Run: `python build.py --framework glfw`
Expected: Build succeeds.

**Step 4: Commit**

```bash
git add libraries/include/renderer/proxy/CameraRenderProxy.h \
      libraries/source/renderer/proxy/CameraRenderProxy.cpp
git commit -m "feat(reblur): store previous-frame matrices in CameraRenderProxy"
```

---

### Task 2: Populate ReblurMatrices in GPURenderer

**Files:**
- Modify: `libraries/source/renderer/renderer/GPURenderer.cpp:757-767`

**Step 1: Populate ReblurMatrices with real camera data**

Replace the default-constructed `ReblurMatrices matrices;` at line 766 with:

```cpp
auto *camera_proxy = scene_render_proxy_->GetMainCamera();
ReblurMatrices matrices;
matrices.view_to_clip = camera_proxy->GetProjectionMatrix();
matrices.view_to_world = camera_proxy->GetViewMatrix().inverse();
matrices.world_to_clip_prev = camera_proxy->GetViewProjectionMatrixPrev();
matrices.world_to_view_prev = camera_proxy->GetViewMatrixPrev();
matrices.world_prev_to_world = Mat4::Identity();  // Static world
```

Note: Check the actual method available on `scene_render_proxy_` to get the camera — it may be accessed differently. The key is to get the CameraRenderProxy's current and previous matrices.

**Step 2: Build and run static camera smoke test**

Run: `python build.py --framework glfw`
Then: `python build.py --framework glfw --run --headless true --pipeline gpu --use_reblur true --spp 1 --max_spp 10 --test_case screenshot`
Expected: Build succeeds. Screenshot captured. No regression (matrices are identity-equivalent for first frame since prev=current when camera doesn't move).

**Step 3: Commit**

```bash
git add libraries/source/renderer/renderer/GPURenderer.cpp
git commit -m "feat(reblur): populate ReblurMatrices from camera proxy"
```

---

### Task 3: Add worldToClip/worldToClipPrev Uniforms to Split Path Tracer

**Files:**
- Modify: `shaders/ray_trace/ray_trace_split.cs.slang:26-38`
- Modify: `libraries/source/renderer/renderer/GPURenderer.cpp:85-97` (SplitPathTracerShader UBO)
- Modify: `libraries/source/renderer/renderer/GPURenderer.cpp:474-484` (UBO upload)

**Step 1: Add matrix uniforms to the split PT shader UBO**

In `ray_trace_split.cs.slang`, add to the `cbuffer ubo` block after `float4x4 view_matrix;` (line 37):

```slang
float4x4 worldToClip;
float4x4 worldToClipPrev;
```

**Step 2: Add matching fields to the C++ UBO struct**

In `GPURenderer.cpp`, in `SplitPathTracerShader::UniformBufferData` (after line 96):

```cpp
Mat4 world_to_clip = Mat4::Identity();
Mat4 world_to_clip_prev = Mat4::Identity();
```

**Step 3: Set the matrices in UBO upload**

In `GPURenderer.cpp` where split_ubo is constructed (line 474-482), add the matrix fields:

```cpp
SplitPathTracerShader::UniformBufferData split_ubo{
    .camera = ubo.camera,
    .sky_light = ubo.sky_light,
    .dir_light = ubo.dir_light,
    .time_seed = ubo.time_seed,
    .total_sample_count = ubo.total_sample_count,
    .spp = ubo.spp,
    .enable_nee = ubo.enable_nee,
    .world_to_clip = camera_proxy->GetViewProjectionMatrix(),
    .world_to_clip_prev = camera_proxy->GetViewProjectionMatrixPrev(),
};
```

Note: `camera_proxy` access depends on what's available in the Update method scope. May need to extract it first — follow the pattern used for ReblurMatrices population.

**Step 4: Build and verify compilation**

Run: `python build.py --framework glfw`
Expected: Build succeeds. Shader compiles.

**Step 5: Commit**

```bash
git add shaders/ray_trace/ray_trace_split.cs.slang \
      libraries/source/renderer/renderer/GPURenderer.cpp
git commit -m "feat(reblur): add worldToClip matrices to split PT shader"
```

---

### Task 4: Compute Motion Vectors in Split Path Tracer

**Files:**
- Modify: `shaders/ray_trace/ray_trace_split.cs.slang:540-557`

**Step 1: Replace zero motion vectors with real computation**

Replace the motion vector output section (around line 551) with:

```slang
// Motion vectors: compute from world position projected through current and previous matrices.
// Convention: MV = prevUV - currentUV (where did this pixel come from in the previous frame?)
if (last_output.isHit)
{
    float3 worldPos = /* primary hit world position — extract from the hit point computation */;

    float4 clipCurr = mul(worldToClip, float4(worldPos, 1.0));
    float2 uvCurr = clipCurr.xy / clipCurr.w * 0.5 + float2(0.5, 0.5);

    float4 clipPrev = mul(worldToClipPrev, float4(worldPos, 1.0));
    float2 uvPrev = clipPrev.xy / clipPrev.w * 0.5 + float2(0.5, 0.5);

    motionVectorOutput[pixel] = uvPrev - uvCurr;
}
else
{
    // Sky pixel: compute from view direction
    // Use the primary ray direction projected through both matrices
    float3 skyDir = /* primary ray direction */;
    float3 farPoint = camera.position + skyDir * 10000.0;

    float4 clipCurr = mul(worldToClip, float4(farPoint, 1.0));
    float2 uvCurr = clipCurr.xy / clipCurr.w * 0.5 + float2(0.5, 0.5);

    float4 clipPrev = mul(worldToClipPrev, float4(farPoint, 1.0));
    float2 uvPrev = clipPrev.xy / clipPrev.w * 0.5 + float2(0.5, 0.5);

    motionVectorOutput[pixel] = uvPrev - uvCurr;
}
```

Important: The exact `worldPos` and `skyDir` extraction depends on how the split path tracer structures its ray tracing loop. Read the full `SamplePixel` function to find where the primary hit position is computed and ensure `worldPos` is the primary hit's world-space position.

**Step 2: Build and verify**

Run: `python build.py --framework glfw`
Expected: Build succeeds. Static camera still outputs zero MVs (since prev matrices equal current on first frame).

**Step 3: Commit**

```bash
git add shaders/ray_trace/ray_trace_split.cs.slang
git commit -m "feat(reblur): compute screen-space motion vectors in split PT"
```

---

### Task 5: Add Motion Vector Input to Temporal Accumulation Shader

**Files:**
- Modify: `shaders/ray_trace/reblur_temporal_accumulation.cs.slang:1-14` (bindings)
- Modify: `libraries/source/renderer/denoiser/ReblurDenoiser.cpp:76-109` (shader class)
- Modify: `libraries/source/renderer/denoiser/ReblurDenoiser.cpp:595-652` (TemporalAccumulate)

**Step 1: Add motion vector texture binding to the shader**

In `reblur_temporal_accumulation.cs.slang`, add a new binding after the `inViewZ` binding (line 20):

```slang
[[vk::binding(13, 0)]] Texture2D<float2> inMotionVectors;
```

Also add a `SamplerState` for bilinear history sampling:

```slang
[[vk::binding(14, 0)]] SamplerState linearSampler;
```

**Step 2: Add the resource bindings to the C++ shader class**

In `ReblurDenoiser.cpp`, in `ReblurTemporalAccumShader` class (around line 93), add:

```cpp
USE_SHADER_RESOURCE(inMotionVectors, RHIShaderResourceReflection::ResourceType::Texture2D)
USE_SHADER_RESOURCE(linearSampler, RHIShaderResourceReflection::ResourceType::Sampler)
```

**Step 3: Bind the motion vector texture in TemporalAccumulate()**

In the `TemporalAccumulate()` method (around line 603), add:

```cpp
resources->inMotionVectors().BindResource(inputs.motion_vectors->GetDefaultView(rhi_));
// Create and bind a linear sampler for bilinear history sampling
// (or reuse an existing one if available in the denoiser)
```

Note: Check if there's an existing sampler in the codebase (e.g., a linear sampler for the sky map). If not, create one in `ReblurDenoiser::CreatePipelines()`.

**Step 4: Build and verify**

Run: `python build.py --framework glfw`
Expected: Build succeeds. Behavior unchanged (MV texture is bound but not yet read in the shader).

**Step 5: Commit**

```bash
git add shaders/ray_trace/reblur_temporal_accumulation.cs.slang \
      libraries/source/renderer/denoiser/ReblurDenoiser.cpp
git commit -m "feat(reblur): add motion vector input binding to temporal accumulation"
```

---

### Task 6: Implement Bilinear Reprojection in Temporal Accumulation

**Files:**
- Create: `shaders/include/reblur_reprojection.h.slang`
- Modify: `shaders/ray_trace/reblur_temporal_accumulation.cs.slang:67-122`

**Step 1: Create the reprojection utility header**

Create `shaders/include/reblur_reprojection.h.slang`:

```slang
#ifndef REBLUR_REPROJECTION_H
#define REBLUR_REPROJECTION_H

// Bilinear history sampling with per-sample occlusion validation.
// Returns: sampled history value, sets footprintQuality to [0,1].
//
// prevTexture:    history texture to sample
// prevUV:         UV coordinates in previous frame
// resolution:     texture dimensions
// prevViewZ:      previous frame depth buffer
// prevNormalRough: previous frame normal+roughness buffer
// expectedPrevZ:  expected depth at reprojected position
// currentNormal:  current frame surface normal
// disoccThreshold: disocclusion threshold (relative depth)
// denoisingRange: max valid depth
// footprintQuality: output [0,1] quality metric
// isValid:        output: true if any sample is valid
float4 BilinearHistorySample(
    Texture2D<float4> prevTexture,
    float2 prevUV,
    uint2 resolution,
    Texture2D<float> prevViewZ,
    Texture2D<float4> prevNormalRough,
    float expectedPrevZ,
    float3 currentNormal,
    float disoccThreshold,
    float denoisingRange,
    out float footprintQuality,
    out bool isValid)
{
    float2 prevPixelF = prevUV * float2(resolution) - 0.5;
    int2 tl = int2(floor(prevPixelF));
    float2 f = prevPixelF - float2(tl);

    // Bilinear weights
    float4 bw = float4(
        (1.0 - f.x) * (1.0 - f.y),  // tl
        f.x * (1.0 - f.y),            // tr
        (1.0 - f.x) * f.y,            // bl
        f.x * f.y                     // br
    );

    // Offsets for the 4 corners
    int2 offsets[4] = { int2(0,0), int2(1,0), int2(0,1), int2(1,1) };

    float4 occMask = float4(0, 0, 0, 0);
    float4 result = float4(0, 0, 0, 0);

    [unroll]
    for (int i = 0; i < 4; i++)
    {
        int2 p = tl + offsets[i];

        // Bounds check
        if (p.x < 0 || p.y < 0 || p.x >= int(resolution.x) || p.y >= int(resolution.y))
            continue;

        float pz = prevViewZ[p];

        // Sky check
        if (pz > denoisingRange)
            continue;

        // Depth-based occlusion test
        float zDiff = abs(pz - expectedPrevZ);
        float zThresh = disoccThreshold * max(abs(expectedPrevZ), 1.0);
        if (zDiff > zThresh)
            continue;

        // Normal agreement test
        float4 pnr = prevNormalRough[p];
        float3 pn = DecodeNormalOct(pnr.xy);
        if (dot(pn, currentNormal) < 0.5)
            continue;

        // This sample is valid
        occMask[i] = 1.0;
        result += bw[i] * prevTexture[p];
    }

    float weightSum = dot(bw * occMask, float4(1, 1, 1, 1));
    isValid = weightSum > 1e-6;

    if (isValid)
        result /= weightSum;

    // Footprint quality: fraction of valid bilinear area
    footprintQuality = sqrt(dot(bw, occMask));

    return result;
}

// Catmull-Rom (12-tap, no corners) history sampling.
// Only call when all 4 bilinear samples are valid (no disocclusion in footprint).
// Sharpness parameter: 0.5 (NRD convention).
float4 CatmullRomHistorySample(
    Texture2D<float4> prevTexture,
    SamplerState linearSampler,
    float2 prevUV,
    uint2 resolution)
{
    float2 texSize = float2(resolution);
    float2 samplePos = prevUV * texSize;
    float2 centerPos = floor(samplePos - 0.5) + 0.5;
    float2 f = saturate(samplePos - centerPos);

    // Catmull-Rom weights (sharpness = 0.5)
    static const float S = 0.5;
    float2 w0 = f * (f * (-S * f + 2.0 * S) - S);
    float2 w1 = f * (f * ((2.0 - S) * f - (3.0 - S))) + 1.0;
    float2 w2 = f * (f * (-(2.0 - S) * f + (3.0 - 2.0 * S)) + S);
    float2 w3 = f * (f * (S * f - S));

    float2 w12 = w1 + w2;
    float2 tc = w2 / w12;

    float2 invTexSize = 1.0 / texSize;

    // 12-tap pattern (4 cross taps + 4 center ring + 4 center ring, excluding corners)
    // Optimized to 5 bilinear fetches using hardware filtering
    float4 result = float4(0, 0, 0, 0);
    float totalWeight = 0.0;

    // Center 2x2 (combined into 1 bilinear fetch)
    float2 uv12 = (centerPos + tc) * invTexSize;
    float weight12 = w12.x * w12.y;
    result += prevTexture.SampleLevel(linearSampler, uv12, 0) * weight12;
    totalWeight += weight12;

    // Left column (combined into 1 bilinear fetch)
    float2 uv0y = (centerPos + float2(-1.0, tc.y)) * invTexSize;
    float weight0y = w0.x * w12.y;
    result += prevTexture.SampleLevel(linearSampler, uv0y, 0) * weight0y;
    totalWeight += weight0y;

    // Right column
    float2 uv3y = (centerPos + float2(2.0, tc.y)) * invTexSize;
    float weight3y = w3.x * w12.y;
    result += prevTexture.SampleLevel(linearSampler, uv3y, 0) * weight3y;
    totalWeight += weight3y;

    // Top row
    float2 uvx0 = (centerPos + float2(tc.x, -1.0)) * invTexSize;
    float weightx0 = w12.x * w0.y;
    result += prevTexture.SampleLevel(linearSampler, uvx0, 0) * weightx0;
    totalWeight += weightx0;

    // Bottom row
    float2 uvx3 = (centerPos + float2(tc.x, 2.0)) * invTexSize;
    float weightx3 = w12.x * w3.y;
    result += prevTexture.SampleLevel(linearSampler, uvx3, 0) * weightx3;
    totalWeight += weightx3;

    if (totalWeight > 0.0)
        result /= totalWeight;

    return result;
}

#endif // REBLUR_REPROJECTION_H
```

**Step 2: Replace identity reprojection with MV-based bilinear sampling**

In `reblur_temporal_accumulation.cs.slang`, replace lines 67-122 (from `prev_pixel = pixel` through history reading) with:

```slang
#include "reblur_reprojection.h.slang"

// ...inside main(), after reset_history check...

// Reprojection via motion vectors
float2 motionVec = inMotionVectors[pixel];
float2 currentUV = (float2(pixel) + 0.5) / float2(resolution);
float2 prevUV = currentUV + motionVec;

// Check if reprojected position is in screen
bool inScreen = all(prevUV >= 0.0) && all(prevUV <= 1.0);

// Read previous frame data with occlusion-aware bilinear sampling
float footprintQuality = 0.0;
bool historyValid = false;
float4 hist_diff = float4(0, 0, 0, 0);
float4 hist_spec = float4(0, 0, 0, 0);
float prev_diff_accum_speed = 0.0;
float prev_spec_accum_speed = 0.0;

if (inScreen)
{
    // Expected previous-frame viewZ at reprojected position
    // For static world: transform current world position to previous view space
    // Approximation: use depth from the center of the bilinear footprint
    int2 prevPixelCenter = int2(prevUV * float2(resolution));
    prevPixelCenter = clamp(prevPixelCenter, int2(0, 0), int2(resolution) - int2(1, 1));
    float expectedPrevZ = prevViewZ[prevPixelCenter];

    hist_diff = BilinearHistorySample(
        prevDiffHistory, prevUV, resolution,
        prevViewZ, prevNormalRoughness, expectedPrevZ,
        center_normal, disocclusion_threshold, denoising_range,
        footprintQuality, historyValid);

    // Sample specular with same occlusion pattern
    float specFootprint;
    bool specValid;
    hist_spec = BilinearHistorySample(
        prevSpecHistory, prevUV, resolution,
        prevViewZ, prevNormalRoughness, expectedPrevZ,
        center_normal, disocclusion_threshold, denoising_range,
        specFootprint, specValid);

    historyValid = historyValid && specValid;

    // Catmull-Rom upgrade when all samples valid
    if (historyValid && footprintQuality > 0.99)
    {
        hist_diff = CatmullRomHistorySample(prevDiffHistory, linearSampler, prevUV, resolution);
        hist_spec = CatmullRomHistorySample(prevSpecHistory, linearSampler, prevUV, resolution);
    }

    // Read previous accumulation speed from center of footprint
    float2 prev_packed = prevInternalData[prevPixelCenter];
    UnpackInternalData(prev_packed, prev_diff_accum_speed, prev_spec_accum_speed);
}

// Disocclusion: either out-of-screen or no valid history samples
bool disoccluded = !inScreen || !historyValid;
```

Then keep the existing accumulation speed computation and firefly suppression code, but remove the old `prev_pixel`-based history reads (they're now replaced by the bilinear sampling above).

**Step 3: Build and run smoke test**

Run: `python build.py --framework glfw`
Then: `python build.py --framework glfw --run --headless true --pipeline gpu --use_reblur true --spp 1 --max_spp 30 --test_case screenshot`
Expected: Build succeeds. Static camera: should produce identical output (MVs are zero, prevUV = currentUV, bilinear degenerates to point sample).

**Step 4: Commit**

```bash
git add shaders/include/reblur_reprojection.h.slang \
      shaders/ray_trace/reblur_temporal_accumulation.cs.slang
git commit -m "feat(reblur): implement bilinear/Catmull-Rom reprojection in temporal accumulation"
```

---

### Task 7: Add Camera Delta and Framerate Scale to ReblurDenoiser

**Files:**
- Modify: `libraries/include/renderer/denoiser/ReblurDenoiser.h:47-54` (ReblurMatrices)
- Modify: `libraries/include/renderer/denoiser/ReblurDenoiser.h:56-92` (ReblurDenoiser class)
- Modify: `libraries/source/renderer/denoiser/ReblurDenoiser.cpp:387-484` (Denoise method)
- Modify: `libraries/source/renderer/renderer/GPURenderer.cpp:757-767` (populate camera delta)

**Step 1: Add camera_delta and framerate_scale to ReblurMatrices**

In `ReblurDenoiser.h`, add to `ReblurMatrices`:

```cpp
struct ReblurMatrices
{
    Mat4 view_to_clip = Mat4::Identity();
    Mat4 view_to_world = Mat4::Identity();
    Mat4 world_to_clip_prev = Mat4::Identity();
    Mat4 world_to_view_prev = Mat4::Identity();
    Mat4 world_prev_to_world = Mat4::Identity();
    Vector3 camera_delta = Vector3::Zero();
    float framerate_scale = 1.0f;
};
```

**Step 2: Compute framerate_scale in GPURenderer**

In `GPURenderer.cpp`, when populating matrices, compute and set the framerate scale:

```cpp
// Compute framerate scale (base 30 FPS = 33.333ms per frame)
// Use the actual frame time from the rendering loop
float frame_time_ms = /* get from timer or render loop */;
matrices.framerate_scale = std::max(33.333f / std::max(frame_time_ms, 1.0f), 1.0f);
matrices.camera_delta = camera_proxy->GetPositionPrev() - camera_proxy->GetPosture().position;
```

Note: Check how frame time is measured in the engine — there may be an existing timer or delta time mechanism.

**Step 3: Build and verify**

Run: `python build.py --framework glfw`
Expected: Build succeeds.

**Step 4: Commit**

```bash
git add libraries/include/renderer/denoiser/ReblurDenoiser.h \
      libraries/source/renderer/renderer/GPURenderer.cpp
git commit -m "feat(reblur): add camera delta and framerate scale to ReblurMatrices"
```

---

### Task 8: Add Framerate-Scaled Anti-Lag to Temporal Stabilization

**Files:**
- Modify: `shaders/ray_trace/reblur_temporal_stabilization.cs.slang:5-15` (UBO)
- Modify: `shaders/ray_trace/reblur_temporal_stabilization.cs.slang:145-171` (antilag)
- Modify: `shaders/ray_trace/reblur_temporal_stabilization.cs.slang:173-177` (sigma scale)
- Modify: `libraries/source/renderer/denoiser/ReblurDenoiser.cpp:150-181` (UBO struct)
- Modify: `libraries/source/renderer/denoiser/ReblurDenoiser.cpp:743-753` (UBO upload)

**Step 1: Add framerate_scale to the temporal stabilization UBO**

In the shader UBO:

```slang
[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    float stabilization_strength;
    float fast_history_sigma_scale;
    float antilag_sigma_scale;
    float antilag_sensitivity;
    float denoising_range;
    uint frame_index;
    uint max_stabilized_frame_num;
    float framerate_scale;  // NEW
};
```

In the C++ `ReblurTemporalStabShader::UniformBufferData`:

```cpp
struct UniformBufferData
{
    Vector2UInt resolution;
    float stabilization_strength;
    float fast_history_sigma_scale;
    float antilag_sigma_scale;
    float antilag_sensitivity;
    float denoising_range;
    uint32_t frame_index;
    uint32_t max_stabilized_frame_num;
    float framerate_scale;  // NEW
};
```

**Step 2: Update anti-lag to use NRD-style framerate-scaled formula**

Replace the anti-lag section (lines 151-171) with:

```slang
float diff_antilag = 1.0;
float spec_antilag = 1.0;
{
    if (diff_accum_speed >= 8.0)
    {
        float s = diff_sigma * antilag_sigma_scale;
        float magic = antilag_sensitivity * framerate_scale * framerate_scale;
        float clamped = clamp(stab_diff_luma, diff_mean - s, diff_mean + s);
        float d = abs(stab_diff_luma - clamped) / (max(stab_diff_luma, clamped) + 1e-6);
        diff_antilag = 1.0 / (1.0 + d * diff_accum_speed / max(magic, 1e-6));
        diff_accum_speed = lerp(0.0, diff_accum_speed, diff_antilag);
    }

    if (spec_accum_speed >= 8.0)
    {
        float s = spec_sigma * antilag_sigma_scale;
        float magic = antilag_sensitivity * framerate_scale * framerate_scale;
        float clamped = clamp(stab_spec_luma, spec_mean - s, spec_mean + s);
        float d = abs(stab_spec_luma - clamped) / (max(stab_spec_luma, clamped) + 1e-6);
        spec_antilag = 1.0 / (1.0 + d * spec_accum_speed / max(magic, 1e-6));
        spec_accum_speed = lerp(0.0, spec_accum_speed, spec_antilag);
    }
}
```

**Step 3: Update sigma scale to use framerate_scale**

Replace lines 176-177 with:

```slang
float diff_sigma_scale = fast_history_sigma_scale * (1.0 + 3.0 * framerate_scale * diff_non_linear);
float spec_sigma_scale = fast_history_sigma_scale * (1.0 + 3.0 * framerate_scale * spec_non_linear);
```

**Step 4: Upload framerate_scale in C++ UBO**

In `ReblurDenoiser::TemporalStabilize()`, add to the UBO construction:

```cpp
.framerate_scale = matrices.framerate_scale,
```

Note: The `TemporalStabilize` method currently doesn't take `matrices` as a parameter — it takes `(inputs, settings)`. Either:
- Add `const ReblurMatrices& matrices` parameter to `TemporalStabilize`
- Or store `framerate_scale` as a member variable set in `Denoise()`

Choose whichever matches the existing pattern better.

**Step 5: Build and run static camera test**

Run: `python build.py --framework glfw`
Then: `python build.py --framework glfw --run --headless true --pipeline gpu --use_reblur true --spp 1 --max_spp 64 --test_case screenshot`
Expected: Build succeeds. Static camera: framerate_scale=1.0 at 30 FPS, anti-lag behavior unchanged.

**Step 6: Commit**

```bash
git add shaders/ray_trace/reblur_temporal_stabilization.cs.slang \
      libraries/source/renderer/denoiser/ReblurDenoiser.cpp \
      libraries/include/renderer/denoiser/ReblurDenoiser.h
git commit -m "feat(reblur): add framerate-scaled anti-lag to temporal stabilization"
```

---

### Task 9: Add Reprojection to Temporal Stabilization

**Files:**
- Modify: `shaders/ray_trace/reblur_temporal_stabilization.cs.slang:130-137` (history read)
- Modify: `shaders/ray_trace/reblur_temporal_stabilization.cs.slang` (bindings)

**Step 1: Add motion vector binding to temporal stabilization**

Add bindings:

```slang
[[vk::binding(10, 0)]] Texture2D<float2> inMotionVectors;
```

**Step 2: Use motion vectors to sample stabilized history**

Replace the static-camera history read (lines 132-134):

```slang
// Reproject stabilized history via motion vectors
float2 motionVec = inMotionVectors[pixel];
float2 currentUV = (float2(pixel) + 0.5) / float2(resolution);
float2 prevUV = currentUV + motionVec;

float4 stab_diff, stab_spec;
if (all(prevUV >= 0.0) && all(prevUV <= 1.0))
{
    // Simple bilinear sample of stabilized history at reprojected position
    int2 prevPixel = int2(prevUV * float2(resolution));
    prevPixel = clamp(prevPixel, int2(0, 0), int2(resolution) - int2(1, 1));
    stab_diff = prevStabilizedDiff[prevPixel];
    stab_spec = prevStabilizedSpec[prevPixel];
}
else
{
    // Out-of-screen: use current frame
    stab_diff = current_diff;
    stab_spec = current_spec;
}
```

**Step 3: Update C++ to bind motion vectors**

Add the resource binding in `ReblurTemporalStabShader` and the bind call in `TemporalStabilize()`.

**Step 4: Build and run static camera test**

Run: `python build.py --framework glfw`
Expected: Build succeeds. No regression.

**Step 5: Commit**

```bash
git add shaders/ray_trace/reblur_temporal_stabilization.cs.slang \
      libraries/source/renderer/denoiser/ReblurDenoiser.cpp
git commit -m "feat(reblur): add motion vector reprojection to temporal stabilization"
```

---

### Task 10: Implement CameraAnimator for Programmatic Camera Paths

**Files:**
- Create: `libraries/include/scene/component/camera/CameraAnimator.h`
- Create: `libraries/source/scene/component/camera/CameraAnimator.cpp`
- Modify: `libraries/include/renderer/RenderConfig.h:80-93`
- Modify: `libraries/source/renderer/RenderConfig.cpp:33-68`

**Step 1: Add camera_animation config option**

In `RenderConfig.h`, add after `reblur_debug_pass` (line 91):

```cpp
std::string camera_animation;  // "none", "orbit_sweep", "dolly"
```

In `RenderConfig.cpp`, add the config declaration and registration following the existing pattern:

```cpp
static ConfigValue<std::string> config_camera_animation("camera_animation",
    "camera animation path (none, orbit_sweep, dolly)", "renderer", "none");
// ...
ConfigCollectionHelper::RegisterConfig(this, config_camera_animation, camera_animation);
```

**Step 2: Create CameraAnimator class**

`CameraAnimator.h`:

```cpp
#pragma once

#include "core/math/Types.h"

namespace sparkle
{
class CameraAnimator
{
public:
    enum class PathType { kNone, kOrbitSweep, kDolly };

    static PathType FromString(const std::string& name);

    void Setup(PathType type, uint32_t total_frames,
               const Vector3& initial_position, const Vector3& scene_center,
               float initial_radius);

    // Returns camera position and look-at target for the given frame
    struct CameraPose
    {
        Vector3 position;
        Vector3 target;
        Vector3 up;
    };

    CameraPose GetPose(uint32_t frame_index) const;
    bool IsActive() const { return path_type_ != PathType::kNone; }

private:
    PathType path_type_ = PathType::kNone;
    uint32_t total_frames_ = 1;
    Vector3 initial_position_ = Vector3::Zero();
    Vector3 scene_center_ = Vector3::Zero();
    float initial_radius_ = 1.0f;
    float initial_height_ = 0.0f;
};
} // namespace sparkle
```

`CameraAnimator.cpp`:

```cpp
#include "scene/component/camera/CameraAnimator.h"
#include <cmath>

namespace sparkle
{
CameraAnimator::PathType CameraAnimator::FromString(const std::string& name)
{
    if (name == "orbit_sweep") return PathType::kOrbitSweep;
    if (name == "dolly") return PathType::kDolly;
    return PathType::kNone;
}

void CameraAnimator::Setup(PathType type, uint32_t total_frames,
                            const Vector3& initial_position, const Vector3& scene_center,
                            float initial_radius)
{
    path_type_ = type;
    total_frames_ = std::max(total_frames, 1u);
    initial_position_ = initial_position;
    scene_center_ = scene_center;
    initial_radius_ = initial_radius;
    initial_height_ = initial_position.z();  // Assuming Z-up
}

CameraAnimator::CameraPose CameraAnimator::GetPose(uint32_t frame_index) const
{
    CameraPose pose;
    pose.up = Vector3(0, 0, 1);  // Z-up (engine convention)
    pose.target = scene_center_;

    float t = static_cast<float>(frame_index) / static_cast<float>(total_frames_);

    switch (path_type_)
    {
    case PathType::kOrbitSweep:
    {
        float angle = t * 2.0f * static_cast<float>(M_PI);
        pose.position = Vector3(
            scene_center_.x() + std::cos(angle) * initial_radius_,
            scene_center_.y() + std::sin(angle) * initial_radius_,
            initial_height_
        );
        break;
    }
    case PathType::kDolly:
    {
        // Forward/back along initial viewing direction
        Vector3 dir = (scene_center_ - initial_position_).normalized();
        float distance = std::sin(t * 2.0f * static_cast<float>(M_PI)) * initial_radius_ * 0.3f;
        pose.position = initial_position_ + dir * distance;
        break;
    }
    default:
        pose.position = initial_position_;
        break;
    }

    return pose;
}
} // namespace sparkle
```

**Step 3: Build**

Run: `python build.py --framework glfw`
Expected: Build succeeds.

**Step 4: Commit**

```bash
git add libraries/include/scene/component/camera/CameraAnimator.h \
      libraries/source/scene/component/camera/CameraAnimator.cpp \
      libraries/include/renderer/RenderConfig.h \
      libraries/source/renderer/RenderConfig.cpp
git commit -m "feat(reblur): add CameraAnimator and --camera_animation config"
```

---

### Task 11: Integrate CameraAnimator into the Render Loop

**Files:**
- Modify: `libraries/source/application/AppFramework.cpp` (MainLoop or Tick)
- Modify: `libraries/include/application/AppFramework.h`

**Step 1: Add CameraAnimator member to AppFramework**

In `AppFramework.h`, add a `CameraAnimator` member and include the header:

```cpp
#include "scene/component/camera/CameraAnimator.h"

// ...in private members:
CameraAnimator camera_animator_;
```

**Step 2: Initialize CameraAnimator on startup**

After the scene is loaded and camera is configured, initialize the animator:

```cpp
auto path_type = CameraAnimator::FromString(render_config_.camera_animation);
if (path_type != CameraAnimator::PathType::kNone)
{
    auto* camera = GetMainCamera();
    auto posture = camera->GetRenderProxy()->GetPosture();
    float radius = (posture.position - scene_center).norm();
    camera_animator_.Setup(path_type, render_config_.max_sample_per_pixel,
                          posture.position, scene_center, radius);
}
```

**Step 3: Apply camera animation each frame**

In the main loop, before the camera Update is called:

```cpp
if (camera_animator_.IsActive())
{
    auto pose = camera_animator_.GetPose(frame_index);
    // Set the camera transform from the animator pose
    // This depends on how OrbitCameraComponent sets its transform
    auto* camera = GetMainCamera();
    camera->SetPositionAndLookAt(pose.position, pose.target, pose.up);
}
```

Note: The exact API to set camera position depends on the CameraComponent/OrbitCameraComponent interface. Read `OrbitCameraComponent` to find the right method (may need to call `SetCenter()` + adjust pitch/yaw/radius, or directly set the transform).

**Step 4: Build and run with animation**

Run: `python build.py --framework glfw`
Then: `python build.py --framework glfw --run --headless true --pipeline gpu --use_reblur true --spp 1 --max_spp 30 --camera_animation orbit_sweep --test_case multi_frame_screenshot`
Expected: Build succeeds. Screenshots show different camera angles across frames.

**Step 5: Commit**

```bash
git add libraries/include/application/AppFramework.h \
      libraries/source/application/AppFramework.cpp
git commit -m "feat(reblur): integrate CameraAnimator into render loop"
```

---

### Task 12: Motion Vector Debug Visualization

**Files:**
- Modify: `shaders/ray_trace/reblur_composite.cs.slang` or add debug output path
- Modify: `libraries/include/renderer/RenderConfig.h` (add debug mode)

**Step 1: Add motion_vectors debug mode**

Add a new debug mode value (following the existing `DebugMode` enum pattern in `RenderConfig.h`) for motion vector visualization.

**Step 2: In the composite or split PT shader, add a debug path**

When the motion vector debug mode is active, output MV magnitude as a heatmap:

```slang
if (debug_mode == DEBUG_MODE_MOTION_VECTORS)
{
    float2 mv = motionVectorOutput[pixel];
    float magnitude = length(mv * float2(resolution));
    // Map 0-20 pixels motion to blue-red heatmap
    float t = saturate(magnitude / 20.0);
    float3 color = lerp(float3(0, 0, 1), float3(1, 0, 0), t);
    imageData[pixel] = float4(color, 1.0);
    return;
}
```

**Step 3: Build and verify**

Run: `python build.py --framework glfw`
Expected: Build succeeds.

**Step 4: Commit**

```bash
git add shaders/ray_trace/ray_trace_split.cs.slang \
      libraries/include/renderer/RenderConfig.h
git commit -m "feat(reblur): add motion vector debug visualization"
```

---

### Task 13: Static Camera Non-Regression Test

**Files:**
- Modify: `tests/reblur/reblur_test_suite.py`

**Step 1: Add a non-regression test to the test suite**

Add a test that runs the REBLUR pipeline with `--camera_animation none` and verifies the output matches the existing static camera baseline (FLIP <= 0.08):

```python
# Test 11: Camera motion non-regression (static camera unchanged)
label = "11. Static camera non-regression"
ok, dur, log = run_command([
    sys.executable, "build.py", "--framework", args.framework,
    "--run", "--headless", "true", "--pipeline", "gpu",
    "--use_reblur", "true", "--spp", "1", "--max_spp", "64",
    "--camera_animation", "none",
    "--test_case", "screenshot"
], label)
if ok:
    ok = validate_latest_screenshot(args.framework, label)
results.append((label, ok, dur))
```

**Step 2: Run the full test suite**

Run: `python tests/reblur/reblur_test_suite.py --framework glfw`
Expected: All existing tests pass. New test passes.

**Step 3: Commit**

```bash
git add tests/reblur/reblur_test_suite.py
git commit -m "test(reblur): add static camera non-regression test for motion support"
```

---

### Task 14: Camera Motion Smoke Test

**Files:**
- Modify: `tests/reblur/reblur_test_suite.py`

**Step 1: Add motion smoke test**

```python
# Test 12: Camera motion smoke — orbit sweep, no crash, no NaN
label = "12. Camera motion smoke (orbit_sweep)"
ok, dur, log = run_command([
    sys.executable, "build.py", "--framework", args.framework,
    "--run", "--headless", "true", "--pipeline", "gpu",
    "--use_reblur", "true", "--spp", "1", "--max_spp", "60",
    "--camera_animation", "orbit_sweep",
    "--test_case", "screenshot"
], label)
if ok:
    ok = validate_latest_screenshot(args.framework, label)
results.append((label, ok, dur))
```

**Step 2: Run test**

Run: `python tests/reblur/reblur_test_suite.py --framework glfw`
Expected: Motion smoke test passes — no crash, screenshot captured, no NaN.

**Step 3: Commit**

```bash
git add tests/reblur/reblur_test_suite.py
git commit -m "test(reblur): add camera motion smoke test"
```

---

### Task 15: Camera Motion Quality and Stability Tests

**Files:**
- Create: `tests/reblur/reblur_motion_validation.py`
- Modify: `tests/reblur/reblur_test_suite.py`

**Step 1: Create motion validation test script**

Create `tests/reblur/reblur_motion_validation.py` that:
1. Runs orbit_sweep for 30 frames, then static for 30 frames (total 60)
2. Takes multi-frame screenshots
3. Validates:
   - No NaN/Inf in any frame
   - During motion: no persistent ghosting (max temporal diff < threshold)
   - After stopping: reconverges (FLIP vs reference < 0.12)
   - Frame-to-frame stability during motion (std-dev < 0.03)

Follow the pattern from `tests/reblur/reblur_temporal_validation.py` for how to:
- Launch the app with specific settings
- Capture and analyze screenshots
- Compute per-pixel statistics

**Step 2: Add to test suite**

```python
# Test 13: Camera motion quality validation
label = "13. Camera motion quality validation"
ok, dur, _ = run_command([
    sys.executable, "tests/reblur/reblur_motion_validation.py",
    "--framework", args.framework
], label, show_output=True)
results.append((label, ok, dur))
```

**Step 3: Run test**

Run: `python tests/reblur/reblur_motion_validation.py --framework glfw`
Expected: All quality metrics pass.

**Step 4: Commit**

```bash
git add tests/reblur/reblur_motion_validation.py \
      tests/reblur/reblur_test_suite.py
git commit -m "test(reblur): add camera motion quality and stability validation"
```

---

### Task 16: Tune Parameters and Final Integration Test

**Files:**
- Possibly modify: `libraries/include/renderer/denoiser/ReblurDenoiser.h` (settings defaults)
- Possibly modify: `shaders/ray_trace/reblur_temporal_accumulation.cs.slang` (thresholds)

**Step 1: Run full test suite and analyze**

Run: `python tests/reblur/reblur_test_suite.py --framework glfw`

Analyze any failing tests. Common tuning points:
- `disocclusion_threshold`: May need adjustment for camera motion (0.01 may be too tight)
- Normal agreement threshold (0.5 cosine): May need relaxation for moving camera
- Anti-lag sensitivity: Balance between ghosting rejection and noise

**Step 2: Tune parameters to pass all tests**

Adjust defaults in `ReblurSettings` or threshold constants in shaders as needed.

**Step 3: Run full test suite again**

Run: `python tests/reblur/reblur_test_suite.py --framework glfw`
Expected: All 13+ tests pass.

**Step 4: Commit**

```bash
git add -u
git commit -m "fix(reblur): tune parameters for camera motion quality"
```

---

### Task 17: Update Documentation

**Files:**
- Modify: `docs/implementation/ASVGF.md` or create `docs/implementation/ReblurCameraMotion.md`
- Modify: `docs/Run.md` (document --camera_animation flag)
- Modify: `docs/Test.md` (document new motion tests)

**Step 1: Document new CLI flags**

In `docs/Run.md`, add `--camera_animation` to the arguments table.

**Step 2: Document new test commands**

In `docs/Test.md`, add the motion validation tests.

**Step 3: Commit**

```bash
git add docs/
git commit -m "docs(reblur): document camera motion support and tests"
```
