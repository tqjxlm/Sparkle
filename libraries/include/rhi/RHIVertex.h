#pragma once

#include "core/Exception.h"
#include "core/math/Types.h"

namespace sparkle
{
enum class RHIVertexFormat : uint8_t
{
    R32G32B32A32Float,
    R32G32B32Float,
    R32G32Float,
    Count
};

struct VertexInputAttribute
{
    VertexInputAttribute() = default;

    VertexInputAttribute(RHIVertexFormat in_format, uint32_t in_offset, uint32_t in_padded_size)
        : format(in_format), offset(in_offset), padded_size(in_padded_size)
    {
    }

    VertexInputAttribute(RHIVertexFormat in_format, uint32_t in_offset)
        : VertexInputAttribute(in_format, in_offset, GetAttributeSize(in_format))
    {
    }

    static uint32_t GetAttributeSize(RHIVertexFormat format)
    {
        switch (format)
        {
        case RHIVertexFormat::R32G32B32A32Float:
            return sizeof(Vector4);
        case RHIVertexFormat::R32G32B32Float:
            return sizeof(Vector3);
        case RHIVertexFormat::R32G32Float:
            return sizeof(Vector2);
        default:
            UnImplemented(format);
            return 0;
        }
    }

    RHIVertexFormat format = RHIVertexFormat::Count;
    uint32_t offset;
    uint32_t padded_size;
    uint32_t binding = UINT_MAX;
};

struct VertexAttributeBinding
{
    uint32_t stride = 0;
};

class RHIVertexInputDeclaration
{
public:
    void SetAttribute(unsigned location, unsigned binding, const VertexInputAttribute &attribute)
    {
        if (attribute_bindings_.size() < binding + 1)
        {
            attribute_bindings_.resize(binding + 1);
        }

        if (attributes_.size() < location + 1)
        {
            attributes_.resize(location + 1);
        }

        ASSERT_EQUAL(attributes_[location].format, RHIVertexFormat::Count);

        attributes_[location] = attribute;
        attributes_[location].binding = binding;

        attribute_bindings_[binding].stride += attribute.padded_size;
    }

    void Reset()
    {
        attributes_.clear();
        attribute_bindings_.clear();
    }

    [[nodiscard]] const std::vector<VertexAttributeBinding> &GetBindings() const
    {
        return attribute_bindings_;
    }

    [[nodiscard]] const std::vector<VertexInputAttribute> &GetAttributes() const
    {
        return attributes_;
    }

private:
    std::vector<VertexAttributeBinding> attribute_bindings_;
    std::vector<VertexInputAttribute> attributes_;
};
} // namespace sparkle
