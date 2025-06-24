#include "renderer/proxy/MaterialRenderProxy.h"

#include "io/Material.h"
#include "renderer/BindlessManager.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "rhi/RHI.h"

namespace sparkle
{
MaterialRenderProxy::MaterialRenderData::MaterialRenderData(const MaterialResource &material)
    : baseColor(material.base_color), emissiveColor(material.emissive_color), metallic(material.metallic),
      roughness(material.roughness), eta(material.eta)
{
}

MaterialRenderProxy::MaterialRenderProxy(const MaterialResource &raw_material)
    : raw_material_(raw_material), render_data_(raw_material)
{
}

MaterialRenderProxy::~MaterialRenderProxy() = default;

RHIResourceRef<RHIImage> MaterialRenderProxy::CreateAndRegisterTexture(RHIContext *rhi,
                                                                       const std::shared_ptr<const Image2D> &image,
                                                                       uint32_t &out_id, const std::string &name)
{
    if (!image)
    {
        return rhi->GetOrCreateDummyTexture(RHIImage::Attribute{
            .format = PixelFormat::R8G8B8A8_SRGB,
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
            .usages = RHIImage::ImageUsage::Texture,
        });
    }

    RHIResourceRef<RHIImage> rhi_image = rhi->CreateTexture(image.get(), name);

    if (use_bindless_)
    {
        scene_proxy_->GetBindlessManager()->RegisterTexture(rhi_image);
        out_id = rhi_image->GetBindlessId();
    }
    else
    {
        out_id = 0;
    }

    return rhi_image;
}

void MaterialRenderProxy::InitRenderResources(RHIContext *rhi, const RenderConfig &config)
{
    if (rhi_initialized_)
    {
        return;
    }

    use_bindless_ = config.IsRayTracingMode();

    auto name = raw_material_.name;

    base_color_texture_ = CreateAndRegisterTexture(rhi, raw_material_.base_color_texture,
                                                   render_data_.baseColorTextureId, name + "_BaseColor");
    normal_texture_ =
        CreateAndRegisterTexture(rhi, raw_material_.normal_texture, render_data_.normalTextureId, name + "_Normal");
    emissive_texture_ = CreateAndRegisterTexture(rhi, raw_material_.emissive_texture, render_data_.emissiveTextureId,
                                                 name + "_Emissive");
    metallic_roughness_texture_ =
        CreateAndRegisterTexture(rhi, raw_material_.metallic_roughness_texture, render_data_.metallicRoughnessTextureId,
                                 name + "_MetallicRoughness");

    parameter_buffer_ =
        rhi->CreateBuffer({.size = sizeof(MaterialRenderData),
                           .usages = RHIBuffer::BufferUsage::UniformBuffer,
                           .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                           .is_dynamic = false},
                          "MaterialParameters_" + name);
    parameter_buffer_->UploadImmediate(&render_data_);

    rhi_initialized_ = true;
}

void MaterialRenderProxy::PrintSample(const Vector2 &tex_coord) const
{
    Log(Info, "base color: {} | emissive: {} | normal: {} | roughness: {} | metallic: {} | eta: {}",
        utilities::VectorToString(GetBaseColor(tex_coord)), utilities::VectorToString(GetEmissive(tex_coord)),
        utilities::VectorToString(GetNormal(tex_coord)), GetRoughness(tex_coord), GetMetallic(tex_coord),
        raw_material_.eta);
}

void MaterialRenderProxy::Destroy()
{
    if (use_bindless_)
    {
        scene_proxy_->GetBindlessManager()->UnregisterTexture(base_color_texture_.get());
        scene_proxy_->GetBindlessManager()->UnregisterTexture(normal_texture_.get());
        scene_proxy_->GetBindlessManager()->UnregisterTexture(emissive_texture_.get());
        scene_proxy_->GetBindlessManager()->UnregisterTexture(metallic_roughness_texture_.get());
    }
}
} // namespace sparkle
