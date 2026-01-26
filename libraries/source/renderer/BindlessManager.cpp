#include "renderer/BindlessManager.h"

#include "renderer/proxy/MaterialRenderProxy.h"
#include "renderer/proxy/MeshRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "rhi/RHI.h"
#include "rhi/RHIResourceArray.h"

namespace sparkle
{
static constexpr unsigned BaseBufferSize = 1024;

void BindlessManager::InitRenderResources(RHIContext *rhi)
{
    ASSERT(!is_valid_);

    texture_array_ =
        rhi->CreateBindlessResourceArray(RHIShaderResourceReflection::ResourceType::Texture2D, "BindlessTextureArray");

    index_buffer_array_ = rhi->CreateBindlessResourceArray(RHIShaderResourceReflection::ResourceType::StorageBuffer,
                                                           "BindlessIndexBufferArray");

    vertex_buffer_array_ = rhi->CreateBindlessResourceArray(RHIShaderResourceReflection::ResourceType::StorageBuffer,
                                                            "BindlessVertexBufferArray");

    vertex_attribute_buffer_array_ = rhi->CreateBindlessResourceArray(
        RHIShaderResourceReflection::ResourceType::StorageBuffer, "BindlessVertexAttributeBufferArray");

    material_parameter_buffer_ =
        rhi->CreateBuffer({.size = sizeof(MaterialRenderProxy) * BaseBufferSize,
                           .usages = RHIBuffer::BufferUsage::StorageBuffer,
                           .mem_properties = RHIMemoryProperty::HostCoherent | RHIMemoryProperty::HostVisible,
                           .is_dynamic = false},
                          "MaterialParameterBuffer");

    material_id_buffer_ =
        rhi->CreateBuffer({.size = sizeof(uint32_t) * BaseBufferSize,
                           .usages = RHIBuffer::BufferUsage::StorageBuffer,
                           .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                           .is_dynamic = false},
                          "MaterialIdBuffer");

    is_valid_ = true;
}

void BindlessManager::RegisterTexture(const RHIResourceRef<RHIImage> &rhi_image)
{
    ASSERT(is_valid_);

    // textures are stored in texture_array_ and referenced by others with an id.
    // once a texture get an id via RegisterTexture, its id will never change.
    // newly registered textures will reuse free ids, keeping this array as contiguous as possible.

    ASSERT(rhi_image);

    auto found = texture_ref_count_.find(rhi_image.get());
    if (found != texture_ref_count_.end())
    {
        found->second++;
        return;
    }

    texture_ref_count_.insert({rhi_image.get(), 1});

    new_textures_.push_back(rhi_image.get());

    uint32_t texture_id;
    if (!free_texture_ids_.empty())
    {
        texture_id = *free_texture_ids_.begin();
        free_texture_ids_.erase(texture_id);

        registered_textures_[texture_id] = rhi_image;
    }
    else
    {
        texture_id = static_cast<uint32_t>(registered_textures_.size());

        registered_textures_.push_back(rhi_image);
    }

    rhi_image->SetBindlessId(texture_id);
}

void BindlessManager::UnregisterTexture(RHIImage *rhi_image)
{
    ASSERT(is_valid_);

    if (!rhi_image || !rhi_image->IsRegisteredAsBindless())
    {
        return;
    }

    auto found = texture_ref_count_.find(rhi_image);
    ASSERT(found != texture_ref_count_.end());

    found->second--;

    if (found->second == 0)
    {
        auto texture_id = rhi_image->GetBindlessId();
        free_texture_ids_.insert(texture_id);

        rhi_image->SetBindlessId(UINT_MAX);

        registered_textures_[texture_id].reset();

        texture_ref_count_.erase(rhi_image);

        removed_textures_.push_back(texture_id);
    }
}

void BindlessManager::UpdatePrimitive(PrimitiveRenderProxy *primitive)
{
    ASSERT(is_valid_);

    if (!primitive->IsMesh())
    {
        return;
    }

    const auto *mesh = primitive->As<MeshRenderProxy>();

    auto id = primitive->GetPrimitiveIndex();

    index_buffer_array_->SetResourceAt(mesh->GetIndexBuffer(), id);
    vertex_buffer_array_->SetResourceAt(mesh->GetVertexBuffer(), id);
    vertex_attribute_buffer_array_->SetResourceAt(mesh->GetVertexAttribBuffer(), id);
}

static bool ResizeBufferIfNeeded(RHIContext *rhi, RHIResourceRef<RHIBuffer> &buffer, size_t element_size,
                                 size_t requested_element_count)
{
    if (requested_element_count * element_size <= buffer->GetSize())
    {
        return false;
    }

    buffer = rhi->CreateBuffer({.size = buffer->GetSize() * 2,
                                .usages = buffer->GetUsage(),
                                .mem_properties = buffer->GetMemoryProperty(),
                                .is_dynamic = buffer->IsDynamic()},
                               buffer->GetName());

    return true;
}

void BindlessManager::UpdateFrameData(RHIContext *rhi)
{
    ASSERT(is_valid_);

    is_buffer_dirty_ = false;

    {
        // update primitives
        std::vector<uint32_t> data_to_update;
        std::vector<uint32_t> id_to_update;

        for (const auto &[type, primitive, from, to] : scene_proxy_->GetPrimitiveChangeList())
        {
            switch (type)
            {
            case SceneRenderProxy::PrimitiveChangeType::New:
            case SceneRenderProxy::PrimitiveChangeType::Move: {
                // we treat new primitives and updated primitives in exactly the same way,
                // as we do not care about empty or stale ids which will not be referenced anyway
                if (primitive->GetPrimitiveIndex() != UINT_MAX)
                {
                    UpdatePrimitive(primitive);

                    data_to_update.push_back(primitive->GetMaterialRenderProxy()->GetRenderIndex());
                    id_to_update.push_back(to);
                }
            }
            break;
            case SceneRenderProxy::PrimitiveChangeType::Remove:
            case SceneRenderProxy::PrimitiveChangeType::Update:
                break;
            default:
                UnImplemented(type);
                break;
            }
        }

        // we don't need to handle removed primitives, since their places will be taken up by swapped primitive
        // which will then appear as updated primitives and overwrite the removed primitives.
        // it is possible to leak exactly one resource at the back, but it seems harmless for now.

        const auto &primitives = scene_proxy_->GetPrimitives();
        if (ResizeBufferIfNeeded(rhi, material_id_buffer_, sizeof(uint32_t), primitives.size()))
        {
            // full update
            std::vector<uint32_t> material_ids;
            material_ids.reserve(primitives.size());

            for (auto *primitive : primitives)
            {
                auto material_id = primitive->GetMaterialRenderProxy()->GetRenderIndex();
                material_ids.emplace_back(material_id);
            }

            // TODO(tqjxlm): avoid blocking
            rhi->WaitForDeviceIdle();
            material_id_buffer_->UploadImmediate(material_ids.data());

            is_buffer_dirty_ = true;
        }
        else if (!data_to_update.empty())
        {
            // partial update
            material_id_buffer_->PartialUpdate(rhi, data_to_update, id_to_update);
        }
    }

    {
        auto dummy_texture_2d = rhi->GetOrCreateDummyTexture(RHIImage::Attribute{
            .format = PixelFormat::R8G8B8A8_SRGB,
            .sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                        .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                        .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest},
            .usages = RHIImage::ImageUsage::Texture,
        });

        // update textures
        for (const auto *texture : new_textures_)
        {
            auto id = texture->GetBindlessId();
            auto image = LockRHIResource(registered_textures_[id]);
            texture_array_->SetResourceAt((image ? image : dummy_texture_2d)->GetDefaultView(rhi), id);
        }

        for (auto id : removed_textures_)
        {
            texture_array_->SetResourceAt(dummy_texture_2d->GetDefaultView(rhi), id);
        }

        new_textures_.clear();
        removed_textures_.clear();
    }

