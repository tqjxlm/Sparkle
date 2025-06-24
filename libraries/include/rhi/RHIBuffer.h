#pragma once

#include "rhi/RHIResource.h"

#include "core/Exception.h"
#include "rhi/RHIMemory.h"

namespace sparkle
{
class RHIDynamicBuffer;
class RHIBuffer;
class RHIImage;

class RHIBufferSubAllocation
{
public:
    // max size of every dynamic buffer in bytes
    constexpr static uint32_t DynamicBufferCapacity = 32 * 1024 * 1024;

    RHIBufferSubAllocation() = default;

    RHIBufferSubAllocation(unsigned frames_in_flight, unsigned offset, RHIDynamicBuffer *parent);

    void Deallocate();

    [[nodiscard]] bool IsValid() const
    {
        return parent_ != nullptr;
    }

    [[nodiscard]] unsigned GetOffset(unsigned frame_in_flight) const
    {
        ASSERT(frame_in_flight < UINT_MAX);
        return offset_ + DynamicBufferCapacity * frame_in_flight;
    }

    [[nodiscard]] RHIResourceRef<RHIBuffer> GetBuffer() const;

    [[nodiscard]] void *GetMappedAddress(unsigned frame_in_flight) const
    {
        return cpu_address_[frame_in_flight];
    }

private:
    unsigned offset_;
    // this address is pre-offseted
    std::vector<void *> cpu_address_;
    RHIDynamicBuffer *parent_ = nullptr;
};

class RHIBuffer : public RHIResource
{
public:
    enum class BufferUsage : uint16_t
    {
        None = 0,
        TransferSrc = 0x00000001,
        TransferDst = 0x00000002,
        UniformBuffer = 0x00000004,
        VertexBuffer = 0x00000008,
        IndexBuffer = 0x00000010,
        StorageBuffer = 0x00000020,
        DeviceAddress = 0x00000040,
        AccelerationStructureBuildInput = 0x00000080,
        AccelerationStructureStorage = 0x00000100,
    };

    struct Attribute
    {
        size_t size;
        BufferUsage usages;
        RHIMemoryProperty mem_properties;
        bool is_dynamic;
    };

    RHIBuffer(const Attribute &attribute, const std::string &name) : RHIResource(name), attribute_(attribute)
    {
    }

    [[nodiscard]] auto GetSize() const
    {
        return attribute_.size;
    }

    [[nodiscard]] bool IsValid() const
    {
        return attribute_.size > 0;
    }

    [[nodiscard]] RHIBuffer::BufferUsage GetUsage() const
    {
        return attribute_.usages;
    }

    [[nodiscard]] size_t GetOffset(unsigned frame_index) const
    {
        if (IsDynamic())
        {
            return dynamic_allocation_.GetOffset(frame_index);
        }

        return 0;
    }

    [[nodiscard]] RHIMemoryProperty GetMemoryProperty() const
    {
        return attribute_.mem_properties;
    }

    [[nodiscard]] bool IsDynamic() const override
    {
        return attribute_.is_dynamic;
    }

    [[nodiscard]] uint8_t *GetMappedAddress() const
    {
        ASSERT(GetMemoryProperty() & RHIMemoryProperty::AlwaysMap);
        return mapped_address_;
    }

    // immediately upload data in a lock-copy-unlock manner
    // CAUTION: it does not guarantee resource safety and will block the GPU
    // only use it when you create a new buffer
    void UploadImmediate(const void *data);

    // async upload that happens on the GPU
    // it does not block resources and avoids writing to resources in use
    // the cost is higher memory footprint
    void Upload(RHIContext *rhi, const void *data);

    virtual void CopyToBuffer(const RHIBuffer *buffer) const = 0;

    virtual void CopyToImage(const RHIImage *image) const = 0;

    virtual void *Lock() = 0;

    virtual void UnLock() = 0;

    template <class T>
    void PartialUpdate(RHIContext *rhi, const std::vector<T> &data, const std::vector<uint32_t> &indices)
    {
        PartialUpdate(rhi, reinterpret_cast<const uint8_t *>(data.data()), indices, static_cast<uint32_t>(data.size()),
                      sizeof(T));
    }

    void PartialUpdate(RHIContext *rhi, const uint8_t *data, const std::vector<uint32_t> &indices,
                       uint32_t element_count, uint32_t element_size);

protected:
    Attribute attribute_;

    // when IsDynamic(), RHIBuffer does not contain real resource. dynamic_allocation points to the real resource
    RHIBufferSubAllocation dynamic_allocation_;

    uint8_t *mapped_address_ = nullptr;
};

RegisterEnumAsFlag(RHIBuffer::BufferUsage);

// a persistently mapped buffer that is safe to update on the CPU side
class RHIDynamicBuffer
{
    // hardware requirement for address alignment
    // TODO(tqjxlm): make it dynamic
    constexpr static uint32_t MemoryAddressAlignment = 64;

public:
    void Init(RHIContext *rhi, const RHIBuffer::Attribute &attribute);

    RHIBufferSubAllocation Allocate(unsigned size);

    void Deallocate(RHIBufferSubAllocation &allocation);

    [[nodiscard]] bool CanAllocate(unsigned size) const
    {
        return allocated_size_ + size <= RHIBufferSubAllocation::DynamicBufferCapacity;
    }

    [[nodiscard]] auto GetUnderlyingBuffer() const
    {
        return buffer_;
    }

private:
    unsigned allocated_size_;
    RHIResourceRef<RHIBuffer> buffer_;
    unsigned frames_in_flight_;
};

class RHIBufferManager
{
public:
    explicit RHIBufferManager(RHIContext *rhi) : rhi_(rhi)
    {
    }

    [[nodiscard]] RHIBufferSubAllocation SubAllocateDynamicBuffer(const RHIBuffer::Attribute &attribute);

private:
    RHIContext *rhi_ = nullptr;
    std::unordered_map<RHIBuffer::BufferUsage, RHIDynamicBuffer> dynamic_buffers_;
};
} // namespace sparkle
