#if FRAMEWORK_APPLE

#include "MetalRayTracing.h"

#include "MetalBuffer.h"
#include "MetalContext.h"

#include <limits>

namespace sparkle
{
static constexpr uint32_t InvalidInstanceIndex = std::numeric_limits<uint32_t>::max();

static void ResizeScratchBuffer(id<MTLBuffer> __strong &scratch_buffer, NSUInteger required_size)
{
    if (!scratch_buffer || scratch_buffer.length < required_size)
    {
        scratch_buffer = [context->GetDevice() newBufferWithLength:required_size options:MTLResourceStorageModePrivate];
    }
}

static id<MTLAccelerationStructure> NewAccelerationStructureWithDescriptor(
    MTLAccelerationStructureDescriptor *descriptor, id<MTLAccelerationStructureCommandEncoder> command_encoder,
    id<MTLBuffer> __strong &scratch_buffer)
{
    auto device = context->GetDevice();

    // Query for the sizes needed to store and build the acceleration structure.
    MTLAccelerationStructureSizes accel_sizes = [device accelerationStructureSizesWithDescriptor:descriptor];

    // Allocate an acceleration structure large enough for this descriptor. This method
    // doesn't actually build the acceleration structure, but rather allocates memory.
    id<MTLAccelerationStructure> acceleration_structure =
        [device newAccelerationStructureWithSize:accel_sizes.accelerationStructureSize];

    ResizeScratchBuffer(scratch_buffer, accel_sizes.buildScratchBufferSize);

    // Schedule the actual acceleration structure build.
    [command_encoder buildAccelerationStructure:acceleration_structure
                                     descriptor:descriptor
                                  scratchBuffer:scratch_buffer
                            scratchBufferOffset:0];

    return acceleration_structure;
}

static void FillBLASDescriptor(const MetalBLAS *blas, uint32_t instance_index, uint32_t primitive_id,
                               MTLAccelerationStructureUserIDInstanceDescriptor &out_descriptor)
{
    out_descriptor = {};
    out_descriptor.accelerationStructureIndex = instance_index;
    out_descriptor.userID = primitive_id;

    // we do not support ray tracing pipeline yet, so no need to use function table
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

void MetalBLAS::Build(id<MTLAccelerationStructureCommandEncoder> command_encoder,
                      id<MTLBuffer> __strong &scratch_buffer)
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
    acceleration_structure_ = NewAccelerationStructureWithDescriptor(accel_descriptor, command_encoder, scratch_buffer);

    SetDebugInfo(acceleration_structure_, GetName());

    id_dirty_ = true;
    is_dirty_ = false;
}

id<MTLAccelerationStructure> MetalBLAS::Compact(id<MTLAccelerationStructureCommandEncoder> command_encoder,
                                                NSUInteger compacted_size)
{
    auto source = acceleration_structure_;
    auto destination = [context->GetDevice() newAccelerationStructureWithSize:compacted_size];
    if (!destination)
    {
        return nil;
    }

    [command_encoder copyAndCompactAccelerationStructure:source toAccelerationStructure:destination];
    acceleration_structure_ = destination;
    id_dirty_ = true;

    SetDebugInfo(acceleration_structure_, GetName());

    return source;
}

void MetalTLAS::Build()
{
    std::vector<std::pair<uint32_t, MetalBLAS *>> instances;
    instances.reserve(all_blas_.size());
    size_t dirty_blas_count = 0;
    for (auto primitive_id = 0u; primitive_id < all_blas_.size(); primitive_id++)
    {
        auto *blas = RHICast<MetalBLAS>(all_blas_[primitive_id]);

        if (blas)
        {
            instances.emplace_back(primitive_id, blas);
            if (blas->IsDirty())
            {
                dirty_blas_count++;
            }
        }
    }

    const size_t blas_count = instances.size();
    if (blas_count == 0)
    {
        return;
    }

    primitive_to_instance_.assign(all_blas_.size(), InvalidInstanceIndex);

    // a buffer of BLAS descriptors. each descriptor represents a BLAS, with its own transformation matrix.
    blas_descriptor_buffer_ = context->GetRHI()->CreateBuffer(
        {
            .size = sizeof(MTLAccelerationStructureUserIDInstanceDescriptor) * blas_count,
            .usages = RHIBuffer::BufferUsage::StorageBuffer,
            .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
            .is_dynamic = false,
        },
        GetName() + "_BLAS_Descriptor_Buffer");

    auto *blas_descriptors =
        static_cast<MTLAccelerationStructureUserIDInstanceDescriptor *>(blas_descriptor_buffer_->Lock());

    auto command_buffer = context->GetCurrentCommandBuffer();
    id<MTLAccelerationStructureCommandEncoder> command_encoder = [command_buffer accelerationStructureCommandEncoder];

    id<MTLBuffer> compacted_size_buffer = nil;
    if (dirty_blas_count > 0)
    {
        compacted_size_buffer = [context->GetDevice() newBufferWithLength:sizeof(uint64_t) * dirty_blas_count
                                                                  options:MTLResourceStorageModeShared];
        if (!compacted_size_buffer)
        {
            Log(Warn, "Metal BLAS compaction skipped: failed to allocate the compacted-size buffer");
        }
    }

    std::vector<MetalBLAS *> built_blas;
    built_blas.reserve(dirty_blas_count);
    for (size_t instance_index = 0; instance_index < instances.size(); instance_index++)
    {
        const auto [primitive_id, blas] = instances[instance_index];
        primitive_to_instance_[primitive_id] = static_cast<uint32_t>(instance_index);

        if (blas->IsDirty())
        {
            blas->Build(command_encoder, scratch_buffer_);

            if (compacted_size_buffer)
            {
                const NSUInteger offset = built_blas.size() * sizeof(uint64_t);
                [command_encoder writeCompactedAccelerationStructureSize:blas->GetAccelerationStructure()
                                                                toBuffer:compacted_size_buffer
                                                                  offset:offset
                                                            sizeDataType:MTLDataTypeULong];
                built_blas.push_back(blas);
            }
        }

        FillBLASDescriptor(blas, static_cast<uint32_t>(instance_index), primitive_id, blas_descriptors[instance_index]);
    }

    blas_descriptor_buffer_->UnLock();

    if (!built_blas.empty())
    {
        [command_encoder endEncoding];

        auto build_command_buffer = command_buffer;
        context->SubmitCommandBuffer();
        [build_command_buffer waitUntilCompleted];

        if (build_command_buffer.status != MTLCommandBufferStatusCompleted)
        {
            const char *error = build_command_buffer.error
                                    ? [build_command_buffer.error.localizedDescription UTF8String]
                                    : "unknown error";
            Log(Error, "Metal BLAS build failed: {}", error);
            DumpAndAbort();
        }

        context->BeginCommandBuffer();
        command_buffer = context->GetCurrentCommandBuffer();
        command_encoder = [command_buffer accelerationStructureCommandEncoder];

        const auto *compacted_sizes = static_cast<const uint64_t *>(compacted_size_buffer.contents);
        NSMutableArray *retained_sources = [[NSMutableArray alloc] initWithCapacity:built_blas.size()];
        uint64_t source_bytes = 0;
        uint64_t resident_bytes = 0;
        size_t compacted_count = 0;
        for (size_t i = 0; i < built_blas.size(); i++)
        {
            auto *blas = built_blas[i];
            const NSUInteger source_size = blas->GetAccelerationStructure().size;
            const uint64_t compacted_size = compacted_sizes[i];
            source_bytes += source_size;

            if (compacted_size > 0 && compacted_size < source_size)
            {
                if (auto source = blas->Compact(command_encoder, static_cast<NSUInteger>(compacted_size)))
                {
                    [retained_sources addObject:source];
                    resident_bytes += compacted_size;
                    compacted_count++;
                    continue;
                }
            }

            resident_bytes += source_size;
        }

        [command_encoder endEncoding];

        if (retained_sources.count > 0)
        {
            NSArray *sources = [retained_sources copy];
            [command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
              (void)sources;
            }];
        }

        const double saved_percentage = source_bytes == 0 ? 0.0
                                                          : 100.0 * static_cast<double>(source_bytes - resident_bytes) /
                                                                static_cast<double>(source_bytes);
        Log(Info, "Metal BLAS compaction: {}/{} compacted, {} source bytes -> {} resident bytes ({:.1f}% saved)",
            compacted_count, built_blas.size(), source_bytes, resident_bytes, saved_percentage);

        command_encoder = [command_buffer accelerationStructureCommandEncoder];
    }

