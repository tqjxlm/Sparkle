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

    std::string GetAbsoluteFilePath(const FileEntry &file) override;
    bool Exists(const FileEntry &file) override;
    size_t GetSize(const FileEntry &file) override;
    std::vector<char> Read(const FileEntry &file) override;
    std::vector<PathEntry> ListDirectory(const FileEntry &dirpath) override;
};
} // namespace sparkle

#endif
