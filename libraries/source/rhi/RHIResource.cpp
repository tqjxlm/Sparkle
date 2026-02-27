#include "rhi/RHIResource.h"

#ifndef NDEBUG
#include "core/Logger.h"
#endif

#include <atomic>

namespace sparkle
{
void RHIResource::UpdateId() const
{
    static std::atomic<size_t> next_id{1};

    id_ = next_id.fetch_add(1, std::memory_order_relaxed);
    id_dirty_ = false;
}

#ifndef NDEBUG
void RHIResource::PrintTrace(RHIResource *resource, int64_t use_count)
{
    Log(Debug, "Resource {} count {}", resource->GetName(), use_count);
    ExceptionHandler::PrintStackTrace();
}
#endif
} // namespace sparkle
