#pragma once

#include "core/Enum.h"
#include "core/Logger.h"
#include "core/math/Types.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace sparkle
{
// The reconstruction providers (denoisers or upscalers) a renderer has created, one per provider. A provider is created
// on first use; one that fails to create, or stops being ready, is skipped until the next Resize retries it.
template <class Provider, class Instance> class ProviderCache
{
public:
    // `kind` names the providers in logs
    explicit ProviderCache(const char *kind) : kind_(kind)
    {
    }

    // `create(provider)` returns the new instance, or null when this build cannot serve the provider
    template <class Create> Instance *Get(Provider provider, Create &&create)
    {
        Slot &slot = FindOrAdd(provider);
        if (slot.failed)
        {
            return nullptr;
        }

        if (slot.instance && !slot.instance->IsReady())
        {
            slot.failed = true;
            Log(Error, "{} failed while encoding; selecting a fallback", slot.instance->GetName());
            return nullptr;
        }

        if (!slot.instance)
        {
            slot.instance = create(provider);
            if (!slot.instance || !slot.instance->IsReady())
            {
                slot.failed = true;
                slot.instance.reset();
                Log(Warn, "{} provider {} is unavailable", kind_, Enum2Str(provider));
                return nullptr;
            }
        }
        return slot.instance.get();
    }

    void Resize(const Vector2UInt &input_size, const Vector2UInt &output_size)
    {
        for (auto &slot : slots_)
        {
            if (slot.instance)
            {
                slot.instance->Resize(input_size, output_size);
            }
            slot.failed = slot.instance && !slot.instance->IsReady();
        }
    }

private:
    struct Slot
    {
        Provider provider;
        std::unique_ptr<Instance> instance;
        bool failed = false;
    };

    Slot &FindOrAdd(Provider provider)
    {
        auto slot = std::ranges::find(slots_, provider, &Slot::provider);
        if (slot != slots_.end())
        {
            return *slot;
        }
        return slots_.emplace_back(Slot{.provider = provider, .instance = nullptr, .failed = false});
    }

    const char *kind_;
    std::vector<Slot> slots_;
};
} // namespace sparkle
