#pragma once

#if ENABLE_VULKAN

#include "VulkanMath.h"
#include "rhi/RHIRayTracing.h"

namespace sparkle
{
class VulkanBuffer;

class VulkanBLAS : public RHIBLAS
{
public:
    using RHIBLAS::RHIBLAS;

    ~VulkanBLAS() override;

    void Build();

    [[nodiscard]] const VkAccelerationStructureKHR &GetAccelerationStructure() const
    {
        return acceleration_structure_;
    }

    [[nodiscard]] const VkDeviceAddress &GetDeviceAddress() const
    {
        return device_address_;
    }

    [[nodiscard]] VkTransformMatrixKHR GetTransform() const
    {
        return GetVulkanMatrix(transform_);
    }

    [[nodiscard]] VkAccelerationStructureInstanceKHR GetDescriptor() const
    {
        VkAccelerationStructureInstanceKHR instance{};
        instance.transform = GetTransform();
        instance.mask = 0xff;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = GetDeviceAddress();
        return instance;
    }

private:
    RHIResourceRef<VulkanBuffer> buffer_;
    VkAccelerationStructureKHR acceleration_structure_;
    VkDeviceAddress device_address_;
};

class VulkanTLAS : public RHITLAS
{
public:
    explicit VulkanTLAS(const std::string &name) : RHITLAS(name)
    {
    }

    ~VulkanTLAS() override;

    void Build() override;

    void Update(const std::unordered_set<uint32_t> &instances_to_update) override;

    [[nodiscard]] const VkAccelerationStructureKHR &GetAccelerationStructure() const
    {
        return acceleration_structure_;
    }

    void WriteDescriptor(uint32_t slot, VkDescriptorSet descriptor_set, VkDescriptorType descriptor_type,
                         std::vector<VkWriteDescriptorSet> &out_set_write) const;

private:
    void BuildInternal(bool rebuild);

    RHIResourceRef<RHIBuffer> buffer_;
    RHIResourceRef<RHIBuffer> instance_buffer_;
    RHIResourceRef<RHIBuffer> scratch_buffer_;
    VkAccelerationStructureKHR acceleration_structure_ = nullptr;
};
} // namespace sparkle

#endif
