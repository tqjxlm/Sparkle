#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "core/Logger.h"
#include "core/task/TaskManager.h"
#include "rhi/RHI.h"

#include <atomic>

namespace sparkle
{
class DynamicBufferReuseTest : public TestCase
{
public:
    Result OnTick(AppFramework &app) override
    {
        if (task_pending_.load(std::memory_order_acquire))
        {
            return Result::Pending;
        }

        if (failed_.load(std::memory_order_acquire))
        {
            return Result::Fail;
        }

        auto *rhi = app.GetRHI();

        switch (stage_)
        {
        case Stage::ReleaseAdjacentAllocations:
            RunOnRenderThread([this, rhi] {
                const auto alignment = rhi->GetMinBufferOffsetAlignment();
                auto first = rhi->CreateBuffer(MakeAttribute(1), "DynamicBufferReuseFirst");
                auto second = rhi->CreateBuffer(MakeAttribute(1), "DynamicBufferReuseSecond");

                Expect(first->GetOffset(0) == 0, "first allocation starts at offset zero");
                Expect(second->GetOffset(0) == alignment, "second allocation respects buffer alignment");
            });

            stage_ = Stage::VerifyCoalescedReuse;
            wait_until_frame_ = frame_ + rhi->GetMaxFramesInFlight() + 2;
            return Result::Pending;

        case Stage::VerifyCoalescedReuse:
            if (frame_ < wait_until_frame_)
            {
                return Result::Pending;
            }

            RunOnRenderThread([this, rhi] {
                const auto size = rhi->GetMinBufferOffsetAlignment() + 1;
                auto combined = rhi->CreateBuffer(MakeAttribute(size), "DynamicBufferReuseCombined");

                Expect(combined->GetOffset(0) == 0, "adjacent freed ranges are coalesced and reused");
            });

            stage_ = Stage::Done;
            return Result::Pending;

        case Stage::Done:
            return Result::Pass;

        default:
            return Result::Fail;
        }
    }

    [[nodiscard]] uint32_t GetDefaultTimeoutFrames() const override
    {
        return 1000;
    }

private:
    enum class Stage : uint8_t
    {
        ReleaseAdjacentAllocations,
        VerifyCoalescedReuse,
        Done,
    };

    static RHIBuffer::Attribute MakeAttribute(size_t size)
    {
        return {
            .size = size,
            .usages = RHIBuffer::BufferUsage::TransferSrc | RHIBuffer::BufferUsage::TransferDst,
            .mem_properties = RHIMemoryProperty::HostVisible | RHIMemoryProperty::HostCoherent,
            .is_dynamic = true,
        };
    }

    template <typename Task> void RunOnRenderThread(Task &&task)
    {
        task_pending_.store(true, std::memory_order_release);
        TaskManager::RunInRenderThread([this, render_task = std::forward<Task>(task)] {
            render_task();
            task_pending_.store(false, std::memory_order_release);
        });
    }

    void Expect(bool condition, const std::string &what)
    {
        if (condition)
        {
            Log(Info, "{}: OK - {}", GetName(), what);
        }
        else
        {
            Log(Error, "{}: FAILED - {}", GetName(), what);
            failed_.store(true, std::memory_order_release);
        }
    }

    Stage stage_ = Stage::ReleaseAdjacentAllocations;
    uint32_t wait_until_frame_ = 0;
    std::atomic<bool> task_pending_{false};
    std::atomic<bool> failed_{false};
};

static TestCaseRegistrar<DynamicBufferReuseTest> dynamic_buffer_reuse_test_registrar("dynamic_buffer_reuse");
} // namespace sparkle
