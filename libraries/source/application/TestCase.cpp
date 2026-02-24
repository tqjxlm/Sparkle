#include "application/TestCase.h"

#if ENABLE_TEST_CASES

#include "core/Logger.h"

#include <map>

namespace sparkle
{
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
