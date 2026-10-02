#pragma once

#include "rhi/RHIResource.h"

#include "rhi/RHIRenderingInfo.h"

struct ImDrawData;
struct ImTextureData;

namespace sparkle
{
class RHICommandContext;

class RHIUiHandler : public RHIResource
{
public:
    ~RHIUiHandler() override = 0;

    // starts a frame drawn into the open rendering of `info`
    virtual void BeginFrame(const RHIRenderingInfo &info) = 0;

    // draws into color slot 0 of the open rendering, leaving its other attachments untouched, with pipelines compiled
    // for its attachment signature
    virtual void Render(RHICommandContext &command_context) = 0;

    // ImGui's textures belong to the main thread, which acknowledges their requests (e.g. new font glyphs) and queues
    // them for the handler: call before ImGui::NewFrame()
    static void BeginImGuiFrame();

    // call after ImGui::Render() with its draw data, before handing a copy of it to the render thread
    static void QueueTextureRequests(ImDrawData &draw_data);

protected:
    RHIUiHandler(const std::string &name, void (*update_texture)(ImTextureData *texture), unsigned in_flight_frames);

    // serves the queued texture requests before `draw_data`, the adopted copy, is drawn
    static void ProcessTextureRequests(ImDrawData &draw_data);

    // destroys the queued textures' backend objects; call before the backend shuts down, once no thread runs ImGui
    static void ShutdownTextureQueue();

    bool is_valid_ = false;
};
} // namespace sparkle
