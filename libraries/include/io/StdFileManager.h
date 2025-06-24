#pragma once

#include "core/FileManager.h"

namespace sparkle
{
// a general file manager where:
// 1. resource files are saved in ResourceRoot
// 2. external and internal files are both visible to users. they are located in GeneratedRoot
class StdFileManager : public FileManager
{
public:
    using FileManager::FileManager;

    bool ResourceExists(const std::string &filepath) override;

    size_t GetResourceSize(const std::string &filepath) override;

    bool FileExists(const std::string &filepath, bool external) override;

    bool TryCreateDirectory(const std::string &filepath, bool external) override;

    std::string GetAbosluteFilePath(const std::string &filepath, bool external) override;

    std::vector<char> ReadResource(const std::string &filepath) override;

    std::vector<char> ReadFile(const std::string &filepath, bool external) override;

    std::string WriteFile(const std::string &filepath, const char *data, uint64_t size, bool external) override;

protected:
    static std::vector<char> ReadFile(const std::string &absolute_path);
};
} // namespace sparkle
