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

    std::string GetAbsoluteFilePath(const FileEntry &file) override;
    bool Exists(const FileEntry &file) override;
    size_t GetSize(const FileEntry &file) override;
    std::vector<char> Read(const FileEntry &file) override;
    std::string Write(const FileEntry &file, const char *data, uint64_t size) override;
    bool TryCreateDirectory(const FileEntry &file) override;
    std::vector<PathEntry> ListDirectory(const FileEntry &dirpath) override;

protected:
    static std::vector<char> ReadFile(const std::string &absolute_path);
};
} // namespace sparkle
