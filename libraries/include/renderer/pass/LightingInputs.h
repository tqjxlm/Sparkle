#pragma once

#include "renderer/graph/RenderGraph.h"

namespace sparkle
{
class ImageBasedLighting;

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
        builder.SampledOrPlaceholder(shadow_map, placeholder_2d, &Table::shadow_map, &Table::shadow_map_sampler);
        builder.SampledOrPlaceholder(ibl_brdf, placeholder_2d, &Table::ibl_brdf, &Table::ibl_brdf_sampler);
        builder.SampledOrPlaceholder(ibl_diffuse, placeholder_cube, &Table::ibl_diffuse, &Table::ibl_diffuse_sampler);
        builder.SampledOrPlaceholder(ibl_specular, placeholder_cube, &Table::ibl_specular,
                                     &Table::ibl_specular_sampler);
    }

private:
    [[nodiscard]] static RHIResourceRef<RHIImage> GetPlaceholder(RHIContext *rhi, RHIImage::ImageType type);
};
} // namespace sparkle
