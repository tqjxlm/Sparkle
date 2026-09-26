#pragma once

#include "rhi/RHIResource.h"

namespace sparkle
{
class RHICommandContext;

class RHITimer : public RHIResource
{
public:
    enum class Status : uint8_t
    {
        Inactive,         // before the first query
        Measuring,        // after Begin()
        WaitingForResult, // after End()
        Ready,            // after result is available
    };

    using RHIResource::RHIResource;

    virtual void Begin(RHICommandContext &command_context) = 0;

    virtual void End(RHICommandContext &command_context) = 0;

    virtual void TryGetResult() = 0;

    Status GetStatus()
    {
        if (status_ == Status::WaitingForResult)
        {
            TryGetResult();
        }

        return status_;
    }

    [[nodiscard]] float GetTime() const
    {
        ASSERT_EQUAL(status_, Status::Ready);
        return cached_time_ms_;
    }

protected:
    Status status_ = Status::Inactive;

    float cached_time_ms_ = 0.0f;
};
} // namespace sparkle
