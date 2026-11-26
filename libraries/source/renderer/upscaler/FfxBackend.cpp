#include "renderer/upscaler/FfxBackend.h"

#include "core/Logger.h"
#include "rhi/RHI.h"
#include "rhi/RHIBuffer.h"
#include "rhi/RHICommandContext.h"
#include "rhi/RHIComputePass.h"
#include "rhi/RHIImage.h"
#include "rhi/RHIImageView.h"
#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIShader.h"

#include <ffx_backends.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <string>

// the FSR3 upscaler's memory query reaches this; the engine does not query it
FfxErrorCode GetResourceSizeFromDescription(FfxDevice /*device*/, const FfxCreateResourceDescription * /*description*/,
                                            uint64_t * /*sizeInBytes*/, uint64_t * /*alignment*/)
{
    return FFX_ERROR_INVALID_ARGUMENT;
}

namespace sparkle
{
namespace
{
using ResourceType = RHIShaderResourceReflection::ResourceType;

class FfxClearFloatShader : public RHIShaderInfo
{
    REGISTGER_SHADER(FfxClearFloatShader, RHIShaderStage::Compute, "shaders/fsr/ffx_clear_float.cs.slang",
                     "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(target, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE
};

class FfxClearUintShader : public RHIShaderInfo
{
    REGISTGER_SHADER(FfxClearUintShader, RHIShaderStage::Compute, "shaders/fsr/ffx_clear_uint.cs.slang", "shader_main")

    BEGIN_SHADER_RESOURCE_TABLE(RHIShaderResourceTable)