    {
        // update materials
        const auto &material_proxies = scene_proxy_->GetMaterialProxies();

        if (ResizeBufferIfNeeded(rhi, material_parameter_buffer_, sizeof(MaterialRenderProxy::MaterialRenderData),
                                 material_proxies.size()))
        {
            // full update
            std::vector<MaterialRenderProxy::MaterialRenderData> material_parameters;
            material_parameters.reserve(material_proxies.size());

            for (const auto &material : material_proxies)
            {
                material_parameters.push_back(material->GetRenderData());
            }

            // TODO(tqjxlm): avoid blocking
            rhi->WaitForDeviceIdle();
            material_parameter_buffer_->UploadImmediate(material_parameters.data());

            is_buffer_dirty_ = true;
        }
        else if (const auto &material_to_update = scene_proxy_->GetNewMaterialProxiesThisFrame();
                 !material_to_update.empty())
        {
            // partial update
            std::vector<MaterialRenderProxy::MaterialRenderData> data_to_update;
            std::vector<uint32_t> id_to_update;

            data_to_update.reserve(material_to_update.size());
            id_to_update.reserve(material_to_update.size());

            for (const auto *material : scene_proxy_->GetNewMaterialProxiesThisFrame())
            {
                data_to_update.push_back(material->GetRenderData());
                id_to_update.push_back(material->GetRenderIndex());
            }

            material_parameter_buffer_->PartialUpdate(rhi, data_to_update, id_to_update);
        }
    }
}
} // namespace sparkle
