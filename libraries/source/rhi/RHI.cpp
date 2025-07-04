#include "rhi/RHI.h"

#if FRAMEWORK_APPLE
#include "rhi/MetalRHI.h"
#endif
#if ENABLE_VULKAN
#include "rhi/VulkanRHI.h"
#endif

#include "core/Exception.h"
#include "core/Profiler.h"
#include "io/Image.h"

namespace sparkle
{
#ifndef NDEBUG
std::unordered_set<size_t> RHIContext::deleted_resources_;
#endif

std::unique_ptr<RHIContext> RHIContext::CreateRHI(const RHIConfig &config)
{
    std::unique_ptr<RHIContext> context;

    switch (config.api_platform)
    {
    case RHIConfig::ApiPlatform::Vulkan:
#if ENABLE_VULKAN
        context = std::make_unique<VulkanRHI>(config);
        break;
#endif
    case RHIConfig::ApiPlatform::Metal:
#if FRAMEWORK_APPLE
        context = std::make_unique<MetalRHI>(config);
        break;
#endif
    case RHIConfig::ApiPlatform::None:
    default:
        UnImplemented(config.api_platform);
    }

    return context;
}

RHIContext::RHIContext(const RHIConfig &config) : config_(config)
{
}

void RHIContext::Cleanup()
{
    ReleaseRenderResources();

    // discard unfinished tasks, we won't need them any way
    end_of_render_tasks_.clear();

    buffer_manager_ = nullptr;

    FlushDeferredDeletions();

    CleanupInternal();

    CheckRHIResourceLeak();
}

bool RHIContext::InitRHI(NativeView *inWindow, std::string &error)
{
    view_ = inWindow;
    error = "unhandled error";

    buffer_manager_ = std::make_unique<RHIBufferManager>(this);

    return true;
}

RHIResourceRef<RHISampler> RHIContext::GetSampler(RHISampler::SamplerAttribute attribute)
{
    auto found = samplers_.find(attribute);
    if (found != samplers_.end())
    {
        return found->second;
    }

    auto sampler = CreateSampler(attribute, std::string("Sampler") + std::to_string(samplers_.size()));
    samplers_.insert_or_assign(attribute, sampler);

    return sampler;
}

RHIResourceRef<RHIImage> RHIContext::CreateTexture(const Image2D *image, const std::string &name)
{
    if (!image)
    {
        return nullptr;
    }

    RHIImage::Attribute attribute;
    attribute.format = image->GetFormat();
    attribute.width = image->GetWidth();
    attribute.height = image->GetHeight();
    attribute.usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::TransferDst;
    attribute.sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                         .filtering_method_min = RHISampler::FilteringMethod::Linear,
                         .filtering_method_mag = RHISampler::FilteringMethod::Linear,
                         .filtering_method_mipmap = RHISampler::FilteringMethod::Linear};

    auto rhi_image = CreateImage(attribute, name);
    rhi_image->Upload(image->GetRawData());

    return rhi_image;
}

RHIResourceRef<RHIImage> RHIContext::CreateTextureCube(const Image2DCube *image, const std::string &name)
{
    if (!image)
    {
        return nullptr;
    }

    RHIImage::Attribute attribute;
    attribute.format = image->GetFormat();
    attribute.width = image->GetWidth();
    attribute.height = image->GetHeight();
    attribute.usages = RHIImage::ImageUsage::Texture | RHIImage::ImageUsage::TransferDst;
    attribute.type = RHIImage::ImageType::Image2DCube;
    attribute.sampler = {.address_mode = RHISampler::SamplerAddressMode::Repeat,
                         .filtering_method_min = RHISampler::FilteringMethod::Linear,
                         .filtering_method_mag = RHISampler::FilteringMethod::Linear,
                         .filtering_method_mipmap = RHISampler::FilteringMethod::Linear};

    auto rhi_image = CreateImage(attribute, name);

    std::array<const uint8_t *, 6> data;
    for (auto i = 0u; i < Image2DCube::FaceId::Count; i++)
    {
        data[i] = image->GetFace(static_cast<Image2DCube::FaceId>(i)).GetRawData();
    }
    rhi_image->UploadFaces(data);

    return rhi_image;
}

