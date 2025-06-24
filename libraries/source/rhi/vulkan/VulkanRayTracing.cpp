#if ENABLE_VULKAN

#include "VulkanRayTracing.h"

#include "VulkanBuffer.h"
#include "VulkanCommandBuffer.h"
#include "VulkanCommon.h"
#include "VulkanContext.h"
#include "VulkanDescriptorSet.h"

namespace sparkle
{
void VulkanBLAS::Build()
{
    VkDevice device = context->GetDevice();

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

    // the building process need a temporary buffer, i.e. scratch buffer
    // TODO(tqjxlm): it's expensive to create a buffer for every blas, make a pool instead
    const VulkanBuffer scratch_buffer(
        {.size = size_info.buildScratchSize,
         .usages = RHIBuffer::BufferUsage::StorageBuffer | RHIBuffer::BufferUsage::DeviceAddress,
         .mem_properties = RHIMemoryProperty::DeviceLocal,
         .is_dynamic = false},
        "BLASScratchBuffer");

    build_info.scratchData = scratch_buffer.GetDeviceAddress();
    build_info.srcAccelerationStructure = VK_NULL_HANDLE;
    build_info.dstAccelerationStructure = acceleration_structure_;

    const VkAccelerationStructureBuildRangeInfoKHR *ranges[1] = {&range};

    {
        // TODO(tqjxlm): use a shared command buffer
        const OneShotCommandBufferScope command_buffer_scope;
        VkCommandBuffer command_buffer = command_buffer_scope.GetCommandBuffer();
        vkCmdBuildAccelerationStructuresKHR(command_buffer, 1, &build_info, ranges);
    }

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
    vkDestroyAccelerationStructureKHR(context->GetDevice(), acceleration_structure_, nullptr);
}

VulkanTLAS::~VulkanTLAS()
{
    vkDestroyAccelerationStructureKHR(context->GetDevice(), acceleration_structure_, nullptr);
}

void VulkanTLAS::Build()
{
    const size_t num_meshes = all_blas_.size();

    // create instances for our meshes
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    instances.resize(num_meshes);
    for (auto primitive_id = 0u; primitive_id < num_meshes; primitive_id++)
    {
        auto *blas = RHICast<VulkanBLAS>(all_blas_[primitive_id]);

        if (!blas)
        {
            continue;
        }

        if (blas->IsDirty())
        {
            blas->Build();
        }

        auto &instance = instances[primitive_id];
        instance = blas->GetDescriptor();
        instance.instanceCustomIndex = primitive_id;
    }

    context->GetRHI()->RecreateBuffer(
        RHIBuffer::Attribute{.size = instances.size() * sizeof(VkAccelerationStructureInstanceKHR),
                             .usages = RHIBuffer::BufferUsage::DeviceAddress |
                                       RHIBuffer::BufferUsage::AccelerationStructureBuildInput,
                             .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                             .is_dynamic = false},
        "TLASInstanceBuffer", instance_buffer_);

    instance_buffer_->UploadImmediate(instances.data());

    BuildInternal(true);

    context->SetDebugInfo(reinterpret_cast<uint64_t>(acceleration_structure_),
                          VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, GetName().c_str());
}

void VulkanTLAS::Update(const std::unordered_set<uint32_t> &instances_to_update)
{
    auto *instance_data = reinterpret_cast<VkAccelerationStructureInstanceKHR *>(instance_buffer_->Lock());

    for (auto primitive_id : instances_to_update)
    {
        auto *blas = RHICast<VulkanBLAS>(all_blas_[primitive_id]);

        if (blas->IsDirty())
        {
            blas->Build();
        }

        instance_data[primitive_id] = blas->GetDescriptor();
        instance_data[primitive_id].instanceCustomIndex = primitive_id;
    }

    instance_buffer_->UnLock();

    BuildInternal(false);
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
    build_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build_info.geometryCount = 1;
    build_info.pGeometries = &tlas_geo_info;

    const auto num_instances = static_cast<uint32_t>(all_blas_.size());

    if (rebuild)
    {
        VkAccelerationStructureBuildSizesInfoKHR size_info{};
        size_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        vkGetAccelerationStructureBuildSizesKHR(context->GetDevice(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                &build_info, &num_instances, &size_info);

        if (acceleration_structure_ != nullptr)
        {
            context->GetRHI()->EnqueueEndOfRenderTasks([acceleration_structure = acceleration_structure_]() {
                vkDestroyAccelerationStructureKHR(context->GetDevice(), acceleration_structure, nullptr);
            });
        }

        context->GetRHI()->RecreateBuffer(
            RHIBuffer::Attribute{.size = size_info.accelerationStructureSize,
                                 .usages = RHIBuffer::BufferUsage::AccelerationStructureStorage,
                                 .mem_properties = RHIMemoryProperty::DeviceLocal,
                                 .is_dynamic = false},
            "TLASBuffer", buffer_);

        VkAccelerationStructureCreateInfoKHR create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        create_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        create_info.size = size_info.accelerationStructureSize;
        create_info.buffer = RHICast<VulkanBuffer>(buffer_)->GetResourceThisFrame();

        CHECK_VK_ERROR(
            vkCreateAccelerationStructureKHR(context->GetDevice(), &create_info, nullptr, &acceleration_structure_));

        context->GetRHI()->RecreateBuffer(RHIBuffer::Attribute{.size = size_info.buildScratchSize,
                                                               .usages = RHIBuffer::BufferUsage::StorageBuffer |
                                                                         RHIBuffer::BufferUsage::DeviceAddress,
                                                               .mem_properties = RHIMemoryProperty::DeviceLocal,
                                                               .is_dynamic = false},
                                          "TLASScratchBuffer", scratch_buffer_);
    }

    build_info.scratchData = RHICast<VulkanBuffer>(scratch_buffer_)->GetDeviceAddress();
    build_info.srcAccelerationStructure = rebuild ? VK_NULL_HANDLE : acceleration_structure_;
    build_info.dstAccelerationStructure = acceleration_structure_;

    VkAccelerationStructureBuildRangeInfoKHR range = {};
    range.primitiveCount = num_instances;

    const VkAccelerationStructureBuildRangeInfoKHR *ranges[1] = {&range};

    vkCmdBuildAccelerationStructuresKHR(context->GetCurrentCommandBuffer(), 1, &build_info, ranges);
}
} // namespace sparkle

#endif
