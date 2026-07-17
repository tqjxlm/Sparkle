#pragma once

#include "application/ConfigCollection.h"
#include "renderer/RenderResolution.h"

namespace sparkle
{
class RHIContext;
class NativeView;

struct RenderConfig : public ConfigCollection
{
    enum class Pipeline : uint8_t
    {
        Cpu,
        Gpu,
        Forward,
        Deferred,
    };

    enum class OutputImage : uint8_t
    {
        SceneColor,
        IBLBrdfTexture,
        IBLDiffuseMap,
        IBLSpecularMap,
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
    };

    [[nodiscard]] bool IsCPURenderMode() const
    {
        return pipeline == Pipeline::Cpu;
    }

    [[nodiscard]] bool IsRayTracingMode() const
    {
        return pipeline == Pipeline::Gpu;
    }

    [[nodiscard]] bool IsRaterizationMode() const
    {
        return pipeline == Pipeline::Forward || pipeline == Pipeline::Deferred;
    }

    void Init();

    [[nodiscard]] RenderResolution GetResolution() const
    {
        return {{image_width, image_height}, render_scale};
    }

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
    // Added to the per-frame RNG seed. 0 = deterministic (default; functional tests rely on it). A test
    // harness varies it across runs to draw independent stochastic realizations of the same render state,
    // so their average filters the stochastic noise (used by the per-pixel-vs-GT fidelity criterion).
    uint32_t random_seed_offset;
    uint32_t max_bounce;
    uint32_t image_width;
    uint32_t image_height;
    uint32_t shadow_map_resolution;
    bool spatial_denoise;
    bool use_ssao;
    bool use_prepass;
    bool use_diffuse_ibl;
    bool use_specular_ibl;
    bool use_vsync;
    bool render_ui = false;
    bool use_dynamic_spp;
    bool enable_nee;
    bool clear_screenshots;
    bool manual_accumulation;
    float target_framerate;
    float gpu_time_budget_ratio;
    float render_scale;

    // manual-accumulation hold states. Not ConfigValues: the app layer rewrites them every frame
    // (space key / the panel button) and the per-frame snapshot carries them to the render thread.
    bool accumulate_key_held = false;
    bool accumulate_button_held = false;

protected:
    void Validate() override;

    RHIContext *rhi_ = nullptr;
    NativeView *view_ = nullptr;

    friend class AppFramework;
};
} // namespace sparkle
