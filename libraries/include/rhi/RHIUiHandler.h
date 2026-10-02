#pragma once

#include "rhi/RHIResource.h"

#include "rhi/RHIRenderingInfo.h"

namespace sparkle
{
class RHICommandContext;

class RHIUiHandler : public RHIResource
{
public:
    explicit RHIUiHandler(const std::string &name) : RHIResource(name)
    {
    }

    ~RHIUiHandler() override = 0;

    // starts a frame drawn into the open rendering of `info`, whose extent is the display size
    virtual void BeginFrame(const RHIRenderingInfo &info) = 0;

    // draws into color slot 0 of the open rendering, leaving its other attachments untouched, with pipelines compiled
    // for its attachment signature
    virtual void Render(RHICommandContext &command_context) = 0;

protected:
    bool is_valid_ = false;
};
} // namespace sparkle
