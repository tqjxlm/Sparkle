#pragma once

#include "rhi/RHIResource.h"

#include "rhi/RHIBuffer.h"
#include "rhi/RHIRenderPass.h"
#include "rhi/RHIShader.h"
#include "rhi/RHIVertex.h"

namespace sparkle
{
class RHIPipelineState : public RHIResource
{
public:
    enum class PipelineType : uint8_t
    {
        Graphics,
        Compute
    };

    enum class DepthTestState : uint8_t
    {
        Always,
        Equal,
        NotEqual,
        Less,
        LessEqual,
        Greater,
        GreaterEqual,
    };

    enum class PolygonMode : uint8_t
    {
        Fill,
        Line,
        Point,
    };

    enum class FaceCullMode : uint8_t
    {
        Front,
        Back,
        None,
    };

    enum class BlendFactor : uint8_t
    {
        Zero,
        One,
        SrcAlpha,
        OneMinusSrcAlpha,
    };

    enum class BlendOp : uint8_t
    {
        Add,
        Min,
        Max,
    };

    struct DepthState
    {
        DepthTestState test_state = DepthTestState::Less;
        bool write_depth = true;
    };

    struct RasterizationState
    {
        PolygonMode polygon_mode = PolygonMode::Fill;
        FaceCullMode cull_mode = FaceCullMode::Front;
        float line_width = 1.0f;
    };

    struct BlendState
    {
        bool enabled = false;
        BlendFactor color_factor_src = BlendFactor::SrcAlpha;
        BlendFactor color_factor_dst = BlendFactor::OneMinusSrcAlpha;
        BlendOp color_op = BlendOp::Add;
        BlendFactor alpha_factor_src = BlendFactor::One;
        BlendFactor alpha_factor_dst = BlendFactor::Zero;
        BlendOp alpha_op = BlendOp::Add;
    };

    RHIPipelineState(PipelineType type, const std::string &name) : RHIResource(name), pipeline_type_(type)
    {
    }

    void Compile()
    {
        CompileInternal();
        compiled_ = true;
    }

    virtual void CompileInternal() = 0;

    void SetRenderPass(const RHIResourceRef<RHIRenderPass> &pass)
    {
        render_pass_ = pass;
    }

    void SetVertexBuffer(uint32_t binding, const RHIResourceRef<RHIBuffer> &buffer)
    {
        if (vertex_buffers_.size() < binding + 1)
        {
            vertex_buffers_.resize(binding + 1);
        }
        vertex_buffers_[binding] = buffer;
    }

    void SetIndexBuffer(const RHIResourceRef<RHIBuffer> &buffer)
    {
        index_buffer_ = buffer;
    }

    template <RHIShaderStage Stage> void SetShader(const RHIResourceRef<RHIShader> &shader)
    {
        // the stage parameter is actually redundant here. it is used to ensure the user's intention
        if constexpr (Stage == RHIShaderStage::Vertex || Stage == RHIShaderStage::Pixel)
        {
            ASSERT_EQUAL(pipeline_type_, PipelineType::Graphics);
        }
        else if constexpr (Stage == RHIShaderStage::Compute)
        {
            ASSERT_EQUAL(pipeline_type_, PipelineType::Compute);
        }

        shaders_[static_cast<int>(Stage)] = shader;

        resource_table_[static_cast<int>(Stage)] = shader->GetInfo()->CreateShaderResourceTable();
    }

    void SetDepthState(DepthState depth_state)
    {
        depth_state_ = depth_state;
    }

    void SetRasterizationState(RasterizationState rasterization_state)
    {
        rasterization_state_ = rasterization_state;
    }

    void SetBlendState(BlendState blend_state)
    {
        blend_state_ = blend_state;
    }

    [[nodiscard]] RasterizationState GetRasterizationState() const
    {
        return rasterization_state_;
    }

    RHIVertexInputDeclaration &GetVertexInputDeclaration()
    {
        return vertex_input_declaration_;
    }

    RHIResourceRef<RHIBuffer> GetIndexBuffer()
    {
        return index_buffer_;
    }

    template <class T> typename T::ResourceTable *GetShaderResource()
    {
        ASSERT(compiled_);

        return static_cast<typename T::ResourceTable *>(GetResourceTable(T::GetStage()));
    }

protected:
    [[nodiscard]] const RHIShaderResourceTable *GetResourceTable(RHIShaderStage stage) const
    {
        return resource_table_[static_cast<size_t>(stage)].get();
    }

    RHIShaderResourceTable *GetResourceTable(RHIShaderStage stage)
    {
        return resource_table_[static_cast<size_t>(stage)].get();
    }

    RHIResourceRef<RHIRenderPass> render_pass_;
    RHIVertexInputDeclaration vertex_input_declaration_;
    std::vector<RHIResourceRef<RHIBuffer>> vertex_buffers_;
    RHIResourceRef<RHIBuffer> index_buffer_;
    RHIResourceRef<RHIShader> shaders_[static_cast<int>(RHIShaderStage::Count)];
    std::array<std::unique_ptr<RHIShaderResourceTable>, static_cast<size_t>(RHIShaderStage::Count)> resource_table_;

    DepthState depth_state_;
    RasterizationState rasterization_state_;
    BlendState blend_state_;

    PipelineType pipeline_type_;

    bool compiled_ = false;
};
} // namespace sparkle
