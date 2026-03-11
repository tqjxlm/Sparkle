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

    /// Calls OnTick(), increments the frame counter, and enforces the timeout.
    Result Tick(AppFramework &app);

    /// Applies any test-specific config overrides before the rest of app init consumes them.
    void EnforceConfigs();

    [[nodiscard]] const std::string &GetName() const
    {
        return name_;
    }

protected:
    virtual void OnEnforceConfigs()
    {
    }

    virtual Result OnTick(AppFramework &app) = 0;

    /// Optional per-test timeout in frames. When the app config does not
    /// provide --test_timeout, Tick() uses this value instead.
    [[nodiscard]] virtual uint32_t GetDefaultTimeoutFrames() const
    {
        return 0;
    }

    void EnforceConfig(const std::string &config_name, bool value) const;
    void EnforceConfig(const std::string &config_name, uint32_t value) const;
    void EnforceConfig(const std::string &config_name, float value) const;
    void EnforceConfig(const std::string &config_name, const std::string &value) const;

    uint32_t frame_ = 0;

private:
    friend class TestCaseRegistry;

    void SetName(std::string name)
    {
        name_ = std::move(name);
    }

    std::string name_;
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