    USE_SHADER_RESOURCE(ubo, RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
    USE_SHADER_RESOURCE(target, RHIShaderResourceReflection::ResourceType::StorageImage2D)

    END_SHADER_RESOURCE_TABLE
};

const Vector3UInt ClearThreadGroupSize{8u, 8u, 1u};

// the RHI has no 8-bit dual channel, 16-bit snorm, packed 11-11-10 or two-channel 32-bit formats; those
// resources use the closest wider float format, which holds every value they would
PixelFormat ToPixelFormat(uint32_t format)
{
    switch (format)
    {
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT:
    case FFX_API_SURFACE_FORMAT_R32G32_FLOAT:
        return PixelFormat::RGBAFloat;
    case FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT:
    case FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT:
        return PixelFormat::RGBAFloat16;
    case FFX_API_SURFACE_FORMAT_R16G16_FLOAT:
    case FFX_API_SURFACE_FORMAT_R8G8_UNORM:
        return PixelFormat::RGFloat16;
    case FFX_API_SURFACE_FORMAT_R16_FLOAT:
    case FFX_API_SURFACE_FORMAT_R16_SNORM:
        return PixelFormat::R16Float;
    case FFX_API_SURFACE_FORMAT_R8_UNORM:
        return PixelFormat::R8Unorm;
    case FFX_API_SURFACE_FORMAT_R32_FLOAT:
        return PixelFormat::R32Float;
    case FFX_API_SURFACE_FORMAT_R32_UINT:
        return PixelFormat::R32UInt;
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM:
        return PixelFormat::R8G8B8A8Unorm;
    case FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM:
        return PixelFormat::R10G10B10A2Unorm;
    default:
        return PixelFormat::Count;
    }
}

uint32_t ToFfxFormat(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::RGBAFloat:
        return FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT;
    case PixelFormat::RGBAFloat16:
        return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case PixelFormat::RGFloat16:
        return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case PixelFormat::R16Float:
        return FFX_API_SURFACE_FORMAT_R16_FLOAT;
    case PixelFormat::R8Unorm:
        return FFX_API_SURFACE_FORMAT_R8_UNORM;
    case PixelFormat::R32Float:
        return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case PixelFormat::R32UInt:
        return FFX_API_SURFACE_FORMAT_R32_UINT;
    case PixelFormat::R8G8B8A8Unorm:
        return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case PixelFormat::R10G10B10A2Unorm:
        return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    default:
        return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

// the channels of one texel of an effect format whose resources use a wider format
Vector4 DecodeTexel(uint32_t format, const uint8_t *texel)
{
    Vector4 value = Vector4::Zero();
    switch (format)
    {
    case FFX_API_SURFACE_FORMAT_R8G8_UNORM:
        value.x() = static_cast<float>(texel[0]) / 255.f;
        value.y() = static_cast<float>(texel[1]) / 255.f;
        break;
    case FFX_API_SURFACE_FORMAT_R16_SNORM: {
        int16_t snorm = 0;
        std::memcpy(&snorm, texel, sizeof(snorm));
        value.x() = std::max(static_cast<float>(snorm) / 32767.f, -1.f);
        break;
    }
    case FFX_API_SURFACE_FORMAT_R32G32_FLOAT:
        std::memcpy(value.data(), texel, 2 * sizeof(float));
        break;
    default:
        UnImplemented(format);
        break;
    }
    return value;
}

void EncodeTexel(PixelFormat format, const Vector4 &value, uint8_t *texel)
{
    switch (format)
    {
    case PixelFormat::RGBAFloat:
        std::memcpy(texel, value.data(), 4 * sizeof(float));
        break;
    case PixelFormat::RGFloat16:
    case PixelFormat::R16Float:
        for (unsigned channel = 0; channel < GetPixelSize(format) / sizeof(Half); channel++)
        {
            const Half half(value[channel]);
            std::memcpy(texel + channel * sizeof(Half), &half, sizeof(Half));
        }
        break;
    default:
        UnImplemented(format);
        break;
    }
}

// the initial texels of a created resource, in the format the RHI image has
std::vector<uint8_t> GetInitialTexels(const FfxCreateResourceDescription &description, PixelFormat format)
{
    const auto &init = description.initData;
    std::vector<uint8_t> source(init.size);
    if (init.type == FFX_RESOURCE_INIT_DATA_TYPE_BUFFER)
    {
        std::memcpy(source.data(), init.buffer, init.size);
    }
    else
    {
        std::ranges::fill(source, init.value);
    }

    const auto &resource = description.resourceDescription;
    if (ToFfxFormat(format) == resource.format)
    {
        return source;
    }

    const size_t texel_count = static_cast<size_t>(resource.width) * resource.height;
    const size_t source_texel_size = init.size / texel_count;
    std::vector<uint8_t> converted(texel_count * GetPixelSize(format));
    for (size_t texel = 0; texel < texel_count; texel++)
    {
        EncodeTexel(format, DecodeTexel(resource.format, source.data() + texel * source_texel_size),
                    converted.data() + texel * GetPixelSize(format));
    }
    return converted;
}

std::string Narrow(const wchar_t *name)
{
    std::string narrow;
    for (; *name != L'\0'; name++)
    {
        narrow.push_back(static_cast<char>(*name));
    }
    return narrow;
}

// reports one binding of a pipeline to the effect, which matches it to its resources by name
void Report(FfxResourceBinding &binding, std::string_view name, size_t slot)
{
    binding = {};
    binding.slotIndex = static_cast<uint32_t>(slot);
    const size_t length = std::min(name.size(), static_cast<size_t>(FFX_RESOURCE_NAME_SIZE - 1));
    std::copy_n(name.begin(), length, binding.name);
}

template <class Shader>
void BindClear(RHIPipelineState &state, const RHIResourceRef<RHIBuffer> &value,
               const RHIResourceRef<RHIImageView> &target)
{
    auto *resources = state.GetShaderResource<Shader>();
    resources->ubo().BindResource(value);
    resources->target().BindResource(target);
}

// a binding the compiled shader has no slot for, because the compiler removed it, takes no resource
template <ResourceType Type, class T> void Bind(RHIShaderResourceBinding *binding, const RHIResourceRef<T> &resource)
{
    if (binding->GetReflection()->slot != UINT_MAX)
    {
        static_cast<RHIShaderResourceBindingTyped<Type, false> *>(binding)->BindResource(resource);
    }
}

void ToLayout(RHICommandContext &command_context, RHIImage *image, RHIImageLayout layout, unsigned base_mip,
              unsigned mip_count)
{
    image->Transition(command_context, {.target_layout = layout,
                                        .after_stage = RHIPipelineStage::ComputeShader,
                                        .before_stage = RHIPipelineStage::ComputeShader,
                                        .base_mip = base_mip,
                                        .mip_count = mip_count});
}
} // namespace

FfxBackend::FfxBackend(RHIContext *rhi) : rhi_(rhi), resources_(1)
{
    interface_.device = this;
    interface_.fpCreateBackendContext = [](FfxInterface *, FfxEffect, FfxEffectBindlessConfig *,
                                           FfxUInt32 *effect_context_id) -> FfxErrorCode {
        *effect_context_id = 0;
        return FFX_OK;
    };
    interface_.fpDestroyBackendContext = [](FfxInterface *, FfxUInt32) -> FfxErrorCode { return FFX_OK; };
    // 32-wide waves without 16-bit math select the passes the engine compiles (shaders/include/fsr3upscaler.h.slang)
    interface_.fpGetDeviceCapabilities = [](FfxInterface *, FfxDeviceCapabilities *capabilities) -> FfxErrorCode {
        *capabilities = {};
        capabilities->maximumSupportedShaderModel = FFX_SHADER_MODEL_6_0;
        capabilities->waveLaneCountMin = 32;
        capabilities->waveLaneCountMax = 32;
        return FFX_OK;
    };
    interface_.fpCreateResource = CreateResource;
    interface_.fpRegisterResource = RegisterResource;
    interface_.fpGetResource = GetApiResource;
    interface_.fpUnregisterResources = UnregisterResources;
    interface_.fpGetResourceDescription = GetResourceDescription;
    interface_.fpDestroyResource = DestroyResource;
    interface_.fpStageConstantBufferDataFunc = StageConstantBufferData;
    interface_.fpCreatePipeline = CreatePipeline;
    interface_.fpDestroyPipeline = DestroyPipeline;
    interface_.fpScheduleGpuJob = ScheduleGpuJob;
    interface_.fpExecuteGpuJobs = ExecuteGpuJobs;
}

FfxBackend::~FfxBackend() = default;

FfxBackend &FfxBackend::Get(FfxInterface *backend_interface)
{
    return *static_cast<FfxBackend *>(backend_interface->device);
}

FfxApiResource FfxBackend::Wrap(RHIImage *image)
{
    const auto &attributes = image->GetAttributes();
    FfxApiResource resource{};
    resource.resource = image;
    resource.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    resource.description.format = ToFfxFormat(attributes.format);
    resource.description.width = attributes.width;
    resource.description.height = attributes.height;
    resource.description.depth = 1;
    resource.description.mipCount = attributes.mip_levels;
    resource.description.usage =
        (attributes.usages & RHIImage::ImageUsage::UAV) ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    return resource;
}

FfxBackend::Resource &FfxBackend::Find(FfxResourceInternal resource)
{
    ASSERT(resource.internalIndex >= 0 && static_cast<size_t>(resource.internalIndex) < resources_.size());
    return resources_[static_cast<size_t>(resource.internalIndex)];
}

FfxErrorCode FfxBackend::CreateResource(FfxInterface *backend_interface,
                                        const FfxCreateResourceDescription *description,
                                        FfxUInt32 /*effect_context_id*/, FfxResourceInternal *out_resource)
{
    auto &self = Get(backend_interface);
    ASSERT_F(self.resources_.size() == self.created_resource_count_, "FfxBackend: a resource is created mid-dispatch");

    auto image = self.CreateImage(*description);
    if (!image)
    {
        return FFX_ERROR_INVALID_ARGUMENT;
    }

    out_resource->internalIndex = static_cast<int32_t>(self.resources_.size());
    self.resources_.push_back({.owned = image, .image = image.get(), .description = description->resourceDescription});
    self.created_resource_count_ = self.resources_.size();
    return FFX_OK;
}

RHIResourceRef<RHIImage> FfxBackend::CreateImage(const FfxCreateResourceDescription &description) const
{
    const auto &resource = description.resourceDescription;
    const PixelFormat format = ToPixelFormat(resource.format);
    const std::string name = Narrow(description.name);
    if (resource.type != FFX_API_RESOURCE_TYPE_TEXTURE2D || format == PixelFormat::Count)
    {
        Log(Error, "FfxBackend: {} has an unsupported type {} or format {}", name, resource.type, resource.format);
        return nullptr;
    }

    const bool initialized = description.initData.type == FFX_RESOURCE_INIT_DATA_TYPE_BUFFER ||
                             description.initData.type == FFX_RESOURCE_INIT_DATA_TYPE_VALUE;
    const auto mip_levels = resource.mipCount > 0
                                ? resource.mipCount
                                : static_cast<uint32_t>(std::bit_width(std::max(resource.width, resource.height)));
    const auto usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::UAV;

    auto image = rhi_->CreateImage(
        {
            .format = format,
            .width = resource.width,
            .height = resource.height,
            .usages = initialized ? usages | RHIImage::ImageUsage::TransferDst : usages,
            .memory_properties = RHIMemoryProperty::DeviceLocal,
            .mip_levels = static_cast<uint8_t>(mip_levels),
            .msaa_samples = 1,
        },
        name);
    if (initialized)
    {
        image->Upload(*rhi_->GetCommandContext(), GetInitialTexels(description, format).data());
    }
    return image;
}

FfxErrorCode FfxBackend::RegisterResource(FfxInterface *backend_interface, const FfxApiResource *resource,
                                          FfxUInt32 /*effect_context_id*/, FfxResourceInternal *out_resource)
{
    auto &self = Get(backend_interface);
    if (!resource->resource)
    {
        out_resource->internalIndex = 0;
        return FFX_OK;
    }

    out_resource->internalIndex = static_cast<int32_t>(self.resources_.size());
    self.resources_.push_back(
        {.owned = nullptr, .image = static_cast<RHIImage *>(resource->resource), .description = resource->description});
    return FFX_OK;
}

FfxApiResource FfxBackend::GetApiResource(FfxInterface *backend_interface, FfxResourceInternal resource)
{
    const Resource &entry = Get(backend_interface).Find(resource);
    return {.resource = entry.image, .description = entry.description, .state = 0};
}

FfxErrorCode FfxBackend::UnregisterResources(FfxInterface *backend_interface, FfxCommandList /*command_list*/,
                                             FfxUInt32 /*effect_context_id*/)
{
    auto &self = Get(backend_interface);
    self.resources_.resize(self.created_resource_count_);
    return FFX_OK;
}

FfxApiResourceDescription FfxBackend::GetResourceDescription(FfxInterface *backend_interface,
                                                             FfxResourceInternal resource)
{
    return Get(backend_interface).Find(resource).description;
}

FfxErrorCode FfxBackend::DestroyResource(FfxInterface *backend_interface, FfxResourceInternal resource,
                                         FfxUInt32 /*effect_context_id*/)
{
    auto &self = Get(backend_interface);
    if (resource.internalIndex > 0)
    {
        auto &entry = self.Find(resource);
        if (entry.image)
        {
            std::erase_if(self.clear_pipelines_,
                          [id = entry.image->GetId()](const auto &clear) { return std::get<0>(clear.first) == id; });
        }
        entry = {};
    }
    return FFX_OK;
}

FfxErrorCode FfxBackend::StageConstantBufferData(FfxInterface *backend_interface, void *data, FfxUInt32 size,
                                                 FfxConstantBuffer *constant_buffer)
{
    auto &staged = Get(backend_interface).staged_constants_.emplace_back(size / sizeof(uint32_t));
    std::memcpy(staged.data(), data, staged.size() * sizeof(uint32_t));
    constant_buffer->data = staged.data();
    constant_buffer->num32BitEntries = static_cast<uint32_t>(staged.size());
    return FFX_OK;
}

FfxErrorCode FfxBackend::CreatePipeline(FfxInterface *backend_interface, FfxShaderBlob *shader_blob,
                                        const FfxPipelineDescription *description, FfxUInt32 /*effect_context_id*/,
                                        FfxPipelineState *out_pipeline)
{
    auto &self = Get(backend_interface);
    out_pipeline->pipeline = nullptr;
    out_pipeline->srvTextureCount = 0;
    out_pipeline->uavTextureCount = 0;
    out_pipeline->constCount = 0;

    const auto *source = reinterpret_cast<const FfxShaderSource *>(shader_blob->data);
    if (!source)
    {
        return FFX_OK;
    }

    const std::string name = Narrow(description->name);
    auto pipeline = std::make_unique<Pipeline>();
    pipeline->shader = source->create(self.rhi_);
    pipeline->state = self.rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, name);
    pipeline->state->SetShader<RHIShaderStage::Compute>(pipeline->shader);
    pipeline->state->Compile();
    pipeline->pass = self.rhi_->CreateComputePass(name, false);
    pipeline->thread_group_size = source->thread_group_size;

