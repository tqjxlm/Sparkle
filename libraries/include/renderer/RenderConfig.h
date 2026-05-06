#pragma once

#include "application/ConfigCollection.h"

#include <string>

namespace sparkle
{
class RHIContext;
class NativeView;

struct RenderConfig : public ConfigCollection
{
    enum class Pipeline : uint8_t
    {
        cpu,
        gpu,
        forward,
        // forward_rt,
        deferred,
        // deferred_rt,
    };

    enum class OutputImage : uint8_t
    {
        SceneColor,
        IBL_BrdfTexture,
        IBL_DiffuseMap,
        IBL_SpecularMap,
    };

    enum class DebugMode : uint8_t
    {
        Color = 0,               // default output
        Debug = 1,               // temporary debug output, default to zero
        RayDepth = 2,            // ray bounce depth, only valid for ray-traced mode
        Normal = 3,              // world normal of first hit
        RayDirection = 4,        // ray out direction of first hit
        IndirectLighting = 5,    // lighting that comes from other surfaces or environment
        DirectionalLighting = 6, // lighting that comes directly from light source
        Metallic = 7,
        Roughness = 8,
        Albedo = 9,
        Emissive = 10,
        Depth = 11,
        MotionVectors = 12,
    };

    enum class ReblurDebugPass : uint8_t
    {
        Full,           // run full denoiser pipeline (default)
        PrePass,        // output after PrePass only
        Blur,           // output after Blur
        BlurSpecular,   // output after Blur, specular term only
        PostBlur,       // output after PostBlur (skip TemporalStabilization)
        PostBlurSpecular, // output after PostBlur, specular term only
        TemporalAccum,  // output after TemporalAccumulation
        TemporalAccumSpecular, // output after TemporalAccumulation, specular term only
        HistoryFix,     // output after HistoryFix
        HistoryFixSpecular, // output after HistoryFix, specular term only
        TADisocclusion, // temporal accum diagnostic: disocclusion
        TAMotionVector, // temporal accum diagnostic: motion vector
        TADepth,        // temporal accum diagnostic: depth
        TAHistory,      // temporal accum diagnostic: raw reprojected history
        TASpecHistory,  // temporal accum diagnostic: raw reprojected specular history
        TAMaterialId,   // temporal accum diagnostic: material ID mismatch
        TAAccumSpeed,   // temporal accum diagnostic: current/previous accum speed + footprint quality
        TASpecAccumSpeed, // temporal accum diagnostic: specular accum speed + footprint quality
        TASpecMotionInputs, // temporal accum diagnostic: spec accum / spec history quality / full-footprint-valid
        TASpecQualityDelta, // temporal accum diagnostic: amplified footprint/quality deficits + full-footprint-valid
        TASpecSurfaceInputs, // temporal accum diagnostic: roughness / normalized hit distance / spec magic curve
        TAMotionVectorFine, // temporal accum diagnostic: motion vector with finer subpixel scale
        TAPlaneDistance, // temporal accum diagnostic: NRD-style plane-distance mismatch vs current validity
        TSStabCount,    // temporal stabilization diagnostic: stab_count, blend, antilag
        TSSpecBlend,    // temporal stabilization diagnostic: specular blend / antilag / footprint
        TSSpecAntilagInputs, // temporal stabilization diagnostic: divergence / incoming spec confidence / outgoing spec confidence
        TSSpecClampInputs, // temporal stabilization diagnostic: history delta / clamp band / divergence
        TSDiffClampInputs, // temporal stabilization diagnostic: diffuse history delta / clamp band / divergence
        StabilizedDiffuse,  // output after temporal stabilization, diffuse term only, before albedo remodulation
        StabilizedSpecular, // output after temporal stabilization, specular term only, before final composite
        InputComposite, // raw split input composite: diff * albedo + spec
        CompositeDiffuse,  // final composite diffuse term only: denoisedDiffuse * stabilized albedo
        CompositeDiffuseRawAlbedo, // final composite diffuse term only: denoisedDiffuse * current-frame albedo
        CompositeSpecular, // final composite specular term only: denoisedSpecular
        StabilizedAlbedo,  // stabilized composite albedo only
        Passthrough,    // no denoising, use raw split PT output
    };

    [[nodiscard]] bool IsCPURenderMode() const
    {
        return pipeline == Pipeline::cpu;
    }

    [[nodiscard]] bool IsRayTracingMode() const
    {
        return pipeline == Pipeline::gpu;
    }

    [[nodiscard]] bool IsRaterizationMode() const
    {
        return pipeline == Pipeline::forward || pipeline == Pipeline::deferred;
    }

    void Init();

    void SetupBackend(RHIContext *rhi, NativeView *view)
    {
        rhi_ = rhi;
        view_ = view;
        Validate();
    }

    Pipeline pipeline;
    OutputImage output_image;
    DebugMode debug_mode;
    uint32_t sample_per_pixel;
    uint32_t max_sample_per_pixel;
    uint32_t max_bounce;
    uint32_t image_width;
    uint32_t image_height;
    uint32_t shadow_map_resolution;
    bool spatial_denoise;
    bool use_reblur;
    bool use_ssao;
    bool use_prepass;
    bool use_diffuse_ibl;
    bool use_specular_ibl;
    bool use_vsync;
    bool render_ui = false;
    bool use_dynamic_spp;
    bool enable_nee;
    bool clear_screenshots;
    bool measure_gpu_convergence = false;
    float gpu_convergence_threshold = 0.01f;
    uint32_t gpu_convergence_stability_frames = 16;
    ReblurDebugPass reblur_debug_pass;
    float target_framerate;
    float gpu_time_budget_ratio;
    bool reblur_no_pt_blend; // force composite to use pure denoised output
    float reblur_ghosting_yaw_step = 3.0f; // test-only override for repeated camera-nudge size

protected:
    void Validate() override;

    RHIContext *rhi_ = nullptr;
    NativeView *view_ = nullptr;

    friend class AppFramework;
};
} // namespace sparkle
