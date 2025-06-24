#include "renderer/pass/DepthPass.h"

#include "../shader/MeshPassVertexShader.h"
#include "renderer/proxy/MeshRenderProxy.h"
#include "renderer/proxy/SceneRenderProxy.h"
#include "rhi/RHI.h"

namespace sparkle
{
class DepthOnlyPixelShader : public RHIShaderInfo
{
    REGISTGER_SHADER(DepthOnlyPixelShader, RHIShaderStage::Pixel, "shaders/standard/depth_only.ps.hlsl", "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    // no resource

    END_SHADER_RESOURCE_TABLE
};

DepthPass::DepthPass(RHIContext *ctx, SceneRenderProxy *scene_proxy, unsigned width, unsigned height)
    : MeshPass(ctx, scene_proxy), width_(width), height_(height)
{
}

void DepthPass::InitRenderResources(const RenderConfig &)
{
    {
        RHIImage::Attribute attribute;
        attribute.width = width_;
        attribute.height = height_;
        attribute.format = PixelFormat::D32;
        attribute.usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::DepthStencilAttachment;
        attribute.sampler = {.address_mode = RHISampler::SamplerAddressMode::ClampToBorder,
                             .border_color = RHISampler::BorderColor::FloatOpaqueWhite,
                             .filtering_method_min = RHISampler::FilteringMethod::Nearest,
                             .filtering_method_mag = RHISampler::FilteringMethod::Nearest,
                             .filtering_method_mipmap = RHISampler::FilteringMethod::Nearest};

        depth_texture_ = rhi_->CreateImage(attribute, "DepthPassDepthBuffer");
    }

    depth_target_ = rhi_->CreateRenderTarget({}, nullptr, depth_texture_, "DepthRT");

    RHIRenderPass::Attribute pass_attribute;
    pass_attribute.color_load_op = RHIRenderPass::LoadOp::None;
    pass_attribute.color_store_op = RHIRenderPass::StoreOp::None;
    pass_attribute.depth_load_op = RHIRenderPass::LoadOp::Clear;
    pass_attribute.depth_store_op = RHIRenderPass::StoreOp::Store;

    pass_ = rhi_->CreateRenderPass(pass_attribute, depth_target_, "DepthPass");

    vertex_shader_ = rhi_->CreateShader<DepthOnlyVertexShader>();
    pixel_shader_ = rhi_->CreateShader<DepthOnlyPixelShader>();

    view_buffer_ = rhi_->CreateBuffer({.size = sizeof(ViewUBO),
                                       .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                       .mem_properties = RHIMemoryProperty::None,
                                       .is_dynamic = true},
                                      "ForwardRendererViewUniformBuffer");
}

void DepthPass::HandleNewPrimitive(uint32_t primitive_id)
{
    auto *primitive = scene_proxy_->GetPrimitives()[primitive_id];

    auto *mesh_proxy = primitive->As<MeshRenderProxy>();
    const RHIResourceRef<RHIPipelineState> pso =
        rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Graphics, "DepthDrawPipelineState");

    pso->SetRenderPass(pass_);

    pso->SetShader<RHIShaderStage::Vertex>(vertex_shader_);

    pso->SetShader<RHIShaderStage::Pixel>(pixel_shader_);

    pso->SetVertexBuffer(0, mesh_proxy->GetVertexBuffer());
    pso->SetIndexBuffer(mesh_proxy->GetIndexBuffer());

    auto &vertex_decl = pso->GetVertexInputDeclaration();
    vertex_decl.SetAttribute(0, 0, {RHIVertexFormat::R32G32B32Float, 0});

    pso->Compile();

    // bind resources for this pso
    auto *vs_resources = pso->GetShaderResource<DepthOnlyVertexShader>();
    vs_resources->view().BindResource(view_buffer_);
    vs_resources->mesh().BindResource(mesh_proxy->GetUniformBuffer());

    pipeline_states_.resize(std::max(pipeline_states_.size(), static_cast<size_t>(primitive_id + 1)));
    pipeline_states_[primitive_id] = pso;
}

void DepthPass::SetProjectionMatrix(const Mat4 &matrix)
{
    ViewUBO view_ubo{.view_projection_matrix = matrix};
    view_buffer_->Upload(rhi_, &view_ubo);
}

void DepthPass::Render()
{
    rhi_->BeginRenderPass(pass_);

    MeshPass::Render();

    rhi_->EndRenderPass();
}
} // namespace sparkle