    for (auto *binding :
         pipeline->state->GetResourceTables()[static_cast<size_t>(RHIShaderStage::Compute)]->GetBindings())
    {
        const std::string_view binding_name = binding->GetReflection()->name;
        switch (binding->GetType())
        {
        case ResourceType::Texture2D:
            Report(out_pipeline->srvTextureBindings[pipeline->srv_textures.size()], binding_name,
                   pipeline->srv_textures.size());
            pipeline->srv_textures.push_back(binding);
            break;
        case ResourceType::StorageImage2D:
            Report(out_pipeline->uavTextureBindings[pipeline->uav_textures.size()], binding_name,
                   pipeline->uav_textures.size());
            pipeline->uav_textures.push_back(binding);
            break;
        case ResourceType::DynamicUniformBuffer:
            Report(out_pipeline->constantBufferBindings[pipeline->constant_buffer_bindings.size()], binding_name,
                   pipeline->constant_buffer_bindings.size());
            pipeline->constant_buffer_bindings.push_back(binding);
            pipeline->constant_buffers.emplace_back();
            break;
        case ResourceType::Sampler: {
            const auto filter = binding_name == "s_PointClamp" ? RHISampler::FilteringMethod::Nearest
                                                               : RHISampler::FilteringMethod::Linear;
            Bind<ResourceType::Sampler>(binding, self.rhi_->GetSampler({
                                                     .address_mode = RHISampler::SamplerAddressMode::ClampToEdge,
                                                     .filtering_method_min = filter,
                                                     .filtering_method_mag = filter,
                                                     .filtering_method_mipmap = filter,
                                                     .max_lod = RHISampler::SamplerAttribute::UnclampedLod,
                                                     .enable_anisotropy = false,
                                                 }));
            break;
        }
        default:
            Log(Error, "FfxBackend: {} declares {} of an unsupported type", name, binding_name);
            return FFX_ERROR_INVALID_ARGUMENT;
        }
    }

