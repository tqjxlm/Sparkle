#include "application/TestCase.h"

#include "application/AppFramework.h"

namespace sparkle
{
class SmokeTest : public TestCase
{
public:
    Result Tick(AppFramework & /*app*/) override
    {
        if (frame_++ < 2)
        {
            return Result::Pending;
        }
        return Result::Pass;
    }

private:
    uint32_t frame_ = 0;
};

static TestCaseRegistrar<SmokeTest> smoke_test_registrar("smoke");
} // namespace sparkle
