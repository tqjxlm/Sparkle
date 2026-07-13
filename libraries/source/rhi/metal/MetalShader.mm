#if FRAMEWORK_APPLE

#include "MetalShader.h"

#include "MetalContext.h"
#include "core/Exception.h"
#include "core/FileManager.h"
#include "core/Logger.h"

namespace sparkle
{
void MetalShader::Load()
{
    if (IsValid())
    {
        return;
    }

    auto path = shader_info_->GetPath() + ".metal";
    auto shader_data = FileManager::GetNativeFileManager()->ReadAsType<std::string>(Path::Resource(path));

    MTLCompileOptions *compile_option = [[MTLCompileOptions alloc] init];

    // slang-emitted [[vertex]]/[[fragment]]/[[kernel]] attributes need MSL 2.3+; lower versions fail on every shader
    compile_option.languageVersion = MTLLanguageVersion3_0;

    NSError *error;
    NSString *shader_source = [NSString stringWithCString:shader_data.c_str() encoding:NSASCIIStringEncoding];
    if (shader_source.length == 0)
    {
        Log(Error, "Failed to load shader file {}", path);
        DumpAndAbort();
    }

    library_ = [context->GetDevice() newLibraryWithSource:shader_source options:compile_option error:&error];

    if (!library_)
    {
        Log(Error, "Failed to load and compile shader library {}. error: {}", path,
            [error.localizedDescription UTF8String]);
        DumpAndAbort();
    }

    auto find_function = [&](const std::string &entry_point) -> bool {
        function_ = [library_ newFunctionWithName:[NSString stringWithUTF8String:entry_point.c_str()]];
        return function_ != nil;
    };

    // Slang emits exact non-reserved entry names. spirv-cross currently emits main0
    // for the ray-tracing SPIR-V path. Older Slang-generated Metal used main_0
    // after Apple's compiler renamed a reserved main entry.
    const auto &entry_point = shader_info_->GetEntryPoint();
    if (!find_function(entry_point))
    {
        find_function(entry_point + "_0");
    }
    if (!function_)
    {
        find_function(entry_point + "0");
    }
    if (!function_ && entry_point != "main")
    {
        find_function("main_0");
    }
    if (!function_ && entry_point != "main")
    {
        find_function("main0");
    }

    if (!function_)
    {
        Log(Error, "Failed to load shader function {}", path);
        DumpAndAbort();
    }

    SetDebugInfo(function_, GetName());

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
