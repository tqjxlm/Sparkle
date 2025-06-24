#pragma once

#include "core/Exception.h"
#include "core/StackMemoryAllocator.h"
#include "core/math/Types.h"
#include "rhi/RHIBuffer.h"
#include "rhi/RHIComputePass.h"
#include "rhi/RHIConfig.h"
#include "rhi/RHIImage.h"
#include "rhi/RHIPIpelineState.h"
#include "rhi/RHIRayTracing.h"
#include "rhi/RHIRenderPass.h"
#include "rhi/RHIRenderTarget.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceArray.h"
#include "rhi/RHIShader.h"
#include "rhi/RHITimer.h"
#include "rhi/RHIUiHandler.h"

namespace sparkle
{
class NativeView;
class MeshRenderProxy;
class Image2D;
class Image2DCube;

class RHIContext
{
public:
    explicit RHIContext(const RHIConfig &config);

    virtual ~RHIContext() = default;

    static std::unique_ptr<RHIContext> CreateRHI(const RHIConfig &config);

    NativeView *GetHardwareInterface()
    {
        return view_;
    }

    [[nodiscard]] NativeView *GetHardwareInterface() const
    {
        return view_;
    }

    [[nodiscard]] bool IsInitialized() const
    {
        return initialization_success_;
    }

    [[nodiscard]] bool IsBackBufferDirty() const
    {
        return back_buffer_dirty_;
    }

    [[nodiscard]] uint64_t GetRenderedFrameCount() const
    {
        return total_frame_;
    }

    [[nodiscard]] unsigned GetMaxFramesInFlight() const
    {
        return max_frames_in_flight_;
    }

    [[nodiscard]] unsigned GetFrameIndex() const
    {
        return frame_index_;
    }

    void SetMaxFramesInFlight(unsigned max_frames_in_flight);

    RHIResourceRef<RHIRenderTarget> GetBackBufferRenderTarget()
    {
        ASSERT_F(back_buffer_rt_, "Back buffer render target not initialized");
        return back_buffer_rt_;
    }

    void RecreateFrameBuffer(int /*width*/, int /*height*/)
    {
        frame_buffer_resized_ = true;
    }

    [[nodiscard]] const RHIConfig &GetConfig() const
    {
        return config_;
    }

    [[nodiscard]] RHIBufferManager *GetBufferManager() const
    {
        return buffer_manager_.get();
    }

    virtual bool InitRHI(NativeView *inWindow, std::string &error);

    virtual void CaptureNextFrames(int /*count*/)
    {
        UnImplemented();
    }

    void BeginFrame();

    void EndFrame();

    void BeginRenderPass(const RHIResourceRef<RHIRenderPass> &pass);

    void EndRenderPass();

    void BeginComputePass(const RHIResourceRef<RHIComputePass> &pass);

    void EndComputePass(const RHIResourceRef<RHIComputePass> &pass);

    void Cleanup();

    template <class T> RHIResourceRef<RHIShader> CreateShader()
    {
        return CreateShader(T::GetShaderInfo());
    }

    virtual void InitRenderResources() = 0;
    virtual bool SupportsHardwareRayTracing() = 0;

    virtual void BeginCommandBuffer() = 0;
    virtual void SubmitCommandBuffer() = 0;

    virtual void WaitForDeviceIdle() = 0;

    virtual void DestroySurface()
    {
    }

    virtual bool RecreateSurface() = 0;

    virtual void RecreateSwapChain() = 0;

    virtual void ReleaseRenderResources();

    virtual void NextSubpass() = 0;

    virtual void DrawMesh(const RHIResourceRef<RHIPipelineState> &pipeline_state, const DrawArgs &draw_args) = 0;
    virtual void DispatchCompute(const RHIResourceRef<RHIPipelineState> &pipeline, Vector3UInt total_threads,
                                 Vector3UInt thread_per_group) = 0;

    virtual RHIResourceRef<RHIResourceArray> CreateResourceArray(RHIShaderResourceReflection::ResourceType type,
                                                                 unsigned capacity, const std::string &name) = 0;

    RHIResourceRef<RHIResourceArray> CreateBindlessResourceArray(RHIShaderResourceReflection::ResourceType type,
                                                                 const std::string &name)
    {
        return CreateResourceArray(type, RHIShaderResourceBinding::MaxBindlessResources, name);
    }

