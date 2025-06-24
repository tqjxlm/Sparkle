#pragma once

#include "rhi/RHIResource.h"

#include "rhi/RHIShader.h"

namespace sparkle
{
class RHIResourceArray : public RHIResource
{
public:
    explicit RHIResourceArray(RHIShaderResourceReflection::ResourceType type, unsigned capacity,
                              const std::string &name)
        : RHIResource(name), type_(type)
    {
        resources_.resize(capacity);
    }

    [[nodiscard]] unsigned GetArraySize() const
    {
        return static_cast<unsigned>(resources_.size());
    }

    void SetResourceAt(const RHIResourceRef<RHIResource> &resource, unsigned index);

    void FinishUpdate()
    {
        dirty_resource_indices_.clear();
    }

    [[nodiscard]] RHIResource *GetResourceAt(unsigned index) const
    {
        ASSERT(index < resources_.size());
        return resources_[index].get();
    }

    [[nodiscard]] bool IsBindless() const override
    {
        return resources_.size() == RHIShaderResourceBinding::MaxBindlessResources;
    }

    [[nodiscard]] auto GetUnderlyingType() const
    {
        return type_;
    }

protected:
    virtual void OnResourceUpdate();

    std::vector<RHIResourceRef<RHIResource>> resources_;
    RHIShaderResourceReflection::ResourceType type_;
    std::vector<unsigned> dirty_resource_indices_;
};
} // namespace sparkle
