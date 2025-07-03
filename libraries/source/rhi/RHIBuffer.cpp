#include "rhi/RHIBuffer.h"

#include "rhi/RHI.h"

namespace sparkle
{
class BufferUpdateComputeShader : public RHIShaderInfo
{
    REGISTGER_SHADER(BufferUpdateComputeShader, RHIShaderStage::Compute, "shaders/utilities/buffer_update.cs.slang",
                     "main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::UniformBuffer)
    USE_SHADER_RESOURCE(indexBuffer, RHIShaderResourceReflection::ResourceType::StorageBuffer)
    USE_SHADER_RESOURCE(dataBuffer, RHIShaderResourceReflection::ResourceType::StorageBuffer)
    USE_SHADER_RESOURCE(outputBuffer, RHIShaderResourceReflection::ResourceType::StorageBuffer)

    END_SHADER_RESOURCE_TABLE

public:
    struct UniformBufferData
    {
        uint32_t element_size;
        uint32_t element_count;
    };
};

void RHIBuffer::PartialUpdate(RHIContext *rhi, const uint8_t *data, const std::vector<uint32_t> &indices,
                              uint32_t element_count, uint32_t element_size)
{
    auto shader = rhi->CreateShader<BufferUpdateComputeShader>();

    auto pipeline_state = rhi->CreatePipelineState(RHIPipelineState::PipelineType::Compute, "UpdateBufferPineline");
    pipeline_state->SetShader<RHIShaderStage::Compute>(shader);

    pipeline_state->Compile();

    auto uniform_buffer =
        rhi->CreateBuffer({.size = sizeof(BufferUpdateComputeShader::UniformBufferData),
                           .usages = RHIBuffer::BufferUsage::UniformBuffer,
                           .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                           .is_dynamic = false},
                          "BufferUpdateUniformBuffer");

    BufferUpdateComputeShader::UniformBufferData ubo{
        .element_size = element_size / static_cast<uint32_t>(sizeof(uint32_t)), .element_count = element_count};
    uniform_buffer->UploadImmediate(&ubo);

    auto index_buffer =
        rhi->CreateBuffer({.size = ARRAY_SIZE(indices),
                           .usages = RHIBuffer::BufferUsage::StorageBuffer,
                           .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                           .is_dynamic = false},
                          "BufferUpdateIndexBuffer");

    index_buffer->UploadImmediate(indices.data());

    auto data_buffer =
        rhi->CreateBuffer({.size = element_count * element_size,
                           .usages = RHIBuffer::BufferUsage::StorageBuffer,
                           .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                           .is_dynamic = false},
                          "BufferUpdateDataBuffer");

    data_buffer->UploadImmediate(data);

    auto *cs_resources = pipeline_state->GetShaderResource<BufferUpdateComputeShader>();
    cs_resources->ubo().BindResource(uniform_buffer);
    cs_resources->outputBuffer().BindResource(this);
    cs_resources->indexBuffer().BindResource(index_buffer);
    cs_resources->dataBuffer().BindResource(data_buffer);

    auto compute_pass = rhi->CreateComputePass("BufferUpdateComputePass", false);

    rhi->BeginComputePass(compute_pass);

    rhi->DispatchCompute(pipeline_state, {element_count, 1u, 1u}, {64u, 1u, 1u});

    rhi->EndComputePass(compute_pass);
}

void RHIDynamicBuffer::Init(RHIContext *rhi, const RHIBuffer::Attribute &attribute)
{
    frames_in_flight_ = rhi->GetMaxFramesInFlight();
    RHIBuffer::Attribute dynamic_attribute{
        .size = RHIBufferSubAllocation::DynamicBufferCapacity * frames_in_flight_,
        .usages = attribute.usages,
        .mem_properties =
            RHIMemoryProperty::AlwaysMap | RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
        .is_dynamic = false,
    };

    buffer_ =
        rhi->CreateBuffer(dynamic_attribute, "DynamicBuffer" + std::to_string(static_cast<uint32_t>(attribute.usages)));
}

RHIBufferSubAllocation RHIDynamicBuffer::Allocate(unsigned size)
{
    ASSERT(CanAllocate(size));

    RHIBufferSubAllocation sub_allocation(frames_in_flight_, allocated_size_, this);

    allocated_size_ = utilities::AlignAddress(allocated_size_ + size, MemoryAddressAlignment);

    return sub_allocation;
}

RHIBufferSubAllocation::RHIBufferSubAllocation(unsigned frames_in_flight, unsigned offset, RHIDynamicBuffer *parent)
    : offset_(offset), parent_(parent)
{
    for (auto i = 0u; i < frames_in_flight; i++)
    {
        cpu_address_.push_back(parent->GetUnderlyingBuffer()->GetMappedAddress() + GetOffset(i));
    }
}

RHIBufferSubAllocation RHIBufferManager::SubAllocateDynamicBuffer(const RHIBuffer::Attribute &attribute)
{
    auto index = attribute.usages;
    auto found = dynamic_buffers_.find(index);
    if (found == dynamic_buffers_.end())
    {
        found = dynamic_buffers_.insert({index, RHIDynamicBuffer()}).first;
        found->second.Init(rhi_, attribute);
    }

    auto &matching_buffer = found->second;

    return matching_buffer.Allocate(static_cast<unsigned>(attribute.size));
}

void RHIBufferSubAllocation::Deallocate()
{
    if (parent_)
    {
        parent_->Deallocate(*this);
        parent_ = nullptr;
    }
}

RHIResourceRef<RHIBuffer> RHIBufferSubAllocation::GetBuffer() const
{
    ASSERT(parent_);
    return parent_->GetUnderlyingBuffer();
}

void RHIDynamicBuffer::Deallocate(RHIBufferSubAllocation &)
{
    // TODO(tqjxlm): memory management
}

void RHIBuffer::UploadImmediate(const void *data)
{
    // check that this buffer can be mapped
    ASSERT(GetMemoryProperty() & RHIMemoryProperty::HostVisible);

    void *buffer = Lock();
    memcpy(buffer, data, attribute_.size);
    UnLock();
}

void RHIBuffer::Upload(RHIContext *rhi, const void *data)
{
    if (IsDynamic())
    {
        // if this buffer is dynamic, we can safely copy from cpu.
        // it is guaranteed that the target location is not in use
        memcpy(dynamic_allocation_.GetMappedAddress(rhi->GetFrameIndex()), data, GetSize());
    }
    else
    {
        // otherwise, we create a staging buffer and enqueue a copy command on GPU
        // TODO(tqjxlm): use a dynamic buffer as global staging buffer instead of creating one every time
        RHIBuffer::Attribute staging_attribute{.size = GetSize(),
                                               .usages = RHIBuffer::BufferUsage::TransferSrc,
                                               .mem_properties =
                                                   RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                                               .is_dynamic = false};

        auto staging_buffer = rhi->CreateBuffer(staging_attribute, "UploadStagingBuffer");

        staging_buffer->UploadImmediate(data);

        staging_buffer->CopyToBuffer(this);
    }
}
} // namespace sparkle
