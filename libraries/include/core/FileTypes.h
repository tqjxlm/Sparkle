#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace sparkle
{
enum class FileType : std::uint8_t
{
    Resource, // read-only bundled files, may need platform-specific methods to read
    Internal, // read-write files, not user-visible if the platform allows for it
    External  // read-write files, user-visible
};

struct FileEntry
{
    std::string path;
    FileType type;

    FileEntry(std::string filepath, FileType file_type) : path(std::move(filepath)), type(file_type)
    {
    }

    static FileEntry Resource(const std::string &filepath)
    {
        return FileEntry(filepath, FileType::Resource);
    }

    static FileEntry Internal(const std::string &filepath)
    {
        return FileEntry(filepath, FileType::Internal);
    }

    static FileEntry External(const std::string &filepath)
    {
        return FileEntry(filepath, FileType::External);
    }
};

struct PathEntry
{
    std::string name;
    bool is_directory;
    uint64_t size; // File size in bytes (0 for directories)
};
} // namespace sparkle
