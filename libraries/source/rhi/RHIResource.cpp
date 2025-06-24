#include "rhi/RHIResource.h"

#ifndef NDEBUG
#include "core/Logger.h"
#endif

#include <thread>

namespace sparkle
{
void RHIResource::UpdateId() const
{
    static thread_local size_t hash_seed =
        static_cast<size_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));

    id_ = std::hash<size_t>{}(reinterpret_cast<size_t>(this)) ^ hash_seed;
    hash_seed += 1;

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
