#include "application/TestCase.h"

#if ENABLE_TEST_CASES

#include "application/AppConfig.h"
#include "application/AppFramework.h"
#include "core/ConfigManager.h"
#include "core/Logger.h"
#include "core/math/Utilities.h"
#include "rhi/RHI.h"

#include <map>

namespace sparkle
{
namespace
{
template <class T> void EnforceTestConfig(const TestCase &test_case, const std::string &config_name, const T &value)
{
    auto *config = ConfigManager::Instance().GetConfig<T>(config_name);
    if (!config)
    {
        Log(Error, "Test case '{}' cannot enforce missing config '{}'", test_case.GetName(), config_name);
        return;
    }

    if (utilities::IsSame(config->Get(), value))
    {
        return;
    }

    Log(Info, "Test case '{}' enforces config {}={}", test_case.GetName(), config_name, value);
    config->Set(value);
}
} // namespace

void TestCase::EnforceConfigs()
{
    OnEnforceConfigs();
}

TestCase::Result TestCase::Tick(AppFramework &app)
{
    ++frame_;
    Result result = OnTick(app);

    // any error an active validation layer reports fails the case, and a run asking for sync validation must have it
    if (result != Result::Fail)
    {
        if (app.GetRHIConfig().enable_sync_validation && !app.GetRHI()->IsSyncValidationActive())
        {
            Log(Error, "Test case '{}' requested sync validation, but it is not active", GetName());
            return Result::Fail;
        }
        const auto error_count = app.GetRHI()->GetValidationErrorCount().value_or(0);
        if (error_count > accepted_validation_errors_)
        {
            Log(Error, "Test case '{}' got {} validation error(s)", GetName(),
                error_count - accepted_validation_errors_);
            return Result::Fail;
        }
    }

    if (result == Result::Pending)
    {
        uint32_t timeout = app.GetAppConfig().test_timeout;
        if (timeout == 0)
        {
            timeout = GetDefaultTimeoutFrames();
        }

        if (timeout > 0 && frame_ > timeout)
        {
            Log(Error, "Test case '{}' timed out after {} frames", GetName(), timeout);
            return Result::Fail;
        }
    }

    return result;
}

void TestCase::EnforceConfig(const std::string &config_name, bool value) const
{
    EnforceTestConfig(*this, config_name, value);
}

void TestCase::EnforceConfig(const std::string &config_name, uint32_t value) const
{
    EnforceTestConfig(*this, config_name, value);
}

void TestCase::EnforceConfig(const std::string &config_name, float value) const
{
    EnforceTestConfig(*this, config_name, value);
}

void TestCase::EnforceConfig(const std::string &config_name, const std::string &value) const
{
    EnforceTestConfig(*this, config_name, value);
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
                if (!list.empty())
                    list += ", ";
                list += k;
            }
            return list.empty() ? "(none)" : list;
        }());
        return nullptr;
    }
    auto test_case = it->second();
    test_case->SetName(it->first);
    return test_case;
}

} // namespace sparkle

#endif // ENABLE_TEST_CASES
