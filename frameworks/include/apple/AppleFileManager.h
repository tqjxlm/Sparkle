#pragma once

#if FRAMEWORK_APPLE

#include "io/StdFileManager.h"

namespace sparkle
{
class AppleFileManager : public StdFileManager
{
public:
    using StdFileManager::StdFileManager;

    static std::string GetResourceFilePath(const std::string &filepath);

    bool ResourceExists(const std::string &filepath) override;
    std::string GetAbosluteFilePath(const std::string &filepath, bool external) override;
    std::vector<char> ReadResource(const std::string &filepath) override;
    size_t GetResourceSize(const std::string &filepath) override;
};
} // namespace sparkle

#endif
