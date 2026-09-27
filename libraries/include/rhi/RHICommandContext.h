#pragma once

#include "core/math/Types.h"
#include "rhi/RHIBarrier.h"
#include "rhi/RHIComputePass.h"
#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIRenderPass.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sparkle
{
class RHIContext;
class RHIBuffer;

// records commands into one command buffer at a time: it owns the open pass and the backend recording state
class RHICommandContext
{
public:
    explicit RHICommandContext(RHIContext *rhi) : rhi_(rhi)
    {
    }

    virtual ~RHICommandContext() = default;

    RHICommandContext(const RHICommandContext &) = delete;
    RHICommandContext &operator=(const RHICommandContext &) = delete;

    // renders the pass's rendering info: its attachments are transitioned from their tracked state into the attachment
    // layouts before the rendering, and into the pass's final layouts after it
    void BeginRenderPass(const RHIResourceRef<RHIRenderPass> &pass);

    void EndRenderPass();

    // begins rendering into attachments already in their attachment layouts. the only barriers it records are
    // `barriers`, before the rendering; the pass's debug label and the optional `timer` bracket both.
    void BeginRendering(const RHIRenderingInfo &info, const std::string &name, RHITimer *timer = nullptr,
                        std::span<const RHIImageBarrier> barriers = {});

    // records `barriers` after the rendering ends, inside the pass's label and timer
    void EndRendering(std::span<const RHIImageBarrier> barriers = {});

    // the info the open rendering began with
    [[nodiscard]] const RHIRenderingInfo &GetRenderingInfo() const
    {
        return rendering_info_;
    }

    // the signature pipelines drawing in the open rendering compile against
    [[nodiscard]] const RHIAttachmentSignature &GetAttachmentSignature() const
    {
        return attachment_signature_;
    }

    void BeginComputePass(const RHIResourceRef<RHIComputePass> &pass);

    void EndComputePass(const RHIResourceRef<RHIComputePass> &pass);

    [[nodiscard]] const RHIResourceRef<RHIComputePass> &GetCurrentComputePass() const
    {
        return current_compute_pass_;
    }

    // records one batch of barriers outside any render pass. Metal tracks hazards itself and records nothing.
    void Barrier(std::span<const RHIImageBarrier> image_barriers, std::span<const RHIMemoryBarrier> memory_barriers);

    // resources every pipeline drawn or dispatched through the context binds, into each of its resource tables that
    // has the binding's member, until the bindings are set again. the render graph sets the bindings a pass declared
    // while it records the pass; the span must outlive that time.
    void SetBindings(std::span<const RHIMemberBinding> bindings)
    {
        bindings_ = bindings;
        bindings_applied_.assign(bindings.size(), false);
        drew_or_dispatched_ = false;
    }

    // whether the binding at `index` of the set bindings bound into a pipeline since they were set
    [[nodiscard]] bool IsBindingApplied(size_t index) const
    {
        return bindings_applied_[index];
    }

    // whether a pipeline drew or dispatched since the bindings were set
    [[nodiscard]] bool DrewOrDispatched() const
    {
        return drew_or_dispatched_;
    }

    // a null pipeline draws nothing
    void DrawMesh(const RHIResourceRef<RHIPipelineState> &pipeline_state, const DrawArgs &draw_args);

    void DispatchCompute(const RHIResourceRef<RHIPipelineState> &pipeline, Vector3UInt total_threads,
                         Vector3UInt thread_per_group);

    // transfers are recorded outside any pass: Vulkan forbids them inside a render pass, and Metal cannot open a
    // blit encoder while a render or compute encoder is open
    void CopyBuffer(const RHIBuffer *src, const RHIBuffer *dst);

    void CopyBufferToImage(const RHIBuffer *src, const RHIImage *dst);

    void CopyImageToBuffer(const RHIImage *src, const RHIBuffer *dst);

    void BlitImage(const RHIImage *src, const RHIImage *dst, RHISampler::FilteringMethod filter);

    void AssertOutsidePass(std::string_view command) const;

protected:
    virtual void DrawMeshInternal(const RHIResourceRef<RHIPipelineState> &pipeline_state,
                                  const DrawArgs &draw_args) = 0;
    virtual void DispatchComputeInternal(const RHIResourceRef<RHIPipelineState> &pipeline, Vector3UInt total_threads,
                                         Vector3UInt thread_per_group) = 0;
    virtual void BarrierInternal(std::span<const RHIImageBarrier> image_barriers,
                                 std::span<const RHIMemoryBarrier> memory_barriers) = 0;
    virtual void CopyBufferInternal(const RHIBuffer *src, const RHIBuffer *dst) = 0;
    virtual void CopyBufferToImageInternal(const RHIBuffer *src, const RHIImage *dst) = 0;
    virtual void CopyImageToBufferInternal(const RHIImage *src, const RHIBuffer *dst) = 0;
    virtual void BlitImageInternal(const RHIImage *src, const RHIImage *dst, RHISampler::FilteringMethod filter) = 0;
    // groups the commands of a pass in captures and validation messages, where the backend labels commands
    virtual void BeginDebugLabel(const std::string &name) const = 0;
    virtual void EndDebugLabel() const = 0;
    virtual void BeginRenderingInternal(const RHIRenderingInfo &info, const std::string &name, RHITimer *timer) = 0;
    virtual void EndRenderingInternal() = 0;
    virtual void BeginComputePassInternal(const RHIResourceRef<RHIComputePass> &pass) = 0;
    virtual void EndComputePassInternal(const RHIResourceRef<RHIComputePass> &pass) = 0;

private:
    void ApplyBindings(RHIPipelineState &pipeline);

    RHIContext *rhi_;
    std::span<const RHIMemberBinding> bindings_;
    std::vector<bool> bindings_applied_;
    bool drew_or_dispatched_ = false;
    // the pass that began the open rendering through BeginRenderPass
    RHIResourceRef<RHIRenderPass> current_render_pass_;
    RHIResourceRef<RHIComputePass> current_compute_pass_;
    bool rendering_ = false;
    std::string rendering_name_;
    RHIRenderingInfo rendering_info_;
    RHIAttachmentSignature attachment_signature_;
    RHITimer *rendering_timer_ = nullptr;
};
} // namespace sparkle
