// Reports whether the default Metal device supports Metal 4, and proves it by running a compute dispatch, a barrier
// and a copy through the Metal 4 core API (queue, allocator, command buffer, residency set, argument table, compiler,
// commit feedback). Built and run by dev/metal4_probe.py.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static const NSUInteger ElementCount = 256;

static NSString *const KernelSource = @"kernel void probe(device uint *out [[buffer(0)]],"
                                       "                  uint id [[thread_position_in_grid]])"
                                       "{ out[id] = id * 3 + 1; }";

static void PrintFamilies(id<MTLDevice> device)
{
    const struct
    {
        const char *name;
        MTLGPUFamily family;
    } families[] = {
        {"Apple7", MTLGPUFamilyApple7},   {"Apple8", MTLGPUFamilyApple8},
        {"Apple9", MTLGPUFamilyApple9},   {"Metal3", MTLGPUFamilyMetal3},
#if __has_include(<Metal/MTL4CommandQueue.h>)
        {"Apple10", MTLGPUFamilyApple10}, {"Metal4", MTLGPUFamilyMetal4},
#endif
    };

    for (const auto &entry : families)
    {
        printf("family %s: %s\n", entry.name, [device supportsFamily:entry.family] ? "yes" : "no");
    }
}

#if __has_include(<Metal/MTL4CommandQueue.h>)
API_AVAILABLE(macos(26.0), ios(26.0)) static NSString *RunMetal4Workload(id<MTLDevice> device)
{
    id<MTL4CommandQueue> queue = [device newMTL4CommandQueue];
    if (!queue)
    {
        return @"newMTL4CommandQueue returned nil";
    }

    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:KernelSource options:nil error:&error];
    if (!library)
    {
        return [NSString stringWithFormat:@"library: %@", error];
    }

    MTL4LibraryFunctionDescriptor *function = [[MTL4LibraryFunctionDescriptor alloc] init];
    function.library = library;
    function.name = @"probe";
    MTL4ComputePipelineDescriptor *pipeline_descriptor = [[MTL4ComputePipelineDescriptor alloc] init];
    pipeline_descriptor.computeFunctionDescriptor = function;

    id<MTL4Compiler> compiler = [device newCompilerWithDescriptor:[[MTL4CompilerDescriptor alloc] init] error:&error];
    if (!compiler)
    {
        return [NSString stringWithFormat:@"compiler: %@", error];
    }
    id<MTLComputePipelineState> pipeline = [compiler newComputePipelineStateWithDescriptor:pipeline_descriptor
                                                                       compilerTaskOptions:nil
                                                                                     error:&error];
    if (!pipeline)
    {
        return [NSString stringWithFormat:@"pipeline: %@", error];
    }

    const NSUInteger size = ElementCount * sizeof(uint32_t);
    id<MTLBuffer> written = [device newBufferWithLength:size options:MTLResourceStorageModePrivate];
    id<MTLBuffer> readback = [device newBufferWithLength:size options:MTLResourceStorageModeShared];

    id<MTLResidencySet> residency = [device newResidencySetWithDescriptor:[[MTLResidencySetDescriptor alloc] init]
                                                                    error:&error];
    if (!residency)
    {
        return [NSString stringWithFormat:@"residency set: %@", error];
    }
    [residency addAllocation:written];
    [residency addAllocation:readback];
    [residency commit];
    [queue addResidencySet:residency];

    MTL4ArgumentTableDescriptor *table_descriptor = [[MTL4ArgumentTableDescriptor alloc] init];
    table_descriptor.maxBufferBindCount = 1;
    id<MTL4ArgumentTable> table = [device newArgumentTableWithDescriptor:table_descriptor error:&error];
    if (!table)
    {
        return [NSString stringWithFormat:@"argument table: %@", error];
    }
    [table setAddress:written.gpuAddress atIndex:0];

    id<MTL4CommandAllocator> allocator = [device newCommandAllocator];
    id<MTL4CommandBuffer> command_buffer = [device newCommandBuffer];
    [command_buffer beginCommandBufferWithAllocator:allocator];

    id<MTL4ComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setArgumentTable:table];
    [encoder dispatchThreads:MTLSizeMake(ElementCount, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [encoder barrierAfterEncoderStages:MTLStageDispatch
                   beforeEncoderStages:MTLStageBlit
                     visibilityOptions:MTL4VisibilityOptionDevice];
    [encoder copyFromBuffer:written sourceOffset:0 toBuffer:readback destinationOffset:0 size:size];
    [encoder endEncoding];
    [command_buffer endCommandBuffer];

    __block NSError *gpu_error = nil;
    id<MTLSharedEvent> done = [device newSharedEvent];
    MTL4CommitOptions *options = [[MTL4CommitOptions alloc] init];
    [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
      gpu_error = feedback.error;
      done.signaledValue = 1;
    }];

    id<MTL4CommandBuffer> command_buffers[] = {command_buffer};
    [queue commit:command_buffers count:1 options:options];
    if (![done waitUntilSignaledValue:1 timeoutMS:10000])
    {
        return @"timed out waiting for the GPU";
    }
    if (gpu_error)
    {
        return [NSString stringWithFormat:@"GPU error: %@", gpu_error];
    }

    const auto *values = static_cast<const uint32_t *>(readback.contents);
    for (uint32_t i = 0; i < ElementCount; i++)
    {
        if (values[i] != i * 3 + 1)
        {
            return [NSString stringWithFormat:@"wrong result at %u: %u", i, values[i]];
        }
    }
    return nil;
}
#endif

int main()
{
    @autoreleasepool
    {
        printf("os: %s\n", NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String);

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device)
        {
            printf("RESULT: no Metal device\n");
            return 1;
        }
        printf("device: %s\n", device.name.UTF8String);
        printf("ray tracing: %s\n", device.supportsRaytracing ? "yes" : "no");
        PrintFamilies(device);

#if __has_include(<Metal/MTL4CommandQueue.h>)
        if (@available(macOS 26.0, iOS 26.0, *))
        {
            NSString *failure = RunMetal4Workload(device);
            printf("RESULT: Metal 4 workload %s%s\n", failure ? "failed: " : "passed",
                   failure ? failure.UTF8String : "");
            return failure ? 1 : 0;
        }
        printf("RESULT: OS older than 26, no Metal 4\n");
#else
        printf("RESULT: SDK has no Metal 4 headers\n");
#endif
        return 1;
    }
}
