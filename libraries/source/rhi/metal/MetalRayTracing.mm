#if FRAMEWORK_APPLE

#include "MetalRayTracing.h"

#include "MetalBuffer.h"
#include "MetalContext.h"

namespace sparkle
{
static id<MTLAccelerationStructure> NewAccelerationStructureWithDescriptor(
    MTLAccelerationStructureDescriptor *descriptor, id<MTLAccelerationStructureCommandEncoder> command_encoder)
{
    auto device = context->GetDevice();

    // Query for the sizes needed to store and build the acceleration structure.
    MTLAccelerationStructureSizes accel_sizes = [device accelerationStructureSizesWithDescriptor:descriptor];

    // Allocate an acceleration structure large enough for this descriptor. This method
    // doesn't actually build the acceleration structure, but rather allocates memory.
    id<MTLAccelerationStructure> acceleration_structure =
        [device newAccelerationStructureWithSize:accel_sizes.accelerationStructureSize];

    // Allocate scratch space Metal uses to build the acceleration structure.
    // TODO(tqjxlm): use a shared scratch buffer
    id<MTLBuffer> scratch_buffer = [device newBufferWithLength:accel_sizes.buildScratchBufferSize
                                                       options:MTLResourceStorageModePrivate];

    // Create an acceleration structure command encoder.

    // Schedule the actual acceleration structure build.
    [command_encoder buildAccelerationStructure:acceleration_structure
                                     descriptor:descriptor
                                  scratchBuffer:scratch_buffer
                            scratchBufferOffset:0];

    return acceleration_structure;

    // TODO(tqjxlm): compaction

    // // Allocate a buffer for Metal to write the compacted accelerated structure's size into.
    // id<MTLBuffer> compacted_size_buffer = [device newBufferWithLength:sizeof(uint32_t)
    //                                                           options:MTLResourceStorageModeShared];

    // // Compute and write the compacted acceleration structure size into the buffer. You
    // // must already have a built acceleration structure because Metal determines the compacted
    // // size based on the final size of the acceleration structure. Compacting an acceleration
    // // structure can potentially reclaim significant amounts of memory because Metal must
    // // create the initial structure using a conservative approach.

    // [command_encoder writeCompactedAccelerationStructureSize:acceleration_structure
    //                                                 toBuffer:compacted_size_buffer
    //                                                   offset:0];

    // // The sample waits for Metal to finish executing the command buffer so that it can
    // // read back the compacted size.

    // // Note: Don't wait for Metal to finish executing the command buffer if you aren't compacting
    // // the acceleration structure, as doing so requires CPU/GPU synchronization. You don't have
    // // to compact acceleration structures, but do so when creating large static acceleration
    // // structures, such as static scene geometry. Avoid compacting acceleration structures that
    // // you rebuild every frame, as the synchronization cost may be significant.

    // [command_buffer waitUntilCompleted];

    // uint32_t compacted_size = *(uint32_t *)compacted_size_buffer.contents;

    // // Allocate a smaller acceleration structure based on the returned size.
    // id<MTLAccelerationStructure> compacted_acceleration_structure =
    //     [device newAccelerationStructureWithSize:compacted_size];

    // // Create another command buffer and encoder.
    // commandBuffer = [_queue commandBuffer];

    // command_encoder = [commandBuffer accelerationStructureCommandEncoder];

    // // Encode the command to copy and compact the acceleration structure into the
    // // smaller acceleration structure.
    // [command_encoder copyAndCompactAccelerationStructure:acceleration_structure
    //                              toAccelerationStructure:compacted_acceleration_structure];

    // // End encoding and commit the command buffer. You don't need to wait for Metal to finish
    // // executing this command buffer as long as you synchronize any ray-intersection work
    // // to run after this command buffer completes. The sample relies on Metal's default
    // // dependency tracking on resources to automatically synchronize access to the new
    // // compacted acceleration structure.
    // [command_encoder endEncoding];
    // [commandBuffer commit];

    // return compacted_acceleration_structure;
}

static void FillBLASDescriptor(const MetalBLAS *blas, uint32_t primitive_id,
                               MTLAccelerationStructureInstanceDescriptor &out_descriptor)
{
    out_descriptor.accelerationStructureIndex = primitive_id;

    // we do not support ray tracing pipeline yet, so no need to use function table

    // instance_descriptors[instance_index].options =
    //     instance.geometry.intersectionFunctionName == nil ? MTLAccelerationStructureInstanceOptionOpaque : 0;
    // instance_descriptors[instance_index].intersectionFunctionTableOffset = 0;

    out_descriptor.options = MTLAccelerationStructureInstanceOptionOpaque;

    out_descriptor.mask = 0xffffffff;

    // metal assumes that the bottom row is (0, 0, 0, 1)
    const auto &transform = blas->GetTransform();
    for (int column = 0; column < 4; column++)
    {
        for (int row = 0; row < 3; row++)
        {
            // need to transpose the matrix
            out_descriptor.transformationMatrix.columns[column][row] = transform(row, column);
        }
    }
}

void MetalBLAS::Build(id<MTLAccelerationStructureCommandEncoder> command_encoder)
{
    MTLAccelerationStructureTriangleGeometryDescriptor *descriptor =
        [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];

    descriptor.indexBuffer = RHICast<MetalBuffer>(index_buffer_)->GetResource();
    descriptor.indexType = MTLIndexTypeUInt32;

    descriptor.vertexBuffer = RHICast<MetalBuffer>(vertex_buffer_)->GetResource();
    descriptor.vertexStride = sizeof(Vector3);
    descriptor.triangleCount = num_primitive_;

    // Assign each piece of geometry a consecutive slot in the intersection function table.
    // descriptor.intersectionFunctionTableOffset = instance_id_;

    // Create a primitive acceleration structure descriptor to contain the single piece
    // of acceleration structure geometry.
    MTLPrimitiveAccelerationStructureDescriptor *accel_descriptor =
        [MTLPrimitiveAccelerationStructureDescriptor descriptor];

    accel_descriptor.geometryDescriptors = @[ descriptor ];

    // Build the acceleration structure.
    acceleration_structure_ = NewAccelerationStructureWithDescriptor(accel_descriptor, command_encoder);

    SetDebugInfo(acceleration_structure_, GetName());

    is_dirty_ = false;
}

void MetalTLAS::Build()
{
    blas_array_ = [[NSMutableArray alloc] init];

    auto blas_count = all_blas_.size();

    // a buffer of BLAS descriptors. each descriptor represents a BLAS, with its own transformation matrix.
    blas_descriptor_buffer_ = context->GetRHI()->CreateBuffer(
        {
            .size = sizeof(MTLAccelerationStructureInstanceDescriptor) * blas_count,
            .usages = RHIBuffer::BufferUsage::StorageBuffer,
            .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
            .is_dynamic = false,
        },
        GetName() + "_BLAS_Descriptor_Buffer");

    auto *blas_descriptors = (MTLAccelerationStructureInstanceDescriptor *)blas_descriptor_buffer_->Lock();

    auto command_buffer = context->GetCurrentCommandBuffer();
    id<MTLAccelerationStructureCommandEncoder> command_encoder = [command_buffer accelerationStructureCommandEncoder];

    for (auto primitive_id = 0u; primitive_id < all_blas_.size(); primitive_id++)
    {
        auto *blas = RHICast<MetalBLAS>(all_blas_[primitive_id]);

        if (!blas)
        {
            continue;
        }

        if (blas->IsDirty())
        {
            blas->Build(command_encoder);
        }

        [blas_array_ addObject:blas->GetAccelerationStructure()];

        blas_descriptors[primitive_id].accelerationStructureIndex = primitive_id;

        FillBLASDescriptor(blas, primitive_id, blas_descriptors[primitive_id]);
    }

    blas_descriptor_buffer_->UnLock();

    auto *tlas_descriptor = [MTLInstanceAccelerationStructureDescriptor descriptor];

    tlas_descriptor.instancedAccelerationStructures = blas_array_;
    tlas_descriptor.instanceCount = blas_count;
    tlas_descriptor.instanceDescriptorBuffer = RHICast<MetalBuffer>(blas_descriptor_buffer_)->GetResource();

    // Create the instance acceleration structure that contains all instances in the scene.
    tlas_ = NewAccelerationStructureWithDescriptor(tlas_descriptor, command_encoder);

    [command_encoder endEncoding];

    SetDebugInfo(tlas_, GetName());
}

void MetalTLAS::Bind(id<MTLCommandEncoder> encoder, RHIShaderStage stage, unsigned binding_point) const
{
    auto render_encoder = (id<MTLRenderCommandEncoder>)encoder;
    auto compute_encoder = (id<MTLComputeCommandEncoder>)encoder;

    switch (stage)
    {
    case RHIShaderStage::Vertex:
        [render_encoder setVertexAccelerationStructure:GetResource() atBufferIndex:binding_point];
        break;
    case RHIShaderStage::Pixel:
        [render_encoder setFragmentAccelerationStructure:GetResource() atBufferIndex:binding_point];
        break;
    case RHIShaderStage::Compute:
        [compute_encoder setAccelerationStructure:GetResource() atBufferIndex:binding_point];
        break;
    default:
        UnImplemented(stage);
        break;
    }

    for (const auto &blas : GetBlasArray())
    {
        if (!blas)
        {
            continue;
        }

        auto *rhi_blas = RHICast<MetalBLAS>(blas);
        switch (stage)
        {
        case RHIShaderStage::Vertex:
            [render_encoder useResource:rhi_blas->GetAccelerationStructure()
                                  usage:MTLResourceUsageRead
                                 stages:MTLRenderStageVertex];
            break;
        case RHIShaderStage::Pixel:
            [render_encoder useResource:rhi_blas->GetAccelerationStructure()
                                  usage:MTLResourceUsageRead
                                 stages:MTLRenderStageFragment];
            break;
        case RHIShaderStage::Compute:
            [compute_encoder useResource:rhi_blas->GetAccelerationStructure() usage:MTLResourceUsageRead];
            break;
        default:
            UnImplemented(stage);
            break;
        }
    }
}

void MetalTLAS::Update(const std::unordered_set<uint32_t> &instances_to_update)
{
    auto device = context->GetDevice();
    auto command_buffer = context->GetCurrentCommandBuffer();
    id<MTLAccelerationStructureCommandEncoder> command_encoder = [command_buffer accelerationStructureCommandEncoder];

    auto *blas_descriptors = (MTLAccelerationStructureInstanceDescriptor *)blas_descriptor_buffer_->Lock();

    for (auto primitive_id : instances_to_update)
    {
        auto *blas = RHICast<MetalBLAS>(all_blas_[primitive_id]);

        if (blas->IsDirty())
        {
            blas->Build(command_encoder);
        }

        [blas_array_ replaceObjectAtIndex:primitive_id withObject:blas->GetAccelerationStructure()];

        FillBLASDescriptor(blas, primitive_id, blas_descriptors[primitive_id]);
    }

    blas_descriptor_buffer_->UnLock();

    auto *tlas_descriptor = [MTLInstanceAccelerationStructureDescriptor descriptor];

    tlas_descriptor.instancedAccelerationStructures = blas_array_;
    tlas_descriptor.instanceCount = all_blas_.size();
    tlas_descriptor.instanceDescriptorBuffer = RHICast<MetalBuffer>(blas_descriptor_buffer_)->GetResource();

    // Query for the sizes needed to store and build the acceleration structure.
    MTLAccelerationStructureSizes accel_sizes = [device accelerationStructureSizesWithDescriptor:tlas_descriptor];

    id<MTLBuffer> scratch_buffer = [device newBufferWithLength:accel_sizes.buildScratchBufferSize
                                                       options:MTLResourceStorageModePrivate];

    [command_encoder refitAccelerationStructure:tlas_
                                     descriptor:tlas_descriptor
                                    destination:tlas_
                                  scratchBuffer:scratch_buffer
                            scratchBufferOffset:0];

    [command_encoder endEncoding];
}
} // namespace sparkle

#endif
