#include "application/TestCase.h"

#if ENABLE_TEST_CASES

#include "application/AppConfig.h"
#include "application/AppFramework.h"
#include "core/Logger.h"

#include <map>

namespace sparkle
{

TestCase::Result TestCase::Tick(AppFramework &app)
{
    ++frame_;
    Result result = OnTick(app);

    if (result == Result::Pending)
    {
        uint32_t timeout = app.GetAppConfig().test_timeout;
        if (timeout > 0 && frame_ > timeout)
        {
            Log(Error, "Test case timed out after {} frames", timeout);
            return Result::Fail;
        }
    }

    return result;
}
namespace
{
std::map<std::string, TestCaseRegistry::Factory> &GetRegistry()
{
    static std::map<std::string, TestCaseRegistry::Factory> registry;
    return registry;
}
} // namespace

void TestCaseRegistry::Register(std::string name, Factory factory)
{
    auto [it, inserted] = GetRegistry().emplace(std::move(name), std::move(factory));
    if (!inserted)
    {
        Log(Error, "TestCase '{}' is already registered — duplicate ignored", it->first);
    }
}

std::unique_ptr<TestCase> TestCaseRegistry::Create(const std::string &name)
{
    auto &registry = GetRegistry();
    auto it = registry.find(name);
    if (it == registry.end())
    {
        Log(Error, "TestCase '{}' is not registered. Available: {}", name, [&] {
            std::string list;
            for (const auto &[k, v] : registry)
            {
                if (!list.empty()) list += ", ";
                list += k;
            }
            return list.empty() ? "(none)" : list;
        }());
        return nullptr;
    }
    return it->second();
}

} // namespace sparkle

#endif // ENABLE_TEST_CASES