void RHIContext::CheckRHIResourceLeak()
{
#ifndef NDEBUG
    // check for resource leak
    int leaked_count = 0;
    for (const auto &resource : registered_resources_)
    {
        if (!resource.expired())
        {
            const auto &rhi_resource = resource.lock();

            // weak_ptr::lock created an additional ref count, so the actual ref is use_count() - 1
            Log(Error, "Resource leak name {}. ref count {}. address {}", rhi_resource->GetName(),
                resource.use_count() - 1, reinterpret_cast<uint64_t>(rhi_resource.get()));
            Log(Warn, "Allocation stack: {}", rhi_resource->GetDebugStack());

            leaked_count++;
        }
    }

    ASSERT_EQUAL(leaked_count, 0);
#endif
}

void RHIContext::BeginFrame()
{
    frame_memory_.Reset();

    std::vector<std::function<void(void)>> tasks;
    std::swap(before_frame_tasks_, tasks);
    for (const auto &task : tasks)
    {
        task();
    }

    BeginFrameInternal();

    is_deleting_deferred_resources_ = true;
    deferred_deletion_[frame_index_].DeleteResources();
    is_deleting_deferred_resources_ = false;

    for (auto &task : end_of_render_tasks_[frame_index_])
    {
        task();
    }
    end_of_render_tasks_[frame_index_].clear();
}

void RHIContext::EndFrame()
{
    EndFrameInternal();

    if (!end_of_frame_tasks_.empty())
    {
        std::vector<std::function<void(void)>> tasks;
        swap(tasks, end_of_frame_tasks_);
        for (auto &task : tasks)
        {
            task();
        }
    }

    // next frame
    frame_index_ = (frame_index_ + 1) % max_frames_in_flight_;

    total_frame_++;
}

void RHIContext::EndRenderPass()
{
    ASSERT_F(current_render_pass_ != nullptr, "No active render pass!");

    EndRenderPassInternal();

    current_render_pass_ = nullptr;
}

void RHIContext::BeginRenderPass(const RHIResourceRef<RHIRenderPass> &pass)
{
    ASSERT_F(current_render_pass_ == nullptr, "Previous render pass not ended {}", current_render_pass_->GetName());
    ASSERT_F(current_compute_pass_ == nullptr, "Previous compute pass not ended {}", current_compute_pass_->GetName());

    current_render_pass_ = pass;

    BeginRenderPassInternal(pass);
}

void RHIContext::RecreateBuffer(RHIBuffer::Attribute attribute, const std::string &name,
                                RHIResourceRef<RHIBuffer> &in_out_existing_buffer)
{
    static constexpr size_t BaseSize = 128;

    size_t required_size = attribute.size;
    if (in_out_existing_buffer && in_out_existing_buffer->GetSize() >= required_size)
    {
        return;
    }

    // we enlarge the buffer size by a factor of 2 until it reaches the required_size capacity
    size_t buffer_size = in_out_existing_buffer ? in_out_existing_buffer->GetSize() : BaseSize;
    while (buffer_size < required_size)
    {
        buffer_size *= 2;
    }

    attribute.size = buffer_size;
    in_out_existing_buffer = CreateBuffer(attribute, name);
}

void RHIContext::BeginComputePass(const RHIResourceRef<RHIComputePass> &pass)
{
    ASSERT_F(current_compute_pass_ == nullptr, "Previous compute pass not ended {}", current_compute_pass_->GetName());
    ASSERT_F(current_render_pass_ == nullptr, "Previous render pass not ended {}", current_render_pass_->GetName());

    current_compute_pass_ = pass;

    BeginComputePassInternal(pass);
}

