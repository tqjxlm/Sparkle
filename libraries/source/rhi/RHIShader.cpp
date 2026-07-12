#include "rhi/RHIShader.h"

#include "core/Hash.h"
#include "rhi/RHI.h"

namespace sparkle
{

void RHIShaderResourceTable::RegisterShaderResourceReflection(RHIShaderResourceBinding *binding,
                                                              RHIShaderResourceReflection *decl)
{
    binding_map_.insert_or_assign(decl->name, binding);
    bindings_.push_back(binding);
}

void RHIShaderResourceTable::Initialize()
{
    // should not happen twice
    ASSERT(!initialized_);

    for (auto *resource : bindings_)
    {
        const auto *decl = resource->GetReflection();

        ASSERT(decl->set != UINT_MAX);

        auto set = decl->set;
        auto slot = decl->slot;
        if (slot == UINT_MAX)
        {
            // reflection could not locate this resource in the compiled shader (e.g. optimized out)
            continue;
        }
        if (resource_sets_.size() < set + 1)
        {
            resource_sets_.resize(set + 1);
        }

        auto &resource_set = resource_sets_[set];
        resource_set.SetBinding(slot, resource);
    }

    initialized_ = true;
}

void RHIShaderResourceSet::UpdateResourceHash() const
{
    resource_hash_ = 0;

    // NOLINTNEXTLINE(modernize-loop-convert): slot is used by the assert, which is compiled out in Release
    for (auto slot = 0u; slot < bindings_.size(); slot++)
    {
        const auto *binding = bindings_[slot];
        ASSERT_F(binding, "hole at binding slot {}: shader bindings must be dense per set", slot);
        HashCombine(resource_hash_, binding->GetResource()->GetId());
    }

    resource_dirty_ = false;
}

uint32_t RHIShaderResourceReflection::GetHash() const
{
    CRC32 hasher;
    HashCombine(hasher, set);
    HashCombine(hasher, slot);
    HashCombine(hasher, type);
    HashCombine(hasher, is_bindless);

    uint32_t hash = 0;
    hasher.getHash(reinterpret_cast<unsigned char *>(&hash));

    return hash;
}

void RHIShaderResourceSet::UpdateLayoutHash()
{
    CRC32 hasher;

    ASSERT(!bindings_.empty());

    // NOLINTNEXTLINE(modernize-loop-convert): slot is used by the assert, which is compiled out in Release
    for (auto slot = 0u; slot < bindings_.size(); slot++)
    {
        auto *binding = bindings_[slot];
        ASSERT_F(binding, "hole at binding slot {}: shader bindings must be dense per set", slot);
        HashCombine(hasher, binding->GetReflection()->GetHash());
    }

    hasher.getHash(reinterpret_cast<unsigned char *>(&layout_hash_));
}

RHIShaderResourceBinding::RHIShaderResourceBinding(RHIShaderResourceTable *resource_table, std::string_view name,
                                                   RHIShaderResourceReflection::ResourceType type, bool is_bindless)
{
    Register(std::make_unique<RHIShaderResourceReflection>(name, type, is_bindless));

    resource_table->RegisterShaderResourceReflection(this, decl_.get());
}

void RHIShaderResourceBinding::BindResource(RHIResource *resource, bool rebind)
{
#ifndef NDEBUG
    ASSERT_F(!RHIContext::deleted_resources_.contains(resource->GetId()), "resource already deleted {}. name {}.",
             resource->GetId(), resource->GetName());
#endif

    if (resource_ != resource || rebind)
    {
        ASSERT_F(parent_set_, "Do not bind resources before PSO compilation.");
        parent_set_->MarkResourceDirty();
    }

    resource_ = resource;
}
} // namespace sparkle