    virtual RHIResourceRef<RHIRenderTarget> CreateBackBufferRenderTarget(const RHIRenderTarget::Attribute &attribute,
                                                                         const RHIResourceRef<RHIImage> &depth_image,
                                                                         const std::string &name) = 0;

    virtual RHIResourceRef<RHIRenderTarget> CreateRenderTarget(const RHIRenderTarget::Attribute &attribute,
                                                               const RHIRenderTarget::ColorImageArray &color_images,
                                                               const RHIResourceRef<RHIImage> &depth_image,
                                                               const std::string &name) = 0;

    RHIResourceRef<RHIRenderTarget> CreateRenderTarget(const RHIRenderTarget::Attribute &attribute,
                                                       const RHIResourceRef<RHIImage> &color_image,
                                                       const RHIResourceRef<RHIImage> &depth_image,
                                                       const std::string &name)
    {
        return CreateRenderTarget(attribute, RHIRenderTarget::ColorImageArray{color_image}, depth_image, name);
    }

    virtual RHIResourceRef<RHIRenderPass> CreateRenderPass(const RHIRenderPass::Attribute &attribute,
                                                           const RHIResourceRef<RHIRenderTarget> &rt,
                                                           const std::string &name) = 0;

    virtual RHIResourceRef<RHIPipelineState> CreatePipelineState(RHIPipelineState::PipelineType type,
                                                                 const std::string &name) = 0;

    virtual RHIResourceRef<RHIBuffer> CreateBuffer(const RHIBuffer::Attribute &attribute, const std::string &name) = 0;

    void RecreateBuffer(RHIBuffer::Attribute attribute, const std::string &name,
                        RHIResourceRef<RHIBuffer> &in_out_existing_buffer);

    virtual RHIResourceRef<RHIImage> CreateImage(const RHIImage::Attribute &attributes, const std::string &name) = 0;

    virtual RHIResourceRef<RHIImageView> CreateImageView(RHIImage *image, const RHIImageView::Attribute &attribute) = 0;

    virtual RHIResourceRef<RHIBLAS> CreateBLAS(const TransformMatrix &transform,
                                               const RHIResourceRef<RHIBuffer> &vertex_buffer,
                                               const RHIResourceRef<RHIBuffer> &index_buffer, uint32_t num_primitive,
                                               uint32_t num_vertex, const std::string &name) = 0;
    virtual RHIResourceRef<RHITLAS> CreateTLAS(const std::string &name) = 0;
    virtual RHIResourceRef<RHITimer> CreateTimer(const std::string &name) = 0;

    virtual RHIResourceRef<RHIComputePass> CreateComputePass(const std::string &name, bool need_timestamp) = 0;

    [[nodiscard]] RHIResourceRef<RHIRenderPass> GetCurrentRenderPass() const
    {
        return current_render_pass_;
    }

    [[nodiscard]] RHIResourceRef<RHIComputePass> GetCurrentComputePass() const
    {
        return current_compute_pass_;
    }

    [[nodiscard]] const auto &GetFrameStats(unsigned frame_index) const
    {
        return frame_stats_[frame_index];
    }

    [[nodiscard]] RHIResourceRef<RHIUiHandler> GetUiHandler()
    {
        if (!ui_handler_instance_)
        {
            ui_handler_instance_ = CreateUiHandler();
        }
        return ui_handler_instance_;
    }

    RHIResourceRef<RHISampler> GetSampler(RHISampler::SamplerAttribute attribute);

    RHIResourceRef<RHIImage> CreateTexture(const Image2D *image, const std::string &name);

    RHIResourceRef<RHIImage> CreateTextureCube(const Image2DCube *image, const std::string &name);

    void EnqueueBeforeFrameTasks(std::function<void(void)> func)
    {
        before_frame_tasks_.emplace_back(std::move(func));
    }

    void EnqueueEndOfFrameTasks(std::function<void(void)> func)
    {
        end_of_frame_tasks_.emplace_back(std::move(func));
    }

