#pragma once

#include "renderer/graph/RenderGraph.h"
#include "renderer/resource/ImageBasedLighting.h"

namespace sparkle
{
// the shadow map and IBL maps the forward base pass and deferred lighting sample. a missing one is an invalid texture:
// there is no directional light, or the IBL map is not ready.
struct LightingInputs
{
    RGTexture shadow_map;
    RGTexture ibl_brdf;
    RGTexture ibl_diffuse;
    RGTexture ibl_specular;

    // `shadow_map` and the IBL maps of `ibl` that are ready, imported
    [[nodiscard]] static LightingInputs Import(RenderGraph &graph, RGTexture shadow_map, const ImageBasedLighting *ibl);

    // declares the inputs sampled through the members of the same names in `Table`, binding placeholders for missing
    // ones
    template <class Table> void Sample(RGBuilder &builder, RHIContext *rhi) const
    {
        const auto placeholder_2d = GetPlaceholder(rhi, RHIImage::ImageType::Image2D);
        const auto placeholder_cube = GetPlaceholder(rhi, RHIImage::ImageType::Image2DCube);
        builder.SampledOrPlaceholder(shadow_map, placeholder_2d, &Table::shadow_map, &Table::shadow_map_sampler,
                                     ShadowMapSampler);
        builder.SampledOrPlaceholder(ibl_brdf, placeholder_2d, &Table::ibl_brdf, &Table::ibl_brdf_sampler,
                                     ImageBasedLighting::MapSampler);
        builder.SampledOrPlaceholder(ibl_diffuse, placeholder_cube, &Table::ibl_diffuse, &Table::ibl_diffuse_sampler,
                                     ImageBasedLighting::MapSampler);
        builder.SampledOrPlaceholder(ibl_specular, placeholder_cube, &Table::ibl_specular, &Table::ibl_specular_sampler,
                                     ImageBasedLighting::MapSampler);
    }

private:
    static constexpr RHISampler::SamplerAttribute ShadowMapSampler{
        .address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest};

    [[nodiscard]] static RHIResourceRef<RHIImage> GetPlaceholder(RHIContext *rhi, RHIImage::ImageType type);
};
} // namespace sparkle
