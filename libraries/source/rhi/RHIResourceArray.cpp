#include "rhi/RHIResourceArray.h"

namespace sparkle
{
void RHIResourceArray::OnResourceUpdate()
{
}

void RHIResourceArray::SetResourceAt(const RHIResourceRef<RHIResource> &resource, unsigned index)
{
    ASSERT(index < resources_.size());

    if (resources_[index] == resource)
    {
        return; // no change
    }

    resources_[index] = resource;
    dirty_resource_indices_.push_back(index);
    OnResourceUpdate();
}
} // namespace sparkle
