#pragma once

#include "rhi/RHIUiHandler.h"

namespace sparkle
{
class RHIContext;
class RenderGraph;
struct RGTexture;

// draws the ui through ImGui into a color attachment of `screen_format`
class UiPass
{
public:
    UiPass(RHIContext *rhi, PixelFormat screen_format);

    // adds a Raster pass drawing the ui over `screen`
    void AddTo(RenderGraph &graph, RGTexture screen) const;

private:
    RHIResourceRef<RHIUiHandler> ui_handler_;
};
} // namespace sparkle
