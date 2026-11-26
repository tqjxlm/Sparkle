#include "application/TestCase.h"

#if ENABLE_VULKAN

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"
#include "rhi/RHI.h"

#include "../../libraries/source/rhi/vulkan/VulkanBuffer.h"
#include "../../libraries/source/rhi/vulkan/VulkanCommandContext.h"

namespace sparkle
{
// proves the synchronization gate can fail: two copies into one buffer with no barrier between them are a
// write-after-write hazard, which synchronization validation must report. the case accepts exactly the issues it
// provoked, so any other validation issue still fails it.
class SyncValidationHazardTest : public TestCase
{
public:
    void OnEnforceConfigs() override
    {
        EnforceConfig("validation", true);
        EnforceConfig("validate_sync", true);
    }

    // the hazard is provoked and accepted within one tick: an error not yet accepted when a tick ends fails the case
    Result OnTick(AppFramework &app) override
    {
        unsigned provoked = 0;
        TaskManager::RunInRenderThread([&provoked, rhi = app.GetRHI()] { provoked = RecordHazard(rhi); });
        TaskManager::FlushRenderThread();

        if (provoked == 0)
        {
            Log(Error, "an unsynchronized write-after-write was not reported");
            return Result::Fail;
        }

        Log(Info, "the unsynchronized write-after-write was reported ({} issue(s))", provoked);
        AcceptValidationIssues(provoked);
        return Result::Pass;
    }

private:
    static constexpr size_t BufferSize = 256;

    // two raw copies into one buffer, with no barrier between them. returns the validation issues they raised
    static unsigned RecordHazard(RHIContext *rhi)
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

        const auto issues_before = rhi->GetValidationIssueCount().value_or(0);

        auto *command_buffer = static_cast<VulkanCommandContext &>(rhi->BeginCommandBuffer()).GetCommandBuffer();
        const VkBufferCopy region{.srcOffset = 0, .dstOffset = 0, .size = BufferSize};
        for (auto copy = 0; copy < 2; copy++)
        {
            vkCmdCopyBuffer(command_buffer, RHICast<VulkanBuffer>(source.get())->GetResourceThisFrame(),
                            RHICast<VulkanBuffer>(destination.get())->GetResourceThisFrame(), 1, &region);
        }
        rhi->SubmitCommandBuffer();

        return rhi->GetValidationIssueCount().value_or(0) - issues_before;
    }
};

static TestCaseRegistrar<SyncValidationHazardTest> sync_validation_hazard_test_registrar("sync_validation_hazard");
} // namespace sparkle

#endif
