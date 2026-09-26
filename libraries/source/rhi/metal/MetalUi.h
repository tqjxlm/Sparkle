#pragma once

#if FRAMEWORK_APPLE

#include "MetalRHIInternal.h"

#include "rhi/RHIUiHandler.h"

namespace sparkle
{
class MetalUiHandler : public RHIUiHandler
{
public:
    explicit MetalUiHandler();

    ~MetalUiHandler() override;

    void Render(RHICommandContext *command_context) override;

    void BeginFrame() override;

    void Init() override;
};
} // namespace sparkle

#endif
