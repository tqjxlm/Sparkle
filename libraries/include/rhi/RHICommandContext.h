#pragma once

#include "core/math/Types.h"
#include "rhi/RHIBarrier.h"
#include "rhi/RHIComputePass.h"
#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIPass.h"
#include "rhi/RHIRenderingInfo.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sparkle
{
class RHIBuffer;

// records commands into one command buffer at a time: it owns the open pass and the backend recording state
class RHICommandContext
{
public:
    // labels the commands recorded while it lives, where the backend labels commands
    class DebugLabelScope
    {
    public:
        DebugLabelScope(const RHICommandContext &command_context, const std::string &name)
            : command_context_(command_context)
        {
            command_context_.BeginDebugLabel(name);
        }

        ~DebugLabelScope()
        {
            command_context_.EndDebugLabel();
        }

        DebugLabelScope(const DebugLabelScope &) = delete;
        DebugLabelScope &operator=(const DebugLabelScope &) = delete;

    private:
        const RHICommandContext &command_context_;
    };

    RHICommandContext() = default;

    virtual ~RHICommandContext() = default;

    RHICommandContext(const RHICommandContext &) = delete;
    RHICommandContext &operator=(const RHICommandContext &) = delete;

    // begins rendering into attachments whose tracked layouts are already their attachment layouts. the only barriers
    // it records are `barriers` and `memory_barriers`, before the rendering; the debug label `name` and, when given,
    // the timer of `timed_pass` bracket both.
    void BeginRendering(const RHIRenderingInfo &info, const std::string &name, RHIPass *timed_pass = nullptr,
                        std::span<const RHIImageBarrier> barriers = {},
                        std::span<const RHIMemoryBarrier> memory_barriers = {});

    void EndRendering();

    [[nodiscard]] bool IsRendering() const
    {
        return rendering_;
    }

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

    // begins a compute pass. the only barriers it records are `barriers` and `memory_barriers`, before the pass; the
    // pass's debug label and timer bracket both.
    void BeginComputePass(const RHIResourceRef<RHIComputePass> &pass, std::span<const RHIImageBarrier> barriers = {},
                          std::span<const RHIMemoryBarrier> memory_barriers = {});

    void EndComputePass(const RHIResourceRef<RHIComputePass> &pass);

    [[nodiscard]] const RHIResourceRef<RHIComputePass> &GetCurrentComputePass() const
    {
        return current_compute_pass_;
    }

    // records one batch of barriers outside any render pass. Metal tracks hazards itself and records nothing.
    void Barrier(std::span<const RHIImageBarrier> image_barriers, std::span<const RHIMemoryBarrier> memory_barriers);

    void Barrier(const RHIMemoryBarrier &memory_barrier)
    {
        Barrier({}, std::span(&memory_barrier, 1));
    }

    // resources every pipeline drawn or dispatched through the context binds, into each of its resource tables that
    // has the binding's member, until the bindings are set again. the render graph sets the bindings a pass declared
    // while it records the pass; the span must outlive that time.
    void SetBindings(std::span<const RHIMemberBinding> bindings)
    {
        bindings_ = bindings;
        bindings_applied_.assign(bindings.size(), false);
        pipelines_.clear();
    }

    // whether the binding at `index` of the set bindings bound into a pipeline since they were set
    [[nodiscard]] bool IsBindingApplied(size_t index) const
    {
        return bindings_applied_[index];
    }

    // the pipelines drawn or dispatched since the bindings were set, once per run of consecutive draws or dispatches
    [[nodiscard]] const std::vector<const RHIPipelineState *> &GetPipelines() const
    {
        return pipelines_;
    }

    // draws inside the open rendering. a null pipeline draws nothing.
    void DrawMesh(const RHIResourceRef<RHIPipelineState> &pipeline_state, const DrawArgs &draw_args);

    // dispatches inside the open compute pass
    void DispatchCompute(const RHIResourceRef<RHIPipelineState> &pipeline, Vector3UInt total_threads,
                         Vector3UInt thread_per_group);

    // transfers are recorded outside any pass: Vulkan forbids them inside a render pass, and Metal cannot open a
    // blit encoder while a render or compute encoder is open. they record no barrier: the caller orders them against
    // other work, including a host read of the data they write (the graph through the accesses a pass declares and the
    // buffers the host reads). CopyBufferToImage and CopyImageToBuffer need the image in the TransferDst and
    // TransferSrc layouts; BlitImage reads and writes the images in their tracked layouts.
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

    std::span<const RHIMemberBinding> bindings_;
    std::vector<bool> bindings_applied_;
    std::vector<const RHIPipelineState *> pipelines_;
    RHIResourceRef<RHIComputePass> current_compute_pass_;
    bool rendering_ = false;
    std::string rendering_name_;
    RHIRenderingInfo rendering_info_;
    RHIAttachmentSignature attachment_signature_;
    RHITimer *rendering_timer_ = nullptr;
};
} // namespace sparkle
