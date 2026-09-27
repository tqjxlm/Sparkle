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

    // prepares drawing into attachments of `signature`
    void Setup(const RHIAttachmentSignature &signature)
    {
        signature_ = signature;

        Init();
    }

    // starts a frame drawn into the open rendering of `info`, whose extent is the display size
    virtual void BeginFrame(const RHIRenderingInfo &info) = 0;

    virtual void Render(RHICommandContext *command_context) = 0;

    virtual void Init() = 0;

protected:
    bool is_valid_ = false;

    RHIAttachmentSignature signature_;
};
} // namespace sparkle
