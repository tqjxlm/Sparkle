#include "renderer/pass/LightingInputs.h"

#include "renderer/resource/ImageBasedLighting.h"
#include "rhi/RHI.h"

namespace sparkle
{
LightingInputs LightingInputs::Import(RenderGraph &graph, RGTexture shadow_map, const ImageBasedLighting *ibl)
{
    LightingInputs inputs{.shadow_map = shadow_map, .ibl_brdf = {}, .ibl_diffuse = {}, .ibl_specular = {}};
    if (!ibl)
    {
        return inputs;
    }

    const auto import_map = [&graph](std::string name, const RHIResourceRef<RHIImage> &map) {
        return map ? graph.Import(std::move(name), map) : RGTexture{};
    };
    inputs.ibl_brdf = import_map("IblBrdf", ibl->GetBRDFMap());
    inputs.ibl_diffuse = import_map("IblDiffuse", ibl->GetDiffuseMap());
    inputs.ibl_specular = import_map("IblSpecular", ibl->GetSpecularMap());
    return inputs;
}

RHIResourceRef<RHIImage> LightingInputs::GetPlaceholder(RHIContext *rhi, RHIImage::ImageType type)
{
    return rhi->GetOrCreateDummyTexture(RHIImage::Attribute{
        .format = PixelFormat::RGBAFloat16,
        .usages = RHIImage::ImageUsage::Texture,
        .type = type,
    });
}
} // namespace sparkle