    out_pipeline->srvTextureCount = static_cast<uint32_t>(pipeline->srv_textures.size());
    out_pipeline->uavTextureCount = static_cast<uint32_t>(pipeline->uav_textures.size());
    out_pipeline->constCount = static_cast<uint32_t>(pipeline->constant_buffer_bindings.size());
    out_pipeline->pipeline = pipeline.get();
    self.pipelines_.push_back(std::move(pipeline));
    return FFX_OK;
}

FfxErrorCode FfxBackend::DestroyPipeline(FfxInterface *backend_interface, FfxPipelineState *pipeline,
                                         FfxUInt32 /*effect_context_id*/)
{
    auto &self = Get(backend_interface);
    std::erase_if(self.pipelines_, [handle = pipeline->pipeline](const auto &owned) { return owned.get() == handle; });
    pipeline->pipeline = nullptr;
    return FFX_OK;
}

FfxErrorCode FfxBackend::ScheduleGpuJob(FfxInterface *backend_interface, const FfxGpuJobDescription *job)
{
    Get(backend_interface).jobs_.push_back(*job);
    return FFX_OK;
}

FfxErrorCode FfxBackend::ExecuteGpuJobs(FfxInterface *backend_interface, FfxCommandList command_list,
                                        FfxUInt32 /*effect_context_id*/)
{
    auto &self = Get(backend_interface);
    auto &command_context = *static_cast<RHICommandContext *>(command_list);
    for (const auto &job : self.jobs_)
    {
        switch (job.jobType)
        {
        case FFX_GPU_JOB_COMPUTE:
            self.Dispatch(command_context, job.computeJobDescriptor);
            break;
        case FFX_GPU_JOB_CLEAR_FLOAT:
            self.Clear(command_context, job.clearJobDescriptor);
            break;
        // every job transitions the images it touches itself
        case FFX_GPU_JOB_BARRIER:
        case FFX_GPU_JOB_DISCARD:
            break;
        default:
            Log(Error, "FfxBackend: {} is an unsupported job type {}", Narrow(job.jobLabel),
                static_cast<uint32_t>(job.jobType));
            self.healthy_ = false;
            break;
        }
    }
    self.jobs_.clear();
    self.staged_constants_.clear();
    return self.healthy_ ? FFX_OK : FFX_ERROR_BACKEND_API_ERROR;
}

void FfxBackend::Dispatch(RHICommandContext &command_context, const FfxComputeJobDescription &job)
{
    auto *pipeline = static_cast<Pipeline *>(job.pipeline->pipeline);
    if (!pipeline)
    {
        Log(Error, "FfxBackend: {} has no engine shader", Narrow(job.pipeline->name));
        healthy_ = false;
        return;
    }

    for (size_t index = 0; index < pipeline->srv_textures.size(); index++)
    {
        RHIImage *image = Find(job.srvTextures[index].resource).image;
        ToLayout(command_context, image, RHIImageLayout::Read, 0, 0);
        Bind<ResourceType::Texture2D>(pipeline->srv_textures[index], image->GetDefaultView(rhi_));
    }

    for (size_t index = 0; index < pipeline->uav_textures.size(); index++)
    {
        const auto &uav = job.uavTextures[index];
        RHIImage *image = Find(uav.resource).image;
        ToLayout(command_context, image, RHIImageLayout::StorageWrite, uav.mip, 1);
        Bind<ResourceType::StorageImage2D>(pipeline->uav_textures[index],
                                           image->GetView(rhi_, {.base_mip_level = uav.mip, .mip_level_count = 1}));
    }

    for (size_t index = 0; index < pipeline->constant_buffers.size(); index++)
    {
        const FfxConstantBuffer &constants = job.cbs[index];
        auto &buffer = pipeline->constant_buffers[index];
        if (!buffer)
        {
            buffer = rhi_->CreateBuffer({.size = constants.num32BitEntries * sizeof(uint32_t),
                                         .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                         .mem_properties = RHIMemoryProperty::None,
                                         .is_dynamic = true},
                                        pipeline->pass->GetName() + "Constants");
            Bind<ResourceType::DynamicUniformBuffer>(pipeline->constant_buffer_bindings[index], buffer);
        }
        buffer->Upload(rhi_, constants.data);
    }

    const auto &group = pipeline->thread_group_size;
    command_context.BeginComputePass(pipeline->pass);
    command_context.DispatchCompute(
        pipeline->state, {job.dimensions[0] * group.x(), job.dimensions[1] * group.y(), job.dimensions[2] * group.z()},
        group);
    command_context.EndComputePass(pipeline->pass);
}

void FfxBackend::Clear(RHICommandContext &command_context, const FfxClearFloatJobDescription &job)
{
    RHIImage *image = Find(job.target).image;
    const std::array value{job.color[0], job.color[1], job.color[2], job.color[3]};
    for (uint32_t mip = 0; mip < image->GetAttributes().mip_levels; mip++)
    {
        ClearPipeline &clear = GetClearPipeline(image, mip, value);
        clear.value->Upload(rhi_, value.data());
        ToLayout(command_context, image, RHIImageLayout::StorageWrite, mip, 1);
        command_context.BeginComputePass(clear.pass);
        command_context.DispatchCompute(clear.state, {image->GetWidth(mip), image->GetHeight(mip), 1u},
                                        ClearThreadGroupSize);
        command_context.EndComputePass(clear.pass);
    }
}

FfxBackend::ClearPipeline &FfxBackend::GetClearPipeline(RHIImage *image, uint32_t mip,
                                                        const std::array<float, 4> &value)
{
    auto [entry, created] = clear_pipelines_.try_emplace({image->GetId(), mip, value});
    ClearPipeline &clear = entry->second;
    if (!created)
    {
        return clear;
    }

    const bool integer = image->GetAttributes().format == PixelFormat::R32UInt;
    const std::string name = image->GetName() + "Clear" + std::to_string(mip);
    clear.state = rhi_->CreatePipelineState(RHIPipelineState::PipelineType::Compute, name);
    clear.state->SetShader<RHIShaderStage::Compute>(integer ? rhi_->CreateShader<FfxClearUintShader>()
                                                            : rhi_->CreateShader<FfxClearFloatShader>());
    clear.state->Compile();
    clear.pass = rhi_->CreateComputePass(name, false);
    clear.value = rhi_->CreateBuffer({.size = sizeof(float) * 4,
                                      .usages = RHIBuffer::BufferUsage::UniformBuffer,
                                      .mem_properties = RHIMemoryProperty::None,
                                      .is_dynamic = true},
                                     name + "Value");

    const auto target = image->GetView(rhi_, {.base_mip_level = mip, .mip_level_count = 1});
    if (integer)
    {
        BindClear<FfxClearUintShader>(*clear.state, clear.value, target);
    }
    else
    {
        BindClear<FfxClearFloatShader>(*clear.state, clear.value, target);
    }
    return clear;
}
} // namespace sparkle
