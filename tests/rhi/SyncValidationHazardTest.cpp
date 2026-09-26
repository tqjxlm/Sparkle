#include "application/TestCase.h"

#if ENABLE_VULKAN

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"
#include "rhi/RHI.h"

#include "../../libraries/source/rhi/vulkan/VulkanBuffer.h"
#include "../../libraries/source/rhi/vulkan/VulkanCommandContext.h"

#include <atomic>

namespace sparkle
{
// proves the synchronization gate can fail: two copies into one buffer with no barrier between them are a
// write-after-write hazard, which synchronization validation must report. the case accepts exactly the errors it
// provoked, so any other validation error still fails it.
class SyncValidationHazardTest : public TestCase
{
public:
    void OnEnforceConfigs() override
    {
        EnforceConfig("validation", true);
        EnforceConfig("validate_sync", true);
    }

    Result OnTick(AppFramework &app) override
    {
        if (task_pending_.load(std::memory_order_acquire))
        {
            return Result::Pending;
        }

        if (started_)
        {
            const auto provoked = provoked_errors_.load(std::memory_order_acquire);
            if (provoked == 0)
            {
                Log(Error, "an unsynchronized write-after-write was not reported");
                return Result::Fail;
            }

            Log(Info, "the unsynchronized write-after-write was reported ({} error(s))", provoked);
            AcceptValidationErrors(provoked);
            return Result::Pass;
        }

        started_ = true;
        task_pending_.store(true, std::memory_order_release);

        auto *rhi = app.GetRHI();
        TaskManager::RunInRenderThread([this, rhi] {
            RecordHazard(rhi);
            task_pending_.store(false, std::memory_order_release);
        });

        return Result::Pending;
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return 1000;
    }

private:
    static constexpr size_t BufferSize = 256;

    // raw copies bypass RHICommandContext::CopyBuffer, whose barriers would order them
    void RecordHazard(RHIContext *rhi)
    {
        auto source =
            rhi->CreateBuffer({.size = BufferSize,
                               .usages = RHIBuffer::BufferUsage::TransferSrc,
                               .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
                               .is_dynamic = false},
                              "SyncValidationHazardSource");
        auto destination = rhi->CreateBuffer({.size = BufferSize,
                                              .usages = RHIBuffer::BufferUsage::TransferDst,
                                              .mem_properties = RHIMemoryProperty::DeviceLocal,
                                              .is_dynamic = false},
                                             "SyncValidationHazardDestination");

        const auto errors_before = rhi->GetValidationErrorCount().value_or(0);

        rhi->BeginCommandBuffer();
        auto *command_buffer = static_cast<VulkanCommandContext *>(rhi->GetCommandContext())->GetCommandBuffer();
        const VkBufferCopy region{.srcOffset = 0, .dstOffset = 0, .size = BufferSize};
        for (auto copy = 0; copy < 2; copy++)
        {
            vkCmdCopyBuffer(command_buffer, RHICast<VulkanBuffer>(source.get())->GetResourceThisFrame(),
                            RHICast<VulkanBuffer>(destination.get())->GetResourceThisFrame(), 1, &region);
        }
        rhi->SubmitCommandBuffer();

        provoked_errors_.store(rhi->GetValidationErrorCount().value_or(0) - errors_before, std::memory_order_release);
    }

    bool started_ = false;
    std::atomic<bool> task_pending_ = false;
    std::atomic<unsigned> provoked_errors_ = 0;
};

static TestCaseRegistrar<SyncValidationHazardTest> sync_validation_hazard_test_registrar("sync_validation_hazard");
} // namespace sparkle

#endif