    blas_array_ = [[NSMutableArray alloc] initWithCapacity:blas_count];
    for (const auto &instance : instances)
    {
        [blas_array_ addObject:instance.second->GetAccelerationStructure()];
    }

    auto *tlas_descriptor = [MTLInstanceAccelerationStructureDescriptor descriptor];

    tlas_descriptor.instancedAccelerationStructures = blas_array_;
    tlas_descriptor.instanceCount = blas_count;
    tlas_descriptor.instanceDescriptorBuffer = RHICast<MetalBuffer>(blas_descriptor_buffer_)->GetResource();
    tlas_descriptor.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeUserID;
    tlas_descriptor.usage = MTLAccelerationStructureUsageRefit;

    // Create the instance acceleration structure that contains all instances in the scene.
    tlas_ = NewAccelerationStructureWithDescriptor(tlas_descriptor, command_encoder, scratch_buffer_);
    id_dirty_ = true;

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
    for (auto primitive_id : instances_to_update)
    {
        if (primitive_id >= all_blas_.size() || primitive_id >= primitive_to_instance_.size())
        {
            Build();
            return;
        }

        auto *blas = RHICast<MetalBLAS>(all_blas_[primitive_id]);
        const auto instance_index = primitive_to_instance_[primitive_id];
        if (!blas || instance_index == InvalidInstanceIndex || instance_index >= blas_array_.count || blas->IsDirty())
        {
            Build();
            return;
        }
    }