    // TODO(tqjxlm): this should not be public. restrict its use to RHI modules only.
    template <class T, typename... Args>
        requires std::derived_from<T, RHIResource>
    RHIResourceRef<T> CreateResource(Args &&...args)
    {
        auto deferred_deleter = [this](RHIResource *resource) {
            if (resource)
            {
                DeferResourceDeletion(resource);
            }
        };

        RHIResourceRef<T> resource = std::shared_ptr<T>(new T(std::forward<Args>(args)...), deferred_deleter);

#ifndef NDEBUG
        registered_resources_.emplace_back(RHIResourceWeakRef<T>(resource));
        resource->SetDebugStack(ExceptionHandler::GetStackTrace());
#endif

        return resource;
    }

    void DeferResourceDeletion(RHIResource *resource);

    void FlushDeferredDeletions();

    void EnqueueEndOfRenderTasks(std::function<void(void)> func)
    {
        end_of_render_tasks_[frame_index_].emplace_back(std::move(func));
    }

    // allocate an object on stack that is safe to use before the start of next frame
    // CAUTION: this object will not be constructed or destructed automatically
    template <class T> T *AllocateOneFrameMemory()
    {
        return frame_memory_.Allocate<T>();
    }

    RHIResourceRef<RHIImage> GetOrCreateDummyTexture(RHIImage::Attribute attribute);

#ifndef NDEBUG
    static std::unordered_set<size_t> deleted_resources_;
#endif

protected:
    virtual void BeginRenderPassInternal(const RHIResourceRef<RHIRenderPass> &pass) = 0;
    virtual void EndRenderPassInternal() = 0;
    virtual void BeginComputePassInternal(const RHIResourceRef<RHIComputePass> &pass) = 0;
    virtual void EndComputePassInternal(const RHIResourceRef<RHIComputePass> &pass) = 0;
    virtual void BeginFrameInternal() = 0;
    virtual void EndFrameInternal() = 0;
    virtual void CleanupInternal() = 0;
    virtual RHIResourceRef<RHISampler> CreateSampler(RHISampler::SamplerAttribute attribute,
                                                     const std::string &name) = 0;
    virtual RHIResourceRef<RHIShader> CreateShader(const RHIShaderInfo *shader_info) = 0;
    virtual RHIResourceRef<RHIUiHandler> CreateUiHandler() = 0;

    void CheckRHIResourceLeak();

    NativeView *view_;
    bool frame_buffer_resized_ = false;

#ifndef NDEBUG
    std::vector<RHIResourceWeakRef<RHIResource>> registered_resources_;
#endif

    RHIResourceRef<RHIRenderTarget> back_buffer_rt_;
    RHIResourceRef<RHIRenderPass> current_render_pass_;
    RHIResourceRef<RHIComputePass> current_compute_pass_;

    std::unordered_map<RHISampler::SamplerAttribute, RHIResourceRef<RHISampler>> samplers_;

    bool initialization_success_ = false;
    bool back_buffer_dirty_ = true;

    uint64_t total_frame_ = 0;

    // own as reference because rhi is not responsible to maintain config
    const RHIConfig &config_;

    struct FrameStates
    {
        float elapsed_time_ms = -1.f;
    };

    std::vector<FrameStates> frame_stats_;

private:
    unsigned max_frames_in_flight_ = 0;
    unsigned frame_index_ = 0;
    bool is_deleting_deferred_resources_ = false;

    struct DeferredDeletion
    {
        std::vector<RHIResource *> resources;
        std::unique_ptr<std::mutex> mutex;
#ifndef NDEBUG
        std::unordered_set<RHIResource *> duplication_guard;
#endif

        DeferredDeletion();

        DeferredDeletion(DeferredDeletion &&other) noexcept;

        ~DeferredDeletion();

        void DeleteResources();

        void PushResource(RHIResource *resource, bool should_lock = true);
    };

    // resources that will be held until the frame finishes rendering. it is a ring buffer.
    std::vector<DeferredDeletion> deferred_deletion_;

    // tasks that will be run when the frame finishes rendering. it is a ring buffer.
    std::vector<std::vector<std::function<void(void)>>> end_of_render_tasks_;

    std::vector<std::function<void(void)>> before_frame_tasks_;
    std::vector<std::function<void(void)>> end_of_frame_tasks_;

    std::unique_ptr<RHIBufferManager> buffer_manager_;

    std::unordered_map<uint32_t, RHIResourceRef<RHIImage>> dummy_textures_;

    StackMemoryAllocator frame_memory_;

    // we only need one instance of ui handler
    RHIResourceRef<RHIUiHandler> ui_handler_instance_;
};
} // namespace sparkle
