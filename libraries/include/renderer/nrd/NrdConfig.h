#pragma once

#include "application/ConfigCollection.h"

namespace sparkle
{
// Which NRD channel to visualize. Order/values must stay in sync with the `mode` switch in
// shaders/nrd/nrd_resolve.cs.slang.
enum class NrdDebugMode : uint8_t
{
    None = 0,        // denoised, re-modulated composite (the normal NRD output)
    ViewZ,           // IN_VIEWZ (grayscale; sky saturates white)
    NormalRoughness, // IN_NORMAL_ROUGHNESS normal (remapped to color)
    Roughness,       // IN_NORMAL_ROUGHNESS roughness (grayscale)
    MotionVector,    // IN_MV (amplified; static camera => black)
    DiffRadiance,    // IN_DIFF_RADIANCE_HITDIST radiance (demodulated diffuse)
    DiffHitDist,     // IN_DIFF_RADIANCE_HITDIST normalized hit distance
    SpecRadiance,    // IN_SPEC_RADIANCE_HITDIST radiance (demodulated specular)
    SpecHitDist,     // IN_SPEC_RADIANCE_HITDIST normalized hit distance
    DenoisedDiff,    // OUT_DIFF_RADIANCE_HITDIST radiance (post-ReBLUR, pre re-modulation)
    DenoisedSpec,    // OUT_SPEC_RADIANCE_HITDIST radiance
    Validation       // NRD's built-in validation overlay (OUT_VALIDATION; needs CommonSettings::enableValidation)
};

// Self-contained config for the NRD denoiser on the gpu path tracer. All NRD switches live here, not
// in RenderConfig.
struct NrdConfig : public ConfigCollection
{
    void Init();

    // App-lifetime singleton shared by the denoiser (render thread) and the control panel (main thread);
    // stable lifetime avoids a dangling pointer across renderer recreation. Unlike RenderConfig (which the
    // render thread receives as a per-frame snapshot), writes here are unsynchronized: consumers must
    // sample each flag once per frame (see GPURenderer::Update).
    static NrdConfig &Get();

    bool enabled = false;
    NrdDebugMode debug_mode = NrdDebugMode::None;

protected:
    void Validate() override
    {
    }
};
} // namespace sparkle
