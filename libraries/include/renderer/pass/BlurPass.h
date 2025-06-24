#pragma once

#include "renderer/pass/ScreenQuadPass.h"

namespace sparkle
{
class BlurPass : public ScreenQuadPass
{
protected:
    enum PingPongIndex : uint8_t
    {
        // ping-pong images are internal. in every blur pass we read from one and write to another and then swap
        PING = 0,
        PONG,
        // input image is external, it will only be read once
        INPUT,
        COUNT
    };

public:
    BlurPass(RHIContext *ctx, const RHIResourceRef<RHIImage> &input, int count);

    void Render() override;

    RHIResourceRef<RHIImage> GetOutput()
    {
        return images_[(count_ % 2 == 0) ? PingPongIndex::PING : PingPongIndex::PONG];
    }

protected:
    void SetupRenderPass() override;

    void SetupPixelShader() override;

    void SetupPipeline() override;

    void CompilePipeline() override;

    void SetupVertices() override;

    void SetupVertexShader() override;

    void BindVertexShaderResources() override;

    void BindPixelShaderResources() override;

    void InitImageAndTarget(PingPongIndex index);

    void RenderPass(PingPongIndex index);

    static PingPongIndex GetOutputIndex(PingPongIndex input_index)
    {
        switch (input_index)
        {
        case PONG:
            return PING;
        case PING:
        case INPUT:
            return PONG;
        default:
            UnImplemented(input_index);
            return COUNT;
        }
    }

private:
    std::array<RHIResourceRef<RHIImage>, PingPongIndex::COUNT> images_;

    std::array<RHIResourceRef<RHIRenderTarget>, PingPongIndex::COUNT> targets_;

    std::array<RHIResourceRef<RHIRenderPass>, PingPongIndex::COUNT> passes_;

    std::array<RHIResourceRef<RHIPipelineState>, PingPongIndex::COUNT> pipeline_states_;

    RHIResourceRef<RHIImage> input_;

    int count_;
};
} // namespace sparkle
