#include "rhi/RHIBuffer.h"

#include "rhi/RHI.h"

namespace sparkle
{
class BufferUpdateComputeShader : public RHIShaderInfo
{
    REGISTGER_SHADER(BufferUpdateComputeShader, RHIShaderStage::Compute, "shaders/utilities/buffer_update.cs.slang",
                     "shader_main")

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
    offset_alignment_ = rhi->GetMinBufferOffsetAlignment();
    capacity_ = attribute.dynamic_buffer_capacity;
    ASSERT(capacity_ > 0);
    ASSERT_F(capacity_ % offset_alignment_ == 0, "Dynamic buffer capacity {} must be aligned to {}", capacity_,
             offset_alignment_);
    RHIBuffer::Attribute dynamic_attribute{
        .size = static_cast<size_t>(capacity_) * frames_in_flight_,
        .usages = attribute.usages,
        .mem_properties =
            RHIMemoryProperty::AlwaysMap | RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
        .is_dynamic = false,
    };

    buffer_ =
        rhi->CreateBuffer(dynamic_attribute, "DynamicBuffer" + std::to_string(static_cast<uint32_t>(attribute.usages)) +
                                                 "_" + std::to_string(capacity_));

    free_ranges_.emplace(0, capacity_);
}

bool RHIDynamicBuffer::CanAllocate(unsigned size) const
{
    const auto aligned_size = utilities::AlignAddress(size, offset_alignment_);
    return std::ranges::any_of(free_ranges_,
                               [aligned_size](const auto &range) { return range.second >= aligned_size; });
}

RHIBufferSubAllocation RHIDynamicBuffer::Allocate(unsigned size)
{
    const auto aligned_size = utilities::AlignAddress(size, offset_alignment_);
    auto range = std::ranges::find_if(
        free_ranges_, [aligned_size](const auto &candidate) { return candidate.second >= aligned_size; });
    ASSERT_F(range != free_ranges_.end(), "Dynamic buffer capacity exceeded by allocation of {} bytes", size);

    const auto offset = range->first;
    const auto remaining_size = range->second - aligned_size;
    free_ranges_.erase(range);

    if (remaining_size > 0)
    {
        free_ranges_.emplace(offset + aligned_size, remaining_size);
    }

    return RHIBufferSubAllocation(frames_in_flight_, offset, aligned_size, capacity_, this);
}

RHIBufferSubAllocation::RHIBufferSubAllocation(unsigned frames_in_flight, unsigned offset, unsigned size,
                                               unsigned frame_stride, RHIDynamicBuffer *parent)
    : offset_(offset), size_(size), frame_stride_(frame_stride), parent_(parent)
{
    for (auto i = 0u; i < frames_in_flight; i++)
    {
        cpu_address_.push_back(parent->GetUnderlyingBuffer()->GetMappedAddress() + GetOffset(i));
    }
}

RHIBufferSubAllocation RHIBufferManager::SubAllocateDynamicBuffer(const RHIBuffer::Attribute &attribute)
{
    const DynamicBufferKey index{attribute.usages, attribute.dynamic_buffer_capacity};
    auto found = dynamic_buffers_.find(index);
    if (found == dynamic_buffers_.end())
    {
        found = dynamic_buffers_.insert({index, RHIDynamicBuffer()}).first;
        found->second.Init(rhi_, attribute);
    }

    auto &matching_buffer = found->second;

    return matching_buffer.Allocate(static_cast<unsigned>(attribute.size));
}

bool RHIBufferManager::CanSubAllocateDynamicBuffer(const RHIBuffer::Attribute &attribute) const
{
    if (attribute.dynamic_buffer_capacity == 0 || attribute.size > attribute.dynamic_buffer_capacity)
    {
        return false;
    }

    const DynamicBufferKey index{attribute.usages, attribute.dynamic_buffer_capacity};
    const auto found = dynamic_buffers_.find(index);
    if (found != dynamic_buffers_.end())
    {
        return found->second.CanAllocate(static_cast<unsigned>(attribute.size));
    }

    const auto aligned_size = utilities::AlignAddress(attribute.size, rhi_->GetMinBufferOffsetAlignment());
    return aligned_size <= attribute.dynamic_buffer_capacity;
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

void RHIDynamicBuffer::Deallocate(RHIBufferSubAllocation &allocation)
{
    ASSERT(allocation.parent_ == this);
    ASSERT(allocation.size_ > 0);

    auto offset = allocation.offset_;
    auto size = allocation.size_;
    auto next = free_ranges_.lower_bound(offset);

    ASSERT(next == free_ranges_.end() || offset + size <= next->first);

    if (next != free_ranges_.begin())
    {
        auto previous = std::prev(next);
        ASSERT(previous->first + previous->second <= offset);

        if (previous->first + previous->second == offset)
        {
            offset = previous->first;
            size += previous->second;
            free_ranges_.erase(previous);
        }
    }

    if (next != free_ranges_.end() && offset + size == next->first)
    {
        size += next->second;
        free_ranges_.erase(next);
    }

    const bool inserted = free_ranges_.emplace(offset, size).second;
    ASSERT(inserted);
    (void)inserted;

    allocation.size_ = 0;
}

RHIBuffer::~RHIBuffer()
{
    dynamic_allocation_.Deallocate();
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
        auto staging_buffer = rhi->CreateUploadStagingBuffer(GetSize());
        if (staging_buffer->IsDynamic())
        {
            staging_buffer->Upload(rhi, data);
        }
        else
        {
            staging_buffer->UploadImmediate(data);
        }

        staging_buffer->CopyToBuffer(this);
    }
}
} // namespace sparkle