    auto device = context->GetDevice();
    auto command_buffer = context->GetCurrentCommandBuffer();
    id<MTLAccelerationStructureCommandEncoder> command_encoder = [command_buffer accelerationStructureCommandEncoder];

    auto *blas_descriptors =
        static_cast<MTLAccelerationStructureUserIDInstanceDescriptor *>(blas_descriptor_buffer_->Lock());

    for (auto primitive_id : instances_to_update)
    {
        auto *blas = RHICast<MetalBLAS>(all_blas_[primitive_id]);
        const auto instance_index = primitive_to_instance_[primitive_id];

        [blas_array_ replaceObjectAtIndex:instance_index withObject:blas->GetAccelerationStructure()];

        FillBLASDescriptor(blas, instance_index, primitive_id, blas_descriptors[instance_index]);
    }

    blas_descriptor_buffer_->UnLock();

    auto *tlas_descriptor = [MTLInstanceAccelerationStructureDescriptor descriptor];

    tlas_descriptor.instancedAccelerationStructures = blas_array_;
    tlas_descriptor.instanceCount = blas_array_.count;
    tlas_descriptor.instanceDescriptorBuffer = RHICast<MetalBuffer>(blas_descriptor_buffer_)->GetResource();
    tlas_descriptor.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeUserID;
    tlas_descriptor.usage = MTLAccelerationStructureUsageRefit;

    // Query for the sizes needed to store and build the acceleration structure.
    MTLAccelerationStructureSizes accel_sizes = [device accelerationStructureSizesWithDescriptor:tlas_descriptor];

    ResizeScratchBuffer(scratch_buffer_, accel_sizes.refitScratchBufferSize);

    [command_encoder refitAccelerationStructure:tlas_
                                     descriptor:tlas_descriptor
                                    destination:tlas_
                                  scratchBuffer:scratch_buffer_
                            scratchBufferOffset:0];

    [command_encoder endEncoding];
}
} // namespace sparkle

#endif
