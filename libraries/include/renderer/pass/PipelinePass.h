#pragma once

#include "rhi/RHIShader.h"

namespace sparkle
{
class RHIContext;
struct RenderConfig;
class SceneRenderProxy;

// a pipeline pass manages a series of draw calls with the same type of primitives
// it highly overlaps with the scope of RHIRenderPass, but can be narrower
// i.e. several PipelinePass may exist in one RHIRenderPass
class PipelinePass
{
public:
    virtual void InitRenderResources(const RenderConfig &config) = 0;
    virtual void Render() = 0;
    virtual ~PipelinePass() = default;

    template <class T, typename... Args>
        requires std::derived_from<T, PipelinePass>
    static std::unique_ptr<T> Create(const RenderConfig &config, Args &&...args)
    {
        auto pass = std::make_unique<T>(std::forward<Args>(args)...);
        pass->InitRenderResources(config);
        return std::move(pass);
    }

    virtual void UpdateFrameData([[maybe_unused]] const RenderConfig &config, [[maybe_unused]] SceneRenderProxy *scene)
    {
    }

protected:
    explicit PipelinePass(RHIContext *ctx) : rhi_(ctx)
    {
    }

    RHIContext *rhi_;
    RHIResourceRef<RHIShader> vertex_shader_;
    RHIResourceRef<RHIShader> pixel_shader_;
};
} // namespace sparkle
