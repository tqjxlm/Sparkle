#if ENABLE_VULKAN

#include "VulkanRayTracing.h"

#include "VulkanBuffer.h"
#include "VulkanCommon.h"
#include "VulkanContext.h"
#include "VulkanDescriptorSet.h"
#include "core/math/Utilities.h"

namespace sparkle
{
namespace
{
class RetiredAccelerationStructure
{
public:
    RetiredAccelerationStructure(VkAccelerationStructureKHR acceleration_structure, RHIResourceRef<RHIBuffer> buffer)
        : acceleration_structure_(acceleration_structure), buffer_(std::move(buffer))
    {
    }

    ~RetiredAccelerationStructure()
    {
        vkDestroyAccelerationStructureKHR(context->GetDevice(), acceleration_structure_, nullptr);
        buffer_ = nullptr;
    }

private:
    VkAccelerationStructureKHR acceleration_structure_;
    RHIResourceRef<RHIBuffer> buffer_;
};

void ResizeScratchBuffer(VkDeviceSize required_size, RHIResourceRef<RHIBuffer> &scratch_buffer)
{
    const auto alignment = context->GetMinAccelerationStructureScratchOffsetAlignment();
    context->GetRHI()->RecreateBuffer(
        RHIBuffer::Attribute{.size = static_cast<size_t>(required_size) + alignment - 1,
                             .usages = RHIBuffer::BufferUsage::StorageBuffer | RHIBuffer::BufferUsage::DeviceAddress,
                             .mem_properties = RHIMemoryProperty::DeviceLocal,
                             .is_dynamic = false},
        "AccelerationStructureScratchBuffer", scratch_buffer);
}

VkDeviceOrHostAddressKHR GetScratchAddress(const RHIResourceRef<RHIBuffer> &scratch_buffer)
{
    VkDeviceOrHostAddressKHR address{};
    const auto buffer_address = RHICast<VulkanBuffer>(scratch_buffer)->GetDeviceAddress().deviceAddress;
    address.deviceAddress =
        utilities::AlignAddress(buffer_address, context->GetMinAccelerationStructureScratchOffsetAlignment());
    return address;
}

void SynchronizeAccelerationStructureBuild(VkCommandBuffer command_buffer, bool wait_for_shader_reads = false)
{
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask =
        VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask =
        VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;

    VkPipelineStageFlags source_stages = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    if (wait_for_shader_reads)
    {
        source_stages |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }

    vkCmdPipelineBarrier(command_buffer, source_stages, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1,
                         &barrier, 0, nullptr, 0, nullptr);
}

void PublishAccelerationStructureBuild(VkCommandBuffer command_buffer)
{
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;

    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier,
                         0, nullptr, 0, nullptr);
}
} // namespace

