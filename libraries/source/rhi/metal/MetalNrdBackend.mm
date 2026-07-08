#if FRAMEWORK_APPLE

#include "MetalNrdBackend.h"

#include "MetalContext.h"
#include "MetalImage.h"

#include "core/Exception.h"
#include "core/Logger.h"

#include <NRD.h>

namespace sparkle
{
namespace
{
MTLPixelFormat NrdFormatToMetal(uint32_t nrd_format)
{
    switch (static_cast<nrd::Format>(nrd_format))
    {
    case nrd::Format::R8_UNORM:
        return MTLPixelFormatR8Unorm;
    case nrd::Format::R8_UINT:
        return MTLPixelFormatR8Uint;
    case nrd::Format::RG8_UNORM:
        return MTLPixelFormatRG8Unorm;
    case nrd::Format::RGBA8_UNORM:
        return MTLPixelFormatRGBA8Unorm;
    case nrd::Format::RGBA8_SNORM:
        return MTLPixelFormatRGBA8Snorm;
    case nrd::Format::R16_UNORM:
        return MTLPixelFormatR16Unorm;
    case nrd::Format::R16_UINT:
        return MTLPixelFormatR16Uint;
    case nrd::Format::R16_SFLOAT:
        return MTLPixelFormatR16Float;
    case nrd::Format::RG16_SFLOAT:
        return MTLPixelFormatRG16Float;
    case nrd::Format::RGBA16_UNORM:
        return MTLPixelFormatRGBA16Unorm;
    case nrd::Format::RGBA16_SNORM:
        return MTLPixelFormatRGBA16Snorm;
    case nrd::Format::RGBA16_SFLOAT:
        return MTLPixelFormatRGBA16Float;
    case nrd::Format::R32_UINT:
        return MTLPixelFormatR32Uint;
    case nrd::Format::R32_SFLOAT:
        return MTLPixelFormatR32Float;
    case nrd::Format::RG32_SFLOAT:
        return MTLPixelFormatRG32Float;
    case nrd::Format::RGBA32_SFLOAT:
        return MTLPixelFormatRGBA32Float;
    case nrd::Format::R10_G10_B10_A2_UNORM:
        return MTLPixelFormatRGB10A2Unorm;
    case nrd::Format::R11_G11_B10_UFLOAT:
        return MTLPixelFormatRG11B10Float;
    case nrd::Format::R9_G9_B9_E5_UFLOAT:
        return MTLPixelFormatRGB9E5Float;
    default:
        ASSERT_F(false, "NRD: unmapped format {}", nrd_format);
        return MTLPixelFormatInvalid;
    }
}

id<MTLSamplerState> CreateNrdSampler(id<MTLDevice> device, uint32_t nrd_sampler)
{
    MTLSamplerDescriptor *descriptor = [[MTLSamplerDescriptor alloc] init];
    descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    const bool linear = static_cast<nrd::Sampler>(nrd_sampler) == nrd::Sampler::LINEAR_CLAMP;
    descriptor.minFilter = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    return [device newSamplerStateWithDescriptor:descriptor];
}
} // namespace

MetalNrdBackend::MetalNrdBackend(id<MTLDevice> device) : device_(device)
{
    compile_options_ = [[MTLCompileOptions alloc] init];
    compile_options_.languageVersion = MTLLanguageVersion3_0;
}

bool MetalNrdBackend::AddPipeline(const CookedPipeline &pipeline)
{
    NSError *error = nil;
    id<MTLLibrary> library =
        [device_ newLibraryWithSource:[NSString stringWithUTF8String:pipeline.msl_source.c_str()]
                              options:compile_options_
                                error:&error];
    if (!library)
    {
        Log(Error, "MetalNrdBackend: MSL compile failed: {}",
            error ? [error.localizedDescription UTF8String] : "unknown");
        return false;
    }

    id<MTLFunction> function = [library newFunctionWithName:[NSString stringWithUTF8String:pipeline.entry_point.c_str()]];
    if (!function)
    {
        Log(Error, "MetalNrdBackend: entry '{}' not found", pipeline.entry_point);
        return false;
    }

    id<MTLComputePipelineState> pso = [device_ newComputePipelineStateWithFunction:function error:&error];
    if (!pso)
    {
        Log(Error, "MetalNrdBackend: pipeline creation failed: {}",
            error ? [error.localizedDescription UTF8String] : "unknown");
        return false;
    }

    NrdPipeline nrd_pipeline;
    nrd_pipeline.pso = pso;
    nrd_pipeline.threads_per_group =
        MTLSizeMake(pipeline.threads_per_group[0], pipeline.threads_per_group[1], pipeline.threads_per_group[2]);
    nrd_pipeline.srv_texture_indices = pipeline.srv_texture_indices;
    nrd_pipeline.uav_texture_indices = pipeline.uav_texture_indices;
    nrd_pipeline.sampler_indices = pipeline.sampler_indices;
    nrd_pipeline.constant_buffer_index = pipeline.constant_buffer_index;

    Log(Info, "MetalNrdBackend: [{:2}] SRV={} UAV={} samplers={} cb_idx={}", pipelines_.size(),
        nrd_pipeline.srv_texture_indices.size(), nrd_pipeline.uav_texture_indices.size(),
        nrd_pipeline.sampler_indices.size(), nrd_pipeline.constant_buffer_index);

    pipelines_.push_back(std::move(nrd_pipeline));
    return true;
}

id<MTLTexture> MetalNrdBackend::CreatePoolTexture(const PoolTexture &desc, uint32_t width, uint32_t height)
{
    const uint32_t factor = desc.downsample_factor;
    const uint32_t w = (width + factor - 1) / factor;
    const uint32_t h = (height + factor - 1) / factor;

    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:NrdFormatToMetal(desc.format)
                                                           width:w
                                                          height:h
                                                       mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    descriptor.storageMode = MTLStorageModePrivate;
    return [device_ newTextureWithDescriptor:descriptor];
}

void MetalNrdBackend::AllocateResources(uint32_t width, uint32_t height, const PoolTexture *permanent,
                                        uint32_t permanent_count, const PoolTexture *transient,
                                        uint32_t transient_count, const uint32_t *samplers, uint32_t sampler_count,
                                        uint32_t constant_buffer_size)
{
    permanent_pool_.reserve(permanent_count);
    for (uint32_t i = 0; i < permanent_count; i++)
    {
        permanent_pool_.push_back(CreatePoolTexture(permanent[i], width, height));
    }

    transient_pool_.reserve(transient_count);
    for (uint32_t i = 0; i < transient_count; i++)
    {
        transient_pool_.push_back(CreatePoolTexture(transient[i], width, height));
    }

    samplers_.reserve(sampler_count);
    for (uint32_t i = 0; i < sampler_count; i++)
    {
        samplers_.push_back(CreateNrdSampler(device_, samplers[i]));
    }

    constant_slot_size_ = (constant_buffer_size + 255u) & ~255u;
    constant_slot_count_ = 256;
    constant_buffer_ = [device_ newBufferWithLength:(constant_slot_size_ * constant_slot_count_)
                                            options:MTLResourceStorageModeShared];

    Log(Info, "MetalNrdBackend: allocated pool {}+{} textures, {} samplers, cb {}B, at {}x{}", permanent_count,
        transient_count, sampler_count, constant_buffer_size, width, height);
}

void MetalNrdBackend::RunDispatches(const Dispatch *dispatches, uint32_t count)
{
    id<MTLCommandBuffer> command_buffer = context->GetCurrentCommandBuffer();
    ASSERT_F(command_buffer, "MetalNrdBackend: no active command buffer");

    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    SetDebugInfo(encoder, "NrdDispatches");

    for (uint32_t d = 0; d < count; d++)
    {
        const Dispatch &dispatch = dispatches[d];
        const NrdPipeline &pipeline = pipelines_[dispatch.pipeline_index];

        [encoder setComputePipelineState:pipeline.pso];

        if (pipeline.constant_buffer_index != ~0u && dispatch.constant_size > 0)
        {
            const uint32_t offset = constant_slot_cursor_ * constant_slot_size_;
            constant_slot_cursor_ = (constant_slot_cursor_ + 1) % constant_slot_count_;
            memcpy(static_cast<uint8_t *>(constant_buffer_.contents) + offset, dispatch.constant_data,
                   dispatch.constant_size);
            [encoder setBuffer:constant_buffer_ offset:offset atIndex:pipeline.constant_buffer_index];
        }

        for (uint32_t s = 0; s < samplers_.size(); s++)
        {
            if (s < pipeline.sampler_indices.size() && pipeline.sampler_indices[s] != ~0u)
            {
                [encoder setSamplerState:samplers_[s] atIndex:pipeline.sampler_indices[s]];
            }
        }

        uint32_t srv_register = 0;
        uint32_t uav_register = 0;
        for (uint32_t r = 0; r < dispatch.resource_count; r++)
        {
            const DispatchResource &resource = dispatch.resources[r];

            id<MTLTexture> texture = nil;
            switch (resource.source)
            {
            case DispatchResource::Source::PermanentPool:
                texture = permanent_pool_[resource.index_in_pool];
                break;
            case DispatchResource::Source::TransientPool:
                texture = transient_pool_[resource.index_in_pool];
                break;
            case DispatchResource::Source::User:
                texture = RHICast<MetalImage>(resource.user_image)->GetResource();
                break;
            }

            const auto &indices = resource.is_uav ? pipeline.uav_texture_indices : pipeline.srv_texture_indices;
            const uint32_t reg = resource.is_uav ? uav_register++ : srv_register++;
            if (reg < indices.size() && indices[reg] != ~0u)
            {
                [encoder setTexture:texture atIndex:indices[reg]];
            }
        }

        [encoder dispatchThreadgroups:MTLSizeMake(dispatch.grid_width, dispatch.grid_height, 1)
                threadsPerThreadgroup:pipeline.threads_per_group];
    }

    [encoder endEncoding];
}
} // namespace sparkle

#endif
