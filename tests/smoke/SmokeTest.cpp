#include "application/TestCase.h"

#include "application/AppFramework.h"

namespace sparkle
{
class SmokeTest : public TestCase
{
public:
    Result OnTick(AppFramework & /*app*/) override
    {
        return frame_ > 2 ? Result::Pass : Result::Pending;
    }
};

static TestCaseRegistrar<SmokeTest> smoke_test_registrar("smoke");
} // namespace sparkle