void VulkanBLAS::Build(RHIResourceRef<RHIBuffer> &scratch_buffer)
{
    VkDevice device = context->GetDevice();

    ASSERT(acceleration_structure_ == VK_NULL_HANDLE);

    VkAccelerationStructureBuildSizesInfoKHR size_info{};
    size_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;

    VkAccelerationStructureGeometryKHR geometry{};
    VkAccelerationStructureBuildRangeInfoKHR range{};
    VkAccelerationStructureBuildGeometryInfoKHR build_info{};

    range.primitiveCount = num_primitive_;

    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

    geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geometry.geometry.triangles.vertexData = RHICast<VulkanBuffer>(vertex_buffer_)->GetDeviceAddressConst();
    geometry.geometry.triangles.vertexStride = sizeof(Vector3);
    geometry.geometry.triangles.maxVertex = num_vertex_;
    geometry.geometry.triangles.indexData = RHICast<VulkanBuffer>(index_buffer_)->GetDeviceAddressConst();
    geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;

    build_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build_info.geometryCount = 1;
    build_info.pGeometries = &geometry;

    vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build_info,
                                            &range.primitiveCount, &size_info);

    // allocate a buffer and create the BLAS on it
    buffer_ = context->GetRHI()->CreateResource<VulkanBuffer>(
        RHIBuffer::Attribute{.size = size_info.accelerationStructureSize,
                             .usages = RHIBuffer::BufferUsage::AccelerationStructureStorage |
                                       RHIBuffer::BufferUsage::DeviceAddress,
                             .mem_properties = RHIMemoryProperty::DeviceLocal,
                             .is_dynamic = false},
        "BLASBuffer");

    VkAccelerationStructureCreateInfoKHR create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    create_info.size = size_info.accelerationStructureSize;
    create_info.buffer = buffer_->GetResourceThisFrame();

    CHECK_VK_ERROR(
        vkCreateAccelerationStructureKHR(context->GetDevice(), &create_info, nullptr, &acceleration_structure_));

    ResizeScratchBuffer(size_info.buildScratchSize, scratch_buffer);

    build_info.scratchData = GetScratchAddress(scratch_buffer);
    build_info.srcAccelerationStructure = VK_NULL_HANDLE;
    build_info.dstAccelerationStructure = acceleration_structure_;

    const VkAccelerationStructureBuildRangeInfoKHR *ranges[1] = {&range};

    VkCommandBuffer command_buffer = context->GetCurrentCommandBuffer();
    SynchronizeAccelerationStructureBuild(command_buffer);
    vkCmdBuildAccelerationStructuresKHR(command_buffer, 1, &build_info, ranges);

    // after build finishes, retrieve its address on device
    VkAccelerationStructureDeviceAddressInfoKHR address_info = {};
    address_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    address_info.accelerationStructure = acceleration_structure_;
    device_address_ = vkGetAccelerationStructureDeviceAddressKHR(device, &address_info);

    is_dirty_ = false;

    context->SetDebugInfo(reinterpret_cast<uint64_t>(acceleration_structure_),
                          VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, GetName().c_str());
}

VulkanBLAS::~VulkanBLAS()
{
    if (acceleration_structure_ != VK_NULL_HANDLE)
    {
        vkDestroyAccelerationStructureKHR(context->GetDevice(), acceleration_structure_, nullptr);
    }
}

VulkanTLAS::~VulkanTLAS()
{
    if (acceleration_structure_ != VK_NULL_HANDLE)
    {
        vkDestroyAccelerationStructureKHR(context->GetDevice(), acceleration_structure_, nullptr);
    }
}

void VulkanTLAS::Build()
{
    const size_t num_meshes = all_blas_.size();

    instances_.clear();
    instances_.reserve(num_meshes);
    for (auto primitive_id = 0u; primitive_id < num_meshes; primitive_id++)
    {
        auto *blas = RHICast<VulkanBLAS>(all_blas_[primitive_id]);

        if (!blas)
        {
            continue;
        }

        if (blas->IsDirty())
        {
            blas->Build(scratch_buffer_);
        }

        auto &instance = instances_.emplace_back();
        instance = blas->GetDescriptor();
        instance.instanceCustomIndex = primitive_id;
    }

    UploadInstanceBuffer();

    BuildInternal(true);

    context->SetDebugInfo(reinterpret_cast<uint64_t>(acceleration_structure_),
                          VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, GetName().c_str());
}

void VulkanTLAS::Update(const std::unordered_set<uint32_t> &instances_to_update)
{
    for (auto primitive_id : instances_to_update)
    {
        ASSERT(primitive_id < all_blas_.size());
        auto *blas = RHICast<VulkanBLAS>(all_blas_[primitive_id]);
        ASSERT(blas);

        if (blas->IsDirty())
        {
            blas->Build(scratch_buffer_);
        }

        auto instance = std::ranges::find_if(instances_, [primitive_id](const auto &candidate) {
            return candidate.instanceCustomIndex == primitive_id;
        });
        ASSERT(instance != instances_.end());

        *instance = blas->GetDescriptor();
        instance->instanceCustomIndex = primitive_id;
    }

    UploadInstanceBuffer();

    BuildInternal(false);
}

void VulkanTLAS::UploadInstanceBuffer()
{
    const VkAccelerationStructureInstanceKHR empty_instance{};
    const size_t buffer_size = std::max(instances_.size(), size_t{1}) * sizeof(VkAccelerationStructureInstanceKHR);

    instance_buffer_ = context->GetRHI()->CreateBuffer(
        RHIBuffer::Attribute{.size = buffer_size,
                             .usages = RHIBuffer::BufferUsage::DeviceAddress |
                                       RHIBuffer::BufferUsage::AccelerationStructureBuildInput,
                             .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                             .is_dynamic = false},
        "TLASInstanceBuffer");

    instance_buffer_->UploadImmediate(instances_.empty() ? &empty_instance : instances_.data());
}

