#pragma once

#include "rhi/RHIResource.h"

#include "rhi/RHIRenderPass.h"

namespace sparkle
{

class RHIUiHandler : public RHIResource
{
public:
    explicit RHIUiHandler(const std::string &name) : RHIResource(name)
    {
    }

    ~RHIUiHandler() override = 0;

    void Setup(const RHIResourceRef<RHIRenderPass> &render_pass)
    {
        render_pass_ = render_pass;

        Init();
    }

    virtual void BeginFrame() = 0;

    virtual void Render() = 0;

    virtual void Init() = 0;

protected:
    bool is_valid_ = false;

    RHIResourceRef<RHIRenderPass> render_pass_;
};
} // namespace sparkle