void RHIContext::EndComputePass(const RHIResourceRef<RHIComputePass> &pass)
{
    ASSERT(current_compute_pass_ == pass);

    current_compute_pass_ = nullptr;

    EndComputePassInternal(pass);
}

void RHIContext::DeferResourceDeletion(RHIResource *resource)
{
    ASSERT(resource);

    // if we are in the middle of deleting deferred resources, do not lock because it will cause deadlock
    deferred_deletion_[frame_index_].PushResource(resource, !is_deleting_deferred_resources_);
}

void RHIContext::DeferredDeletion::DeleteResources()
{
    std::lock_guard<std::mutex> lock(*mutex);

    // to handle recursive deletion, we poll resources until it is empty
    while (!resources.empty())
    {
        std::vector<RHIResource *> resource_to_delete;
        std::swap(resource_to_delete, resources);
        for (auto &resource : resource_to_delete)
        {
#ifndef NDEBUG
            deleted_resources_.emplace(resource->GetId());
#endif
            delete resource;
        }
    }

    ASSERT(resources.empty());

#ifndef NDEBUG
    duplication_guard.clear();
#endif
}

void RHIContext::DeferredDeletion::PushResource(RHIResource *resource, bool should_lock)
{
    if (should_lock)
    {
        mutex->lock();
    }

    resources.emplace_back(resource);

#ifndef NDEBUG
    auto name = resource->GetName();

    ASSERT_F(!duplication_guard.contains(resource), "double deletion of resource {}", name);
    duplication_guard.insert(resource);
#endif

    if (should_lock)
    {
        mutex->unlock();
    }
}

RHIContext::DeferredDeletion::DeferredDeletion() : mutex(std::make_unique<std::mutex>())
{
}

RHIContext::DeferredDeletion::~DeferredDeletion()
{
    DeleteResources();
}

RHIContext::DeferredDeletion::DeferredDeletion(DeferredDeletion &&other) noexcept
{
    resources = std::move(other.resources);
    mutex = std::move(other.mutex);
#ifndef NDEBUG
    duplication_guard = std::move(other.duplication_guard);
#endif
}

void RHIContext::SetMaxFramesInFlight(unsigned max_frames_in_flight)
{
    max_frames_in_flight_ = max_frames_in_flight;
    end_of_render_tasks_.resize(max_frames_in_flight_);
    deferred_deletion_.resize(max_frames_in_flight_);
    frame_stats_.resize(max_frames_in_flight_);
}

void RHIContext::FlushDeferredDeletions()
{
    PROFILE_SCOPE("RHIContext::FlushDeferredDeletions");

    is_deleting_deferred_resources_ = true;

    deferred_deletion_.clear();
    deferred_deletion_.resize(max_frames_in_flight_);

    is_deleting_deferred_resources_ = false;
}

void RHIContext::ReleaseRenderResources()
{
    WaitForDeviceIdle();

    back_buffer_rt_ = nullptr;
    current_render_pass_ = nullptr;
    ui_handler_instance_ = nullptr;

    samplers_.clear();
    dummy_textures_.clear();
}

RHIResourceRef<RHIImage> RHIContext::GetOrCreateDummyTexture(RHIImage::Attribute attribute)
{
    // we don't care about the content of a dummy texture. it is just a place holder.
    attribute.initial_layout = RHIImageLayout::PreInitialized;
    attribute.width = attribute.height = 1;

    auto hash = attribute.GetHashForShader();

    auto found = dummy_textures_.find(hash);
    if (found != dummy_textures_.end())
    {
        return found->second;
    }

    auto texture = CreateImage(attribute, std::format("DummyTexture_{}", hash));
    texture->Transition({.target_layout = RHIImageLayout::Read,
                         .after_stage = RHIPipelineStage::Top,
                         .before_stage = RHIPipelineStage::Bottom});
    dummy_textures_.emplace(hash, texture);

    return texture;
}
} // namespace sparkle
