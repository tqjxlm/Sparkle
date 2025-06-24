#pragma once

#include "rhi/RHIBuffer.h"
#include "rhi/RHIResourceArray.h"

#include <unordered_set>

namespace sparkle
{
class SceneRenderProxy;

enum class BindlessResourceType : uint8_t
{
    Texture,
    IndexBuffer,
    VertexBuffer,
    VertexAttributeBuffer,
};

class BindlessManager
{
public:
    explicit BindlessManager(SceneRenderProxy *scene_proxy) : scene_proxy_(scene_proxy)
    {
    }

    void RegisterTexture(const RHIResourceRef<RHIImage> &rhi_image);

    void UnregisterTexture(RHIImage *rhi_image);

    [[nodiscard]] const RHIResourceRef<RHIResourceArray> &GetBindlessBuffer(BindlessResourceType type) const
    {
        switch (type)
        {
        case BindlessResourceType::Texture:
            return texture_array_;
        case BindlessResourceType::IndexBuffer:
            return index_buffer_array_;
        case BindlessResourceType::VertexBuffer:
            return vertex_buffer_array_;
        case BindlessResourceType::VertexAttributeBuffer:
            return vertex_attribute_buffer_array_;
        }
    }

    [[nodiscard]] const RHIResourceRef<RHIBuffer> &GetMaterialParameterBuffer() const
    {
        return material_parameter_buffer_;
    }

    [[nodiscard]] const RHIResourceRef<RHIBuffer> &GetMaterialIdBuffer() const
    {
        return material_id_buffer_;
    }

    // it means anyone who uses buffers from this class should re-bind buffers
    [[nodiscard]] bool IsBufferDirty() const
    {
        return is_buffer_dirty_;
    }

    [[nodiscard]] bool IsValid() const
    {
        return is_valid_;
    }

    void UpdateFrameData(RHIContext *rhi);

    void InitRenderResources(RHIContext *rhi);

protected:
    void UpdatePrimitive(uint32_t primitive_id);

private:
    RHIResourceRef<RHIResourceArray> texture_array_;
    RHIResourceRef<RHIResourceArray> index_buffer_array_;
    RHIResourceRef<RHIResourceArray> vertex_buffer_array_;
    RHIResourceRef<RHIResourceArray> vertex_attribute_buffer_array_;

    RHIResourceRef<RHIBuffer> material_parameter_buffer_;
    RHIResourceRef<RHIBuffer> material_id_buffer_;

    std::vector<RHIResourceWeakRef<RHIImage>> registered_textures_;
    std::unordered_set<uint32_t> free_texture_ids_;
    std::unordered_map<const RHIImage *, unsigned> texture_ref_count_;
    std::vector<const RHIImage *> new_textures_;
    std::vector<uint32_t> removed_textures_;

    SceneRenderProxy *scene_proxy_;

    bool is_buffer_dirty_ = false;

    bool is_valid_ = false;
};
} // namespace sparkle
