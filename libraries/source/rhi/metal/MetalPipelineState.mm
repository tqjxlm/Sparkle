#if FRAMEWORK_APPLE

#include "MetalPipelineState.h"

#include "MetalBuffer.h"
#include "MetalContext.h"
#include "MetalImage.h"
#include "MetalRayTracing.h"
#include "MetalResourceArray.h"

namespace sparkle
{
static MTLCompareFunction GetMetalCompareFunction(RHIPipelineState::DepthTestState test_op)
{
    switch (test_op)
    {
    case RHIPipelineState::DepthTestState::Always:
        return MTLCompareFunctionAlways;
    case RHIPipelineState::DepthTestState::Equal:
        return MTLCompareFunctionEqual;
    case RHIPipelineState::DepthTestState::NotEqual:
        return MTLCompareFunctionNotEqual;
    case RHIPipelineState::DepthTestState::Less:
        return MTLCompareFunctionLess;
    case RHIPipelineState::DepthTestState::LessEqual:
        return MTLCompareFunctionLessEqual;
    case RHIPipelineState::DepthTestState::Greater:
        return MTLCompareFunctionGreater;
    case RHIPipelineState::DepthTestState::GreaterEqual:
        return MTLCompareFunctionGreaterEqual;
    default:
        UnImplemented(test_op);
        return MTLCompareFunctionNever;
    }
}

static MTLCullMode GetMetalCullMode(RHIPipelineState::FaceCullMode mode)
{
    switch (mode)
    {
    case RHIPipelineState::FaceCullMode::Front:
        return MTLCullModeFront;
    case RHIPipelineState::FaceCullMode::Back:
        return MTLCullModeBack;
    case RHIPipelineState::FaceCullMode::None:
        return MTLCullModeNone;
    default:
        UnImplemented(mode);
    }
}

static MTLVertexFormat GetMetalVertexFormat(RHIVertexFormat format)
{
    switch (format)
    {
    case RHIVertexFormat::R32G32B32A32Float:
        return MTLVertexFormatFloat4;
    case RHIVertexFormat::R32G32B32Float:
        return MTLVertexFormatFloat3;
    case RHIVertexFormat::R32G32Float:
        return MTLVertexFormatFloat2;
    case RHIVertexFormat::Count:
        UnImplemented(format);
    }
    return MTLVertexFormatInvalid;
}

static MTLBlendFactor GetMetalBlendFactor(RHIPipelineState::BlendFactor factor)
{
    switch (factor)
    {
    case RHIPipelineState::BlendFactor::Zero:
        return MTLBlendFactorZero;
    case RHIPipelineState::BlendFactor::One:
        return MTLBlendFactorOne;
    case RHIPipelineState::BlendFactor::SrcAlpha:
        return MTLBlendFactorSourceAlpha;
    case RHIPipelineState::BlendFactor::OneMinusSrcAlpha:
        return MTLBlendFactorOneMinusSourceAlpha;
    default:
        UnImplemented(factor);
        break;
    }
}

static MTLBlendOperation GetMetalBlendOp(RHIPipelineState::BlendOp op)
{
    switch (op)
    {
    case RHIPipelineState::BlendOp::Add:
        return MTLBlendOperationAdd;
    case RHIPipelineState::BlendOp::Min:
        return MTLBlendOperationMin;
    case RHIPipelineState::BlendOp::Max:
        return MTLBlendOperationMax;
    default:
        UnImplemented(op);
        break;
    }
}

#if DESCRIPTOR_SET_AS_ARGUMENT_BUFFER
void MetalPipelineState::SetupShaderResources(RHIShaderStage stage)
{
    auto *resource_table = GetResourceTable(stage);
    auto *shader = RHICast<MetalShader>(shaders_[static_cast<int>(stage)]);

    shader->SetupArgumentBuffers(context->GetDevice(), argument_buffers_[static_cast<int>(stage)], resource_table);
}
#else
void MetalPipelineState::SetupShaderResources(MTLAutoreleasedRenderPipelineReflection render_reflection,
                                              MTLAutoreleasedComputePipelineReflection compute_reflection,
                                              RHIShaderStage stage)
{
    // parse reflection
    NSArray<id<MTLBinding>> *bindings = nullptr;
    switch (stage)
    {
    case RHIShaderStage::Vertex:
        bindings = render_reflection.vertexBindings;
        break;
    case RHIShaderStage::Pixel:
        bindings = render_reflection.fragmentBindings;
        break;
    case RHIShaderStage::Compute:
        bindings = compute_reflection.bindings;
        break;
    default:
        UnImplemented(stage);
        break;
    }

    std::unordered_map<std::string, ArgumentReflection> reflection_table;
    for (auto i = 0u; i < [bindings count]; i++)
    {
        id<MTLBinding> argument = [bindings objectAtIndex:i];

        reflection_table.insert_or_assign(
            [argument.name cStringUsingEncoding : NSUTF8StringEncoding],
            ArgumentReflection { .binding_point = argument.index, .type = argument.type });
    }

    // remap all resources into one descriptor set with set id as 0 and binding id as argument index from reflection
    auto *resource_table = GetResourceTable(stage);
    unsigned long max_binding_point = 0;
    for (const auto &[name, resource] : resource_table->GetBindingMap())
    {
        const bool is_bindless = resource->IsBindless();
        const auto &resource_name = std::string(name) + (is_bindless ? "_" : "");

        auto found = reflection_table.find(resource_name);
        if (found == reflection_table.end())
        {
            Log(Warn, "failed to find a variable reflection {}", resource_name);

            resource->UpdateReflectionIndex(0, UINT_MAX);

            continue;
        }

        const auto &argument = found->second;

        resource->UpdateReflectionIndex(0, static_cast<uint32_t>(argument.binding_point));

        max_binding_point = std::max(max_binding_point, argument.binding_point);
    }
}
#endif

void MetalPipelineState::BindResources(id<MTLCommandEncoder> encoder, RHIShaderStage stage)
{
#if DESCRIPTOR_SET_AS_ARGUMENT_BUFFER
    auto render_encoder = (id<MTLRenderCommandEncoder>)encoder;
    auto compute_encoder = (id<MTLComputeCommandEncoder>)encoder;

    const auto &argument_buffers = argument_buffers_[static_cast<int>(stage)];
    for (int set = 0; set < argument_buffers.size(); set++)
    {
        id<MTLBuffer> argument_buffer = argument_buffers[set].buffer;
        switch (stage)
        {
        case RHIShaderStage::Vertex: {
            [render_encoder setVertexBuffer:argument_buffer offset:0 atIndex:set];
            [render_encoder useResources:argument_buffers[set].resources.data()
                                   count:argument_buffers[set].resources.size()
                                   usage:MTLResourceUsageRead
                                  stages:MTLRenderStageVertex];
            break;
        }
        case RHIShaderStage::Pixel: {
            [render_encoder setFragmentBuffer:argument_buffer offset:0 atIndex:set];
            [render_encoder useResources:argument_buffers[set].resources.data()
                                   count:argument_buffers[set].resources.size()
                                   usage:MTLResourceUsageRead
                                  stages:MTLRenderStageFragment];
            break;
        }
        case RHIShaderStage::Compute: {
            [compute_encoder setBuffer:argument_buffer offset:0 atIndex:set];
            [compute_encoder useResources:argument_buffers[set].resources.data()
                                    count:argument_buffers[set].resources.size()
                                    usage:MTLResourceUsageRead];
        }
        default:
            UnImplemented(stage);
            break;
        }
    }
#else
    for (const auto &[name, binding] : GetResourceTable(stage)->GetBindingMap())
    {
        auto *resource = binding->GetResource();

        // binding has been rewritten according to shader reflection
        auto binding_point = binding->GetReflection()->slot;

        if (binding_point < 0)
        {
            Log(Warn, "shader resource not bound {} at binding {}", resource->GetName(), binding_point);
            continue;
        }

        if (binding->IsBindless())
        {
            auto *resource_array = RHICast<MetalResourceArray>(resource);
            resource_array->Bind(encoder, stage, binding_point);
        }
        else
        {
            switch (binding->GetType())
            {
            case RHIShaderResourceReflection::ResourceType::UniformBuffer:
            case RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer:
            case RHIShaderResourceReflection::ResourceType::StorageBuffer: {
                RHICast<MetalBuffer>(resource)->Bind(encoder, stage, binding_point);
                break;
            }
            case RHIShaderResourceReflection::ResourceType::Texture2D:
            case RHIShaderResourceReflection::ResourceType::StorageImage2D: {
                RHICast<MetalImageView>(resource)->Bind(encoder, stage, binding_point);
                break;
            }
            case RHIShaderResourceReflection::ResourceType::Sampler: {
                RHICast<MetalSampler>(resource)->Bind(encoder, stage, binding_point);
                break;
            }
            case RHIShaderResourceReflection::ResourceType::AccelerationStructure: {
                RHICast<MetalTLAS>(resource)->Bind(encoder, stage, binding_point);
                break;
            }
            default:
                UnImplemented(binding->GetType());
            }
        }
    }
#endif
}

void MetalGraphicsPipeline::Bind(id<MTLRenderCommandEncoder> encoder)
{
    [encoder setRenderPipelineState:pipeline_state_];
    [encoder setCullMode:GetMetalCullMode(rasterization_state_.cull_mode)];
    [encoder setDepthStencilState:depth_stencil_state_];

    auto vertex_buffer_index = num_vertex_shader_buffers_;
    for (auto &vertex_buffer : vertex_buffers_)
    {
        auto buffer = (RHICast<MetalBuffer>(vertex_buffer))->GetResource();
        [encoder setVertexBuffer:buffer offset:0 atIndex:vertex_buffer_index++];
    }

    BindResources(encoder, RHIShaderStage::Vertex);
    BindResources(encoder, RHIShaderStage::Pixel);
}

void MetalGraphicsPipeline::CreatePipelineState()
{
    for (auto *binding : resource_table_[static_cast<int>(RHIShaderStage::Vertex)]->GetBindings())
    {
        if (binding->GetType() == RHIShaderResourceReflection::ResourceType::UniformBuffer ||
            binding->GetType() == RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer ||
            binding->GetType() == RHIShaderResourceReflection::ResourceType::StorageBuffer)
        {
            num_vertex_shader_buffers_++;
        }
    }

    auto *vertex_shader = RHICast<MetalShader>(shaders_[static_cast<int>(RHIShaderStage::Vertex)]);
    auto *pixel_shader = RHICast<MetalShader>(shaders_[static_cast<int>(RHIShaderStage::Pixel)]);
    MTLRenderPipelineDescriptor *pipeline_state_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    pipeline_state_descriptor.label = [NSString stringWithUTF8String:GetName().c_str()];
    pipeline_state_descriptor.vertexFunction = vertex_shader->GetFunction();
    pipeline_state_descriptor.fragmentFunction = pixel_shader->GetFunction();

    auto *vs_resource = GetResourceTable(RHIShaderStage::Vertex);
    auto *ps_resource = GetResourceTable(RHIShaderStage::Pixel);

    pipeline_state_descriptor.vertexDescriptor = CreateVertexDescriptor(num_vertex_shader_buffers_);

    auto *render_target = render_pass_->GetRenderTarget();
    auto rt_attribute = render_target->GetAttribute();

    for (auto i = 0u; i < RHIRenderTarget::MaxNumColorImage; ++i)
    {
        if (!render_target->GetColorImage(i))
        {
            continue;
        }

        auto color_attachment = pipeline_state_descriptor.colorAttachments[i];
        color_attachment.pixelFormat = GetMetalPixelFormat(rt_attribute.color_attributes_[i].format);
        if (blend_state_.enabled)
        {
            color_attachment.blendingEnabled = blend_state_.enabled;

            color_attachment.sourceRGBBlendFactor = GetMetalBlendFactor(blend_state_.color_factor_src);
            color_attachment.destinationRGBBlendFactor = GetMetalBlendFactor(blend_state_.color_factor_dst);
            color_attachment.rgbBlendOperation = GetMetalBlendOp(blend_state_.color_op);

            color_attachment.sourceAlphaBlendFactor = GetMetalBlendFactor(blend_state_.alpha_factor_src);
            color_attachment.destinationAlphaBlendFactor = GetMetalBlendFactor(blend_state_.alpha_factor_dst);
            color_attachment.alphaBlendOperation = GetMetalBlendOp(blend_state_.alpha_op);
        }
    }

    if (render_target->GetDepthImage())
    {
        pipeline_state_descriptor.depthAttachmentPixelFormat =
            GetMetalPixelFormat(rt_attribute.depth_attribute_.format);
    }

    SetDebugInfo(pipeline_state_descriptor, GetName());

    MTLAutoreleasedRenderPipelineReflection reflection;

    NSError *error;
    pipeline_state_ = [context->GetDevice() newRenderPipelineStateWithDescriptor:pipeline_state_descriptor
                                                                         options:MTLPipelineOptionBindingInfo
                                                                      reflection:&reflection
                                                                           error:&error];

    ASSERT_F(pipeline_state_, "Failed to create pipeline state {}. Error: {}", GetName(),
             [error.localizedDescription UTF8String]);

#if DESCRIPTOR_SET_AS_ARGUMENT_BUFFER
    // set up shader resource bindings with descriptor info
    SetupShaderResources(RHIShaderStage::Vertex);
    SetupShaderResources(RHIShaderStage::Pixel);
#else
    // setup up shader resource bindings using reflection info
    SetupShaderResources(reflection, nullptr, RHIShaderStage::Vertex);
    SetupShaderResources(reflection, nullptr, RHIShaderStage::Pixel);
#endif

    vs_resource->Initialize();
    ps_resource->Initialize();
}

MTLVertexDescriptor *MetalGraphicsPipeline::CreateVertexDescriptor(uint64_t buffer_index_offset)
{
    MTLVertexDescriptor *desc = [[MTLVertexDescriptor alloc] init];

    const auto &bindings = vertex_input_declaration_.GetBindings();
    for (auto binding_idx = 0u; binding_idx < bindings.size(); binding_idx++)
    {
        const auto &attribute_binding = bindings[binding_idx];
        desc.layouts[buffer_index_offset + binding_idx].stride = attribute_binding.stride;
    }

    const auto &attributes = vertex_input_declaration_.GetAttributes();
    for (auto location = 0u; location < attributes.size(); location++)
    {
        const auto &attribute = attributes[location];
        if (attribute.format == RHIVertexFormat::Count)
        {
            continue;
        }

        desc.attributes[location].bufferIndex = buffer_index_offset + attribute.binding;
        desc.attributes[location].format = GetMetalVertexFormat(attribute.format);
        desc.attributes[location].offset = attribute.offset;
    }

    return desc;
}

void MetalComputePipeline::CreatePipelineState()
{
    auto *compute_shader = RHICast<MetalShader>(shaders_[static_cast<int>(RHIShaderStage::Compute)]);

    MTLComputePipelineDescriptor *pipeline_state_descriptor = [[MTLComputePipelineDescriptor alloc] init];

    pipeline_state_descriptor.label = [NSString stringWithUTF8String:GetName().c_str()];

    pipeline_state_descriptor.computeFunction = compute_shader->GetFunction();

    SetDebugInfo(pipeline_state_descriptor, GetName());

    MTLAutoreleasedComputePipelineReflection reflection;

    NSError *error;
    pipeline_state_ = [context->GetDevice() newComputePipelineStateWithDescriptor:pipeline_state_descriptor
                                                                          options:MTLPipelineOptionBindingInfo
                                                                       reflection:&reflection
                                                                            error:&error];

    ASSERT_F(pipeline_state_, "Failed to create pipeline state {}. Error: {}", GetName(),
             [error.localizedDescription UTF8String]);

#if DESCRIPTOR_SET_AS_ARGUMENT_BUFFER
    // set up shader resource bindings with descriptor info
    SetupShaderResources(RHIShaderStage::Compute);
#else
    // setup up shader resource bindings using reflection info
    SetupShaderResources(nullptr, reflection, RHIShaderStage::Compute);
#endif

    auto *shader_resources = GetResourceTable(RHIShaderStage::Compute);
    shader_resources->Initialize();
}

void MetalGraphicsPipeline::CreateDepthStencilState()
{
    auto ds_descriptor = [[MTLDepthStencilDescriptor alloc] init];

    ds_descriptor.depthWriteEnabled = (BOOL)depth_state_.write_depth;
    ds_descriptor.depthCompareFunction = GetMetalCompareFunction(depth_state_.test_state);

    depth_stencil_state_ = [context->GetDevice() newDepthStencilStateWithDescriptor:ds_descriptor];
}

void MetalPipelineState::LoadShaders()
{
    for (auto &shader : shaders_)
    {
        if (shader)
        {
            shader->Load();
        }
    }
}

void MetalGraphicsPipeline::CompileInternal()
{
    LoadShaders();

    CreatePipelineState();
    CreateDepthStencilState();
}

void MetalComputePipeline::CompileInternal()
{
    LoadShaders();

    CreatePipelineState();
}

void MetalComputePipeline::Bind(id<MTLComputeCommandEncoder> encoder)
{
    [encoder setComputePipelineState:pipeline_state_];

    BindResources(encoder, RHIShaderStage::Compute);
}
} // namespace sparkle

#endif