void VulkanTLAS::WriteDescriptor(uint32_t slot, VkDescriptorSet descriptor_set, VkDescriptorType descriptor_type,
                                 std::vector<VkWriteDescriptorSet> &out_set_write) const
{
    auto &set_write = out_set_write.emplace_back(VkWriteDescriptorSet{});
    InitDescriptorWrite(set_write, slot, descriptor_set, 0, descriptor_type);

    auto &info = *context->GetRHI()->AllocateOneFrameMemory<VkWriteDescriptorSetAccelerationStructureKHR>();
    info = {};
    info.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    info.accelerationStructureCount = 1;
    info.pAccelerationStructures = &GetAccelerationStructure();

    set_write.pNext = &info;
}

void VulkanTLAS::BuildInternal(bool rebuild)
{
    VkAccelerationStructureGeometryInstancesDataKHR tlas_instance_info = {};
    tlas_instance_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tlas_instance_info.data = RHICast<VulkanBuffer>(instance_buffer_)->GetDeviceAddressConst();

    VkAccelerationStructureGeometryKHR tlas_geo_info = {};
    tlas_geo_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tlas_geo_info.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlas_geo_info.geometry.instances = tlas_instance_info;

    VkAccelerationStructureBuildGeometryInfoKHR build_info = {};
    build_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build_info.mode =
        rebuild ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
    build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                       VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    build_info.geometryCount = 1;
    build_info.pGeometries = &tlas_geo_info;

    const auto num_instances = static_cast<uint32_t>(instances_.size());

    if (rebuild)
    {
        VkAccelerationStructureBuildSizesInfoKHR size_info{};
        size_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        vkGetAccelerationStructureBuildSizesKHR(context->GetDevice(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                &build_info, &num_instances, &size_info);

        auto new_buffer = context->GetRHI()->CreateBuffer(
            RHIBuffer::Attribute{.size = size_info.accelerationStructureSize,
                                 .usages = RHIBuffer::BufferUsage::AccelerationStructureStorage,
                                 .mem_properties = RHIMemoryProperty::DeviceLocal,
                                 .is_dynamic = false},
            "TLASBuffer");

        VkAccelerationStructureCreateInfoKHR create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        create_info.size = size_info.accelerationStructureSize;
        create_info.buffer = RHICast<VulkanBuffer>(new_buffer)->GetResourceThisFrame();

        VkAccelerationStructureKHR new_acceleration_structure = VK_NULL_HANDLE;
        CHECK_VK_ERROR(
            vkCreateAccelerationStructureKHR(context->GetDevice(), &create_info, nullptr, &new_acceleration_structure));

        if (acceleration_structure_ != VK_NULL_HANDLE)
        {
            auto retired_acceleration_structure =
                std::make_shared<RetiredAccelerationStructure>(acceleration_structure_, std::move(buffer_));
            context->GetRHI()->EnqueueEndOfRenderTasks(
                [retired_acceleration_structure = std::move(retired_acceleration_structure)]() mutable {
                    retired_acceleration_structure = nullptr;
                });
        }

        buffer_ = std::move(new_buffer);
        acceleration_structure_ = new_acceleration_structure;

        ResizeScratchBuffer(std::max(size_info.buildScratchSize, size_info.updateScratchSize), scratch_buffer_);
    }

    build_info.scratchData = GetScratchAddress(scratch_buffer_);
    build_info.srcAccelerationStructure = rebuild ? VK_NULL_HANDLE : acceleration_structure_;
    build_info.dstAccelerationStructure = acceleration_structure_;

    VkAccelerationStructureBuildRangeInfoKHR range = {};
    range.primitiveCount = num_instances;

    const VkAccelerationStructureBuildRangeInfoKHR *ranges[1] = {&range};

    VkCommandBuffer command_buffer = context->GetCurrentCommandBuffer();
    SynchronizeAccelerationStructureBuild(command_buffer, !rebuild);
    vkCmdBuildAccelerationStructuresKHR(command_buffer, 1, &build_info, ranges);
    PublishAccelerationStructureBuild(command_buffer);
}
} // namespace sparkle

#endif
