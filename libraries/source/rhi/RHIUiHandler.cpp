#include "rhi/RHIUiHandler.h"

#include <imgui_threaded_rendering.h>

#include <mutex>

namespace sparkle
{
namespace
{
// one ImGui context per process, so one queue; the helper needs every call into it to hold one mutex
ImTextureQueue texture_queue;
std::mutex texture_queue_mutex;
} // namespace

RHIUiHandler::RHIUiHandler(const std::string &name, void (*update_texture)(ImTextureData *texture),
                           unsigned in_flight_frames)
    : RHIResource(name)
{
    std::scoped_lock<std::mutex> lock(texture_queue_mutex);
    texture_queue.UpdateTexFunc = update_texture;
    texture_queue.InFlightFrames = static_cast<int>(in_flight_frames);
}

RHIUiHandler::~RHIUiHandler() = default;

void RHIUiHandler::BeginImGuiFrame()
{
    std::scoped_lock<std::mutex> lock(texture_queue_mutex);
    texture_queue.PreNewFrame();
}

void RHIUiHandler::QueueTextureRequests(ImDrawData &draw_data)
{
    std::scoped_lock<std::mutex> lock(texture_queue_mutex);
    texture_queue.QueueRequests(&draw_data);
}

void RHIUiHandler::ProcessTextureRequests(ImDrawData &draw_data)
{
    std::scoped_lock<std::mutex> lock(texture_queue_mutex);
    texture_queue.ProcessRequests(&draw_data);
}

void RHIUiHandler::ShutdownTextureQueue()
{
    std::scoped_lock<std::mutex> lock(texture_queue_mutex);
    texture_queue.Shutdown();
}
} // namespace sparkle
