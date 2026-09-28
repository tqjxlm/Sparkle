#include "renderer/graph/RGPassTimers.h"

#include "rhi/RHI.h"

namespace sparkle
{
RHIPass *RGPassTimers::Get(const std::string &name)
{
    auto &pass = passes_[name];
    if (!pass)
    {
        pass = rhi_->CreateRenderPass(name, true);
    }
    return pass.get();
}
} // namespace sparkle
