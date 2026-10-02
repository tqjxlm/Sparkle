#pragma once

#include "rhi/RHIUiHandler.h"

namespace sparkle
{
class RHIContext;
class RenderGraph;
struct RGTexture;

// draws the ui through ImGui
class UiPass
{
public:
    explicit UiPass(RHIContext *rhi);

    // adds a Raster pass drawing the ui over `screen`
    void AddTo(RenderGraph &graph, RGTexture screen) const;

private:
    RHIResourceRef<RHIUiHandler> ui_handler_;
};
} // namespace sparkle
