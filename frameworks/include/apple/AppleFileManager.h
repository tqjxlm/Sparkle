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

    std::filesystem::path ResolvePath(const Path &path) override;
    bool Exists(const Path &file) override;
    size_t GetSize(const Path &file) override;
    std::vector<char> Read(const Path &file) override;
    std::vector<Path> ListDirectory(const Path &dirpath) override;
};
} // namespace sparkle

#endif
