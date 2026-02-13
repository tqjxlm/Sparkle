#pragma once

#include "application/ConfigCollection.h"

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
    bool use_ssao;
    bool use_prepass;
    bool use_diffuse_ibl;
    bool use_specular_ibl;
    bool use_vsync;
    bool render_ui = false;
    bool use_dynamic_spp;
    bool enable_nee;
    bool auto_screenshot;
    float target_framerate;
    float gpu_time_budget_ratio;

protected:
    void Validate() override;

    RHIContext *rhi_ = nullptr;
    NativeView *view_ = nullptr;

    friend class AppFramework;
};
} // namespace sparkle
