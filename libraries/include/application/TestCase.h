#pragma once

#if ENABLE_TEST_CASES

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace sparkle
{
class AppFramework;

class TestCase
{
public:
    enum class Result : uint8_t
    {
        Pending,
        Pass,
        Fail
    };

    virtual ~TestCase() = default;

    virtual Result Tick(AppFramework &app) = 0;
};

class TestCaseRegistry
{
public:
    using Factory = std::function<std::unique_ptr<TestCase>()>;

    static void Register(std::string name, Factory factory);

    // Returns nullptr if name is not registered.
    static std::unique_ptr<TestCase> Create(const std::string &name);
};

template <typename T> struct TestCaseRegistrar
{
    explicit TestCaseRegistrar(std::string name)
    {
        TestCaseRegistry::Register(std::move(name), [] { return std::make_unique<T>(); });
    }
};

} // namespace sparkle

#endif // ENABLE_TEST_CASES
