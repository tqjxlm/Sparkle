#include "io/CookTargets.h"

#include "core/Logger.h"

#include <algorithm>

namespace sparkle
{
bool CookTargets::Contains(const std::string &target)
{
    return All().contains(target);
}

TextureCompression::Family CookTargets::FamilyOf(const std::string &target)
{
    const auto entry = All().find(target);
    ASSERT_F(entry != All().end(), "unknown cook target {}", target);
    return entry->second;
}

std::string CookTargets::Names()
{
    std::string names;
    for (const auto &[target, family] : All())
    {
        if (!names.empty())
        {
            names += ", ";
        }
        names += target;
    }
    return names;
}

const char *CookTargets::SelfTarget()
{
#if FRAMEWORK_GLFW
#if PLATFORM_MACOS
    return "macos-glfw";
#elif PLATFORM_WINDOWS
    return "windows-glfw";
#else
    return "linux-glfw";
#endif
#elif PLATFORM_MACOS
    return "macos";
#elif PLATFORM_IOS
    return "ios";
#else
    return "android";
#endif
}

TextureCompression::Family CookTargets::PlatformFamily()
{
    return FamilyOf(SelfTarget());
}

std::vector<std::string> CookTargets::Parse(const std::string &config)
{
    const std::string requested = config.empty() ? SelfTarget() : config;

    std::vector<std::string> targets;
    for (size_t begin = 0; begin <= requested.size();)
    {
        const auto end = std::min(requested.find('+', begin), requested.size());
        auto target = requested.substr(begin, end - begin);
        begin = end + 1;

        if (target.empty())
        {
            continue;
        }
        if (!Contains(target))
        {
            Log(Error, "unknown cook target '{}'. known targets: {}", target, Names());
            return {};
        }
        if (std::ranges::find(targets, target) == targets.end())
        {
            targets.push_back(std::move(target));
        }
    }

    if (targets.empty())
    {
        Log(Error, "no cook target requested");
    }

    return targets;
}
} // namespace sparkle
