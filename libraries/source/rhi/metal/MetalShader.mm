#if FRAMEWORK_APPLE

#include "MetalShader.h"

#include "MetalContext.h"
#include "core/FileManager.h"

namespace sparkle
{
void MetalShader::Load()
{
    if (IsValid())
    {
        return;
    }

    auto path = shader_info_->GetPath() + ".metal";
    auto shader_data = FileManager::GetNativeFileManager()->ReadResourceAsType<std::string>(path);

    MTLCompileOptions *compile_option = [[MTLCompileOptions alloc] init];

    if (context->GetRHI()->SupportsHardwareRayTracing())
    {
        compile_option.languageVersion = MTLLanguageVersion3_0;
    }
    else
    {
        compile_option.languageVersion = MTLLanguageVersion2_2;
    }

    NSError *error;
    NSString *shader_source = [NSString stringWithCString:shader_data.c_str() encoding:NSASCIIStringEncoding];
    ASSERT_F(shader_source.length > 0, "Failed to load shader file {}", path);

    library_ = [context->GetDevice() newLibraryWithSource:shader_source options:compile_option error:&error];

    ASSERT_F(library_, "Failed to load and compile shader library {}. error: {}", path,
             [error.localizedDescription UTF8String]);

    auto actual_entry_point = shader_info_->GetEntryPoint() + "0";
    function_ = [library_ newFunctionWithName:[NSString stringWithUTF8String:actual_entry_point.c_str()]];

    SetDebugInfo(function_, GetName());

    ASSERT_F(function_, "Failed to load shader function {}", path);

    loaded_ = true;
}

#if DESCRIPTOR_SET_AS_ARGUMENT_BUFFER
void MetalShader::SetupArgumentBuffers(id<MTLDevice> device, std::vector<ArgumentBuffer> &buffers,
                                       RHIShaderResourceTable *resource_table) const
{
    const auto &resource_sets = resource_table->GetResourceSets();

    buffers.resize(resource_sets.size());

    for (int set = 0; set < resource_sets.size(); set++)
    {
        const auto &resource_set = resource_sets[set];
        auto &buffer = buffers[set];
        if (resource_set.empty())
        {
            buffer.buffer = nullptr;
            continue;
        }

        auto encoder = [function_ newArgumentEncoderWithBufferIndex:set];

        uint64_t argument_buffer_length = encoder.encodedLength;
        buffer.buffer = [device newBufferWithLength:argument_buffer_length
                                            options:(GetManagedBufferStorageMode() | MTLResourceStorageModeShared)];
        [encoder setArgumentBuffer:buffer.buffer offset:0];

        for (int slot = 0; slot < resource_set.size(); slot++)
        {
            const auto *resource = resource_set[slot];
            // it is possible some slots are not used
            if (!resource)
            {
                continue;
            }

            const auto &sub_resource = resource->GetSubResource(0);
            switch (resource->GetType())
            {
            case RHIShaderResourceReflection::ResourceType::UniformBuffer:
            case RHIShaderResourceReflection::ResourceType::StorageBuffer: {
                auto *rhi_buffer = RHICast<MetalBuffer>(sub_resource);
                [encoder setBuffer:rhi_buffer->GetResource() offset:rhi_buffer->GetOffset() atIndex:slot];
                buffer.resources.emplace_back(rhi_buffer->GetResource());
                break;
            }
            case RHIShaderResourceReflection::ResourceType::Texture2D:
            case RHIShaderResourceReflection::ResourceType::StorageImage2D: {
                auto *rhi_image = RHICast<MetalImage>(sub_resource);
                [encoder setTexture:rhi_image->GetResource() atIndex:slot];
                buffer.resources.emplace_back(rhi_image->GetResource());
                break;
            }
            case RHIShaderResourceReflection::ResourceType::Sampler: {
                auto *rhi_sampler = RHICast<MetalSampler>(sub_resource);
                [encoder setSamplerState:rhi_sampler->GetResource() atIndex:slot];
                break;
            }
            default:
                UnImplemented(resource->GetType());
            }
        }

        [buffer.buffer didModifyRange:NSMakeRange(0, argument_buffer_length)];
    }
}
#endif
} // namespace sparkle

#endif
