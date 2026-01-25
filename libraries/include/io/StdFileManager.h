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

    std::filesystem::path ResolvePath(const Path &path) override;
    bool Exists(const Path &file) override;
    size_t GetSize(const Path &file) override;
    std::vector<char> Read(const Path &file) override;
    std::string Write(const Path &file, const char *data, uint64_t size) override;
    bool TryCreateDirectory(const Path &file) override;
    std::vector<Path> ListDirectory(const Path &dirpath) override;

protected:
    static std::vector<char> ReadFile(const std::string &absolute_path);
};
} // namespace sparkle
